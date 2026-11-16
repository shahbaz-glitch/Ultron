
#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ObserverPtr.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace
{

// -------------------------------------------------------------
// MODULE CONFIGURATION
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingChat";
constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kMessageInputId = "message-input";
constexpr const char* kSendButtonId = "send-button";
constexpr const char* kChatAreaId = "chat-area";

constexpr Uint32 kLongPressMilliseconds = 550;


// -------------------------------------------------------------
// SHARED STATE
// -------------------------------------------------------------

Rml::ElementDocument* g_active_document = nullptr;

Rml::ObserverPtr<Rml::Element> g_pending_textarea;

bool g_text_refresh_queued = false;
bool g_first_line_shifted = false;

Rml::ElementPtr g_editing_bubble;
Rml::ElementDocument* g_editing_document = nullptr;


struct PressState
{
    bool active = false;
    bool expired = false;
    bool is_touch = false;

    std::uint64_t generation = 0;

    SDL_TouchID touch_id = 0;
    SDL_FingerID finger_id = 0;

    float start_x = 0.0f;
    float start_y = 0.0f;

    Rml::ObserverPtr<Rml::Element> document;
    Rml::ObserverPtr<Rml::Element> bubble;
};

std::mutex g_press_mutex;
PressState g_press;


struct LongPressTicket
{
    std::uint64_t generation = 0;
};


// -------------------------------------------------------------
// HELPERS
// -------------------------------------------------------------

Rml::String ToPixels(float value)
{
    char buffer[48]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3fpx",
        value
    );

    return Rml::String(buffer);
}


Rml::String TrimMessage(const Rml::String& value)
{
    const auto is_space = [](unsigned char character)
    {
        return std::isspace(character) != 0;
    };

    auto first = std::find_if_not(
        value.begin(),
        value.end(),
        is_space
    );

    auto last = std::find_if_not(
        value.rbegin(),
        value.rend(),
        is_space
    ).base();

    if (first >= last)
    {
        return {};
    }

    return Rml::String(first, last);
}


bool HasMeaningfulText(const Rml::String& value)
{
    return std::any_of(
        value.begin(),
        value.end(),
        [](unsigned char character)
        {
            return std::isspace(character) == 0;
        }
    );
}


Rml::String EscapeRmlText(const Rml::String& text)
{
    Rml::String escaped;
    escaped.reserve(text.size());

    for (const char character : text)
    {
        switch (character)
        {
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default:  escaped += character; break;
        }
    }

    return escaped;
}


Rml::Element* FindAncestorById(
    Rml::Element* element,
    const char* id)
{
    while (element != nullptr)
    {
        if (element->GetId() == id)
        {
            return element;
        }

        element = element->GetParentNode();
    }

    return nullptr;
}


Rml::Element* FindChatBubble(Rml::Element* element)
{
    while (element != nullptr)
    {
        if (element->IsClassSet("chat-message"))
        {
            return element;
        }

        element = element->GetParentNode();
    }

    return nullptr;
}


bool ContainsPoint(
    Rml::Element* element,
    float x,
    float y)
{
    if (
        element == nullptr ||
        element->GetOffsetWidth() <= 0.0f ||
        element->GetOffsetHeight() <= 0.0f
    )
    {
        return false;
    }

    const float left = element->GetAbsoluteLeft();
    const float top = element->GetAbsoluteTop();

    const float right =
        left + element->GetOffsetWidth();

    const float bottom =
        top + element->GetOffsetHeight();

    return (
        x >= left &&
        x <= right &&
        y >= top &&
        y <= bottom
    );
}


// -------------------------------------------------------------
// RESPONSIVE INPUT UPDATE
// -------------------------------------------------------------

void UpdateInput(Rml::Element* textarea_element)
{
    if (
        textarea_element == nullptr ||
        textarea_element->GetId() != kMessageInputId ||
        textarea_element->GetTagName() != "textarea"
    )
    {
        return;
    }

    Rml::Element* input_bar =
        textarea_element->GetParentNode();

    if (
        input_bar == nullptr ||
        input_bar->GetId() != kInputBarId
    )
    {
        return;
    }

    auto* message_input =
        dynamic_cast<Rml::ElementFormControlTextArea*>(
            textarea_element
        );

    if (message_input == nullptr)
    {
        return;
    }

    Rml::ElementDocument* document =
        textarea_element->GetOwnerDocument();

    if (document == nullptr)
    {
        return;
    }

    Rml::Element* wing_hit =
        document->GetElementById("wing-hit");

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (
        wing_hit == nullptr ||
        chat_area == nullptr
    )
    {
        return;
    }

    g_active_document = document;

    const Rml::String message =
        message_input->GetValue();

    const float control_height =
        wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
    {
        return;
    }

    const float padding_height =
        control_height * (1.23f / 14.22f);

    const float max_text_height =
        control_height * (30.15f / 14.22f);

    const float extra_gap =
        control_height * (1.5f / 14.22f);

    float bar_height = control_height;


    // EMPTY INPUT

    if (message.empty())
    {
        g_first_line_shifted = false;

        input_bar->SetClass("multiline", false);

        message_input->RemoveProperty("height");
    }

    else
    {
        // FIRST LINE DETECTION

        if (!g_first_line_shifted)
        {
            document->UpdateDocument();

            if (
                message_input->GetScrollHeight() >
                message_input->GetClientHeight() + 1.0f
            )
            {
                g_first_line_shifted = true;

                input_bar->SetClass("multiline", true);
            }
        }


        // MULTILINE LAYOUT

        if (g_first_line_shifted)
        {
            message_input->SetProperty(
                "height",
                "0px"
            );

            document->UpdateDocument();

            const float content_height =
                message_input->GetScrollHeight();

            const float new_height = std::min(
                content_height,
                max_text_height
            );

            message_input->SetProperty(
                "height",
                ToPixels(
                    std::max(
                        0.0f,
                        new_height - padding_height
                    )
                )
            );

            bar_height =
                new_height +
                control_height +
                extra_gap;
        }

        else
        {
            message_input->RemoveProperty("height");

            bar_height = control_height;
        }
    }


    // INPUT BAR HEIGHT

    input_bar->SetProperty(
        "height",
        ToPixels(bar_height)
    );


    // CHAT AREA POSITION

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / 14.22f);

    chat_area->SetProperty(
        "bottom",
        ToPixels(chat_bottom)
    );


    // PULSE / ARROW

    input_bar->SetClass(
        "has-text",
        HasMeaningfulText(message)
    );
}


// -------------------------------------------------------------
// DEFERRED TEXT UPDATE
// -------------------------------------------------------------

void ApplyPendingTextRefresh(void*)
{
    g_text_refresh_queued = false;

    const Rml::ObserverPtr<Rml::Element> observer =
        g_pending_textarea;

    if (!observer)
    {
        return;
    }

    UpdateInput(observer.operator->());
}


void QueueTextRefresh(Rml::Element* target)
{
    if (
        target == nullptr ||
        target->GetId() != kMessageInputId ||
        target->GetTagName() != "textarea"
    )
    {
        return;
    }

    g_pending_textarea = target->GetObserverPtr();

    if (g_text_refresh_queued)
    {
        return;
    }

    g_text_refresh_queued = true;

    if (!Ultron_PostToUI(
            &ApplyPendingTextRefresh,
            nullptr
        ))
    {
        g_text_refresh_queued = false;
    }
}


void SendMessage(Rml::ElementDocument* document);


// -------------------------------------------------------------
// EXPAND / COLLAPSE
//
// Direct target + parent validation.
// Same working logic supplied by you.
// -------------------------------------------------------------

bool HandleWingClick(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* target =
        event->GetTargetElement();

    if (target == nullptr)
    {
        return false;
    }


    // WING HIT AREA

    if (target->GetId() == kWingHitId)
    {
        Rml::Element* input_bar =
            target->GetParentNode();

        if (
            input_bar == nullptr ||
            input_bar->GetId() != kInputBarId
        )
        {
            return false;
        }

        g_active_document =
            target->GetOwnerDocument();

        const bool currently_expanded =
            input_bar->IsClassSet("expanded");

        input_bar->SetClass(
            "expanded",
            !currently_expanded
        );


        // Preserve original JavaScript focus behaviour.

        if (
            !currently_expanded &&
            g_active_document != nullptr
        )
        {
            auto* textarea =
                dynamic_cast<Rml::ElementFormControlTextArea*>(
                    g_active_document->GetElementById(
                        kMessageInputId
                    )
                );

            if (textarea != nullptr)
            {
                textarea->Focus();
            }
        }

        // Important: do not consume the click.
        return false;
    }


    // SEND BUTTON

    Rml::ElementDocument* document =
        target->GetOwnerDocument();

    if (
        document != nullptr &&
        FindAncestorById(target, kSendButtonId) != nullptr
    )
    {
        g_active_document = document;

        SendMessage(document);
    }

    return false;
}


// -------------------------------------------------------------
// LONG-PRESS EDIT
// -------------------------------------------------------------

void StartEdit(
    Rml::ElementDocument* document,
    Rml::Element* bubble)
{
    if (
        document == nullptr ||
        bubble == nullptr
    )
    {
        return;
    }

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    auto* message_input =
        dynamic_cast<Rml::ElementFormControlTextArea*>(
            document->GetElementById(kMessageInputId)
        );

    if (
        chat_area == nullptr ||
        message_input == nullptr ||
        bubble->GetParentNode() != chat_area
    )
    {
        return;
    }


    // DELETE ALL FOLLOWING MESSAGES

    Rml::Element* next =
        bubble->GetNextSibling();

    while (next != nullptr)
    {
        Rml::Element* following =
            next->GetNextSibling();

        chat_area->RemoveChild(next);

        next = following;
    }


    // READ SELECTED MESSAGE

    Rml::String message;

    auto* text_node =
        dynamic_cast<Rml::ElementText*>(
            bubble->GetFirstChild()
        );

    if (text_node != nullptr)
    {
        message = text_node->GetText();
    }


    // REMOVE SELECTED BUBBLE TEMPORARILY

    g_editing_bubble =
        chat_area->RemoveChild(bubble);

    if (!g_editing_bubble)
    {
        g_editing_document = nullptr;
        return;
    }

    g_editing_document = document;
    g_active_document = document;

    g_first_line_shifted = false;

    message_input->SetValue(message);


    // EXPAND INPUT BAR

    Rml::Element* input_bar =
        document->GetElementById(kInputBarId);

    if (input_bar != nullptr)
    {
        input_bar->SetClass("expanded", true);
    }

    UpdateInput(message_input);

    message_input->Focus();
}


// -------------------------------------------------------------
// LONG-PRESS STATE
// -------------------------------------------------------------

struct PressState
{
    bool active = false;
    bool expired = false;
    bool is_touch = false;

    std::uint64_t generation = 0;

    SDL_TouchID touch_id = 0;
    SDL_FingerID finger_id = 0;

    float start_x = 0.0f;
    float start_y = 0.0f;

    Rml::ObserverPtr<Rml::Element> document;
    Rml::ObserverPtr<Rml::Element> bubble;
};

std::mutex g_press_mutex;
PressState g_press;


struct LongPressTicket
{
    std::uint64_t generation = 0;
};


// -------------------------------------------------------------
// CANCEL PRESS
// -------------------------------------------------------------

void CancelPress(bool force = false)
{
    std::lock_guard lock(g_press_mutex);

    if (!g_press.active)
    {
        return;
    }

    if (g_press.expired && !force)
    {
        return;
    }

    g_press.active = false;

    g_press.document = nullptr;
    g_press.bubble = nullptr;

    ++g_press.generation;
}


// -------------------------------------------------------------
// EXECUTE LONG PRESS ON UI THREAD
// -------------------------------------------------------------

void ExecuteLongPressOnUI(void* user_data)
{
    std::unique_ptr<LongPressTicket> ticket(
        static_cast<LongPressTicket*>(user_data)
    );

    if (!ticket)
    {
        return;
    }

    Rml::ObserverPtr<Rml::Element> document_observer;
    Rml::ObserverPtr<Rml::Element> bubble_observer;


    {
        std::lock_guard lock(g_press_mutex);

        if (
            !g_press.active ||
            !g_press.expired ||
            g_press.generation != ticket->generation
        )
        {
            return;
        }

        document_observer = g_press.document;
        bubble_observer = g_press.bubble;

        g_press.active = false;

        g_press.document = nullptr;
        g_press.bubble = nullptr;
    }


    if (
        !document_observer ||
        !bubble_observer
    )
    {
        return;
    }

    auto* document =
        dynamic_cast<Rml::ElementDocument*>(
            document_observer.operator->()
        );

    if (document != nullptr)
    {
        StartEdit(
            document,
            bubble_observer.operator->()
        );
    }
}


// -------------------------------------------------------------
// SDL 550ms TIMER
// -------------------------------------------------------------

Uint32 SDLCALL OnLongPressTimer(
    void* user_data,
    SDL_TimerID,
    Uint32)
{
    auto* ticket =
        static_cast<LongPressTicket*>(user_data);

    if (ticket == nullptr)
    {
        return 0;
    }

    bool valid = false;

    {
        std::lock_guard lock(g_press_mutex);

        valid =
            g_press.active &&
            !g_press.expired &&
            g_press.generation == ticket->generation;

        if (valid)
        {
            g_press.expired = true;
        }
    }

    if (!valid)
    {
        delete ticket;
        return 0;
    }

    if (!Ultron_PostToUI(
            &ExecuteLongPressOnUI,
            ticket
        ))
    {
        {
            std::lock_guard lock(g_press_mutex);

            if (g_press.generation == ticket->generation)
            {
                g_press.active = false;

                g_press.document = nullptr;
                g_press.bubble = nullptr;
            }
        }

        delete ticket;
    }

    return 0;
}


// -------------------------------------------------------------
// START LONG PRESS
// -------------------------------------------------------------

void BeginLongPress(
    Rml::ElementDocument* document,
    Rml::Element* bubble,
    bool is_touch,
    SDL_TouchID touch_id = 0,
    SDL_FingerID finger_id = 0,
    float start_x = 0.0f,
    float start_y = 0.0f)
{
    if (
        document == nullptr ||
        bubble == nullptr
    )
    {
        return;
    }

    CancelPress(true);

    auto* ticket =
        new (std::nothrow) LongPressTicket;

    if (ticket == nullptr)
    {
        return;
    }

    {
        std::lock_guard lock(g_press_mutex);

        ++g_press.generation;

        g_press.active = true;
        g_press.expired = false;
        g_press.is_touch = is_touch;

        g_press.touch_id = touch_id;
        g_press.finger_id = finger_id;

        g_press.start_x = start_x;
        g_press.start_y = start_y;

        g_press.document = document->GetObserverPtr();
        g_press.bubble = bubble->GetObserverPtr();

        ticket->generation = g_press.generation;
    }

    if (SDL_AddTimer(
            kLongPressMilliseconds,
            &OnLongPressTimer,
            ticket
        ) == 0)
    {
        delete ticket;
        CancelPress(true);
    }
}


// -------------------------------------------------------------
// SEND MESSAGE
// -------------------------------------------------------------

void SendMessage(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return;
    }

    auto* message_input =
        dynamic_cast<Rml::ElementFormControlTextArea*>(
            document->GetElementById(kMessageInputId)
        );

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (
        message_input == nullptr ||
        chat_area == nullptr
    )
    {
        return;
    }

    const Rml::String message =
        TrimMessage(message_input->GetValue());

    if (message.empty())
    {
        return;
    }


    // EDIT EXISTING MESSAGE

    if (
        g_editing_bubble &&
        g_editing_document == document
    )
    {
        g_editing_bubble->SetInnerRML(
            EscapeRmlText(message)
        );

        chat_area->AppendChild(
            std::move(g_editing_bubble)
        );

        g_editing_document = nullptr;
    }

    // CREATE NEW MESSAGE

    else
    {
        g_editing_bubble.reset();
        g_editing_document = nullptr;

        Rml::ElementPtr bubble =
            document->CreateElement("div");

        if (!bubble)
        {
            return;
        }

        bubble->SetClass("chat-message", true);

        bubble->SetInnerRML(
            EscapeRmlText(message)
        );

        chat_area->AppendChild(
            std::move(bubble)
        );
    }


    // RESET INPUT

    message_input->SetValue("");

    g_first_line_shifted = false;
    g_active_document = document;

    UpdateInput(message_input);


    // SCROLL TO LATEST MESSAGE

    document->UpdateDocument();

    chat_area->SetScrollTop(
        chat_area->GetScrollHeight()
    );
}


// -------------------------------------------------------------
// TEXT INPUT EVENT
// -------------------------------------------------------------

bool HandleTextInput(
    Rml::Event* event,
    void*)
{
    if (event != nullptr)
    {
        QueueTextRefresh(
            event->GetTargetElement()
        );
    }

    return false;
}


// -------------------------------------------------------------
// KEYBOARD EVENT
// -------------------------------------------------------------

bool HandleKeyDown(
    Rml::Event* event,
    void*)
{
    if (event != nullptr)
    {
        QueueTextRefresh(
            event->GetTargetElement()
        );
    }

    // Do not consume Enter. Native textarea inserts a new line.
    return false;
}


// -------------------------------------------------------------
// CHANGE EVENT
// -------------------------------------------------------------

bool HandleChange(
    Rml::Event* event,
    void*)
{
    if (event != nullptr)
    {
        QueueTextRefresh(
            event->GetTargetElement()
        );
    }

    return false;
}


// -------------------------------------------------------------
// MOUSE DOWN — LONG PRESS
// -------------------------------------------------------------

bool HandleMouseDown(
    Rml::Event* event,
    void*)
{
    if (
        event == nullptr ||
        event->GetParameter<int>("button", 0) != 0
    )
    {
        return false;
    }

    Rml::Element* target =
        event->GetTargetElement();

    Rml::Element* bubble =
        FindChatBubble(target);

    if (bubble != nullptr)
    {
        Rml::ElementDocument* document =
            bubble->GetOwnerDocument();

        if (document != nullptr)
        {
            g_active_document = document;

            BeginLongPress(
                document,
                bubble,
                false
            );
        }
    }

    return false;
}


// -------------------------------------------------------------
// MOUSE UP — CANCEL LONG PRESS
// -------------------------------------------------------------

bool HandleMouseUp(
    Rml::Event*,
    void*)
{
    CancelPress();
    return false;
}


// -------------------------------------------------------------
// MOUSE OUT — CANCEL LONG PRESS
// -------------------------------------------------------------

bool HandleMouseOut(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* bubble =
        FindChatBubble(event->GetTargetElement());

    if (bubble != nullptr)
    {
        bool cancel = false;

        {
            std::lock_guard lock(g_press_mutex);

            cancel =
                g_press.active &&
                !g_press.is_touch &&
                g_press.bubble.operator->() == bubble;
        }

        if (cancel)
        {
            CancelPress();
        }
    }

    return false;
}


// -------------------------------------------------------------
// DOCUMENT SHOW / HIDE
// -------------------------------------------------------------

bool HandleDocumentVisibility(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* target =
        event->GetTargetElement();

    auto* document =
        dynamic_cast<Rml::ElementDocument*>(target);

    if (document == nullptr && target != nullptr)
    {
        document = target->GetOwnerDocument();
    }

    if (document == nullptr)
    {
        return false;
    }


    if (event->GetType() == "show")
    {
        g_active_document = document;

        auto* textarea =
            dynamic_cast<Rml::ElementFormControlTextArea*>(
                document->GetElementById(kMessageInputId)
            );

        if (textarea != nullptr)
        {
            UpdateInput(textarea);
        }
    }

    else if (
        event->GetType() == "hide" &&
        document == g_active_document
    )
    {
        CancelPress(true);

        g_active_document = nullptr;

        g_pending_textarea = nullptr;
        g_text_refresh_queued = false;

        g_first_line_shifted = false;

        g_editing_bubble.reset();
        g_editing_document = nullptr;
    }

    return false;
}


// -------------------------------------------------------------
// SDL TOUCH — LONG PRESS OBSERVATION ONLY
//
// Always returns false.
// main.cpp retains normal RmlUi touch processing.
// -------------------------------------------------------------

bool HandleTouchEvent(
    const SDL_Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    switch (event->type)
    {
        // -----------------------------------------
        // FINGER DOWN
        // -----------------------------------------

        case SDL_EVENT_FINGER_DOWN:
        {
            Rml::ElementDocument* document =
                g_active_document;

            if (
                document == nullptr ||
                document->GetContext() == nullptr
            )
            {
                return false;
            }

            const Rml::Vector2i dimensions =
                document->GetContext()->GetDimensions();

            const float x =
                event->tfinger.x *
                static_cast<float>(dimensions.x);

            const float y =
                event->tfinger.y *
                static_cast<float>(dimensions.y);

            Rml::Element* chat_area =
                document->GetElementById(kChatAreaId);

            if (chat_area == nullptr)
            {
                return false;
            }

            for (
                Rml::Element* bubble = chat_area->GetFirstChild();
                bubble != nullptr;
                bubble = bubble->GetNextSibling()
            )
            {
                if (
                    bubble->IsClassSet("chat-message") &&
                    ContainsPoint(bubble, x, y)
                )
                {
                    BeginLongPress(
                        document,
                        bubble,
                        true,
                        event->tfinger.touchID,
                        event->tfinger.fingerID,
                        x,
                        y
                    );

                    break;
                }
            }

            break;
        }


        // -----------------------------------------
        // FINGER MOTION
        // -----------------------------------------

        case SDL_EVENT_FINGER_MOTION:
        {
            bool matching_touch = false;

            float start_x = 0.0f;
            float start_y = 0.0f;

            {
                std::lock_guard lock(g_press_mutex);

                matching_touch =
                    g_press.active &&
                    g_press.is_touch &&
                    g_press.touch_id == event->tfinger.touchID &&
                    g_press.finger_id == event->tfinger.fingerID;

                start_x = g_press.start_x;
                start_y = g_press.start_y;
            }

            if (
                matching_touch &&
                g_active_document != nullptr &&
                g_active_document->GetContext() != nullptr
            )
            {
                const Rml::Vector2i dimensions =
                    g_active_document->GetContext()->GetDimensions();

                const float x =
                    event->tfinger.x *
                    static_cast<float>(dimensions.x);

                const float y =
                    event->tfinger.y *
                    static_cast<float>(dimensions.y);

                const float dx = x - start_x;
                const float dy = y - start_y;

                // Cancel when the finger moves more than 10 pixels.
                if ((dx * dx + dy * dy) > 100.0f)
                {
                    CancelPress();
                }
            }

            break;
        }


        // -----------------------------------------
        // FINGER UP / CANCEL
        // -----------------------------------------

        case SDL_EVENT_FINGER_UP:
        case SDL_EVENT_FINGER_CANCELED:
        {
            bool matching_touch = false;

            {
                std::lock_guard lock(g_press_mutex);

                matching_touch =
                    g_press.active &&
                    g_press.is_touch &&
                    g_press.touch_id == event->tfinger.touchID &&
                    g_press.finger_id == event->tfinger.fingerID;
            }

            if (matching_touch)
            {
                CancelPress(
                    event->type == SDL_EVENT_FINGER_CANCELED
                );
            }

            break;
        }

        default:
            break;
    }

    // Do not consume the event.
    return false;
}


// -------------------------------------------------------------
// AUTOMATIC REGISTRATION
//
// Same three-argument API registration pattern
// as your working expand/collapse code.
// -------------------------------------------------------------

[[maybe_unused]] const bool g_handlers_registered = []()
{
    const bool click_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "click",
            &HandleWingClick
        );

    const bool textinput_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "textinput",
            &HandleTextInput
        );

    const bool keydown_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "keydown",
            &HandleKeyDown
        );

    const bool change_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "change",
            &HandleChange
        );

    const bool mousedown_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "mousedown",
            &HandleMouseDown
        );

    const bool mouseup_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "mouseup",
            &HandleMouseUp
        );

    const bool mouseout_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "mouseout",
            &HandleMouseOut
        );

    const bool show_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "show",
            &HandleDocumentVisibility
        );

    const bool hide_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "hide",
            &HandleDocumentVisibility
        );

    const bool finger_down_registered =
        Ultron_RegisterEventHandler(
            kModuleName,
            SDL_EVENT_FINGER_DOWN,
            &HandleTouchEvent
        );

    const bool finger_motion_registered =
        Ultron_RegisterEventHandler(
            kModuleName,
            SDL_EVENT_FINGER_MOTION,
            &HandleTouchEvent
        );

    const bool finger_up_registered =
        Ultron_RegisterEventHandler(
            kModuleName,
            SDL_EVENT_FINGER_UP,
            &HandleTouchEvent
        );

    const bool finger_cancelled_registered =
        Ultron_RegisterEventHandler(
            kModuleName,
            SDL_EVENT_FINGER_CANCELED,
            &HandleTouchEvent
        );

    return
        click_registered &&
        textinput_registered &&
        keydown_registered &&
        change_registered &&
        mousedown_registered &&
        mouseup_registered &&
        mouseout_registered &&
        show_registered &&
        hide_registered &&
        finger_down_registered &&
        finger_motion_registered &&
        finger_up_registered &&
        finger_cancelled_registered;

}();

} // namespace
