
#include "ultron_api.h"

#include <SDL3/SDL.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{

// =============================================================
// CONFIGURATION
// =============================================================

constexpr const char* kModuleName = "ObsidianWingChat";

constexpr Uint64 kLongPressDuration = 550;

// One unit = 1% of the shorter viewport dimension.
// Portrait: vw
// Landscape: vh

constexpr float kInitialInputUnits = 7.5f;
constexpr float kMaximumInputUnits = 30.0f;

constexpr float kMinimumBarUnits = 15.0f;

// Input bar bottom (RML: 3.5vw / 3.5vh).
// Gap between input bar and chat area: 4.1 units.
constexpr float kChatBottomUnits = 7.6f;

// Ignore small finger movement caused by natural hand jitter.
constexpr float kTouchMovementThresholdUnits = 1.0f;


// =============================================================
// ENUMS
// =============================================================

enum class PressSource
{
    None,
    Mouse,
    Touch
};

enum class ActionType
{
    ToggleInput,
    SendMessage,
    StartEdit
};

enum class TouchTargetKind
{
    None,
    Wing,
    Send,
    Bubble,
    Other
};


// =============================================================
// PENDING ACTION
// =============================================================

struct PendingAction
{
    ActionType type = ActionType::SendMessage;

    Rml::ElementDocument* document = nullptr;

    Rml::Element* bubble = nullptr;
};


// =============================================================
// LONG-PRESS TICKET
// =============================================================

struct LongPressTicket
{
    std::uint64_t generation = 0;
};


// =============================================================
// MODULE STATE
// =============================================================

struct ChatState
{
    Rml::ElementDocument* document = nullptr;

    Rml::ElementPtr editing_bubble;

    std::unordered_map<Rml::Element*, std::string>
        bubble_messages;

    std::vector<PendingAction> pending_actions;

    bool action_task_pending = false;

    bool update_task_pending = false;

    Rml::ElementDocument* pending_update_document = nullptr;


    // Long-press state.

    std::atomic_uint64_t press_generation{0};

    bool press_active = false;

    PressSource press_source = PressSource::None;

    Rml::Element* press_target = nullptr;

    Uint64 press_started_at = 0;


    // Native touch observation.
    // RmlUi's SDL backend handles actual touch scrolling.

    bool touch_active = false;

    bool touch_moved = false;

    SDL_TouchID touch_id = 0;

    SDL_FingerID finger_id = 0;

    TouchTargetKind touch_start_kind = TouchTargetKind::None;

    float touch_start_x = 0.0f;

    float touch_start_y = 0.0f;
};


ChatState g_state;


// =============================================================
// LONG-PRESS WORKER
// =============================================================

std::jthread g_long_press_worker;


// =============================================================
// FORWARD DECLARATIONS
// =============================================================

void ProcessPendingActions(void* user_data);

void UpdateInputTask(void* user_data);

void LongPressTimeoutTask(void* user_data);


// =============================================================
// ELEMENT HELPERS
// =============================================================

Rml::Element* GetElement(
    Rml::ElementDocument* document,
    const char* id)
{
    if (document == nullptr || id == nullptr)
    {
        return nullptr;
    }

    return document->GetElementById(id);
}


Rml::ElementFormControlTextArea* GetMessageInput(
    Rml::ElementDocument* document)
{
    Rml::Element* element =
        GetElement(document, "message-input");

    if (element == nullptr)
    {
        return nullptr;
    }

    return static_cast<
        Rml::ElementFormControlTextArea*
    >(element);
}


bool IsChatDocument(Rml::ElementDocument* document)
{
    return
        GetElement(document, "input-bar") != nullptr &&
        GetElement(document, "wing-hit") != nullptr &&
        GetElement(document, "message-input") != nullptr &&
        GetElement(document, "send-button") != nullptr &&
        GetElement(document, "chat-area") != nullptr;
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


Rml::Element* FindMessageBubble(
    Rml::Element* element,
    Rml::ElementDocument* document)
{
    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr)
    {
        return nullptr;
    }

    while (element != nullptr && element != chat_area)
    {
        if (element->IsClassSet("chat-message") &&
            element->GetParentNode() == chat_area)
        {
            return element;
        }

        element = element->GetParentNode();
    }

    return nullptr;
}


Rml::ElementDocument* GetDocumentFromEvent(
    Rml::Event* event)
{
    if (event == nullptr)
    {
        return nullptr;
    }

    Rml::Element* target =
        event->GetTargetElement();

    return target != nullptr
        ? target->GetOwnerDocument()
        : nullptr;
}


// =============================================================
// UTF-8 WHITESPACE
// =============================================================

bool IsUnicodeWhitespace(std::uint32_t codepoint)
{
    return
        (codepoint >= 0x0009 && codepoint <= 0x000D) ||
        codepoint == 0x0020 ||
        codepoint == 0x00A0 ||
        codepoint == 0x1680 ||
        (codepoint >= 0x2000 && codepoint <= 0x200A) ||
        codepoint == 0x2028 ||
        codepoint == 0x2029 ||
        codepoint == 0x202F ||
        codepoint == 0x205F ||
        codepoint == 0x3000 ||
        codepoint == 0xFEFF;
}


std::uint32_t DecodeUtf8(
    const std::string& value,
    std::size_t position,
    std::size_t& length)
{
    if (position >= value.size())
    {
        length = 0;
        return 0;
    }

    const auto first =
        static_cast<unsigned char>(value[position]);

    if (first < 0x80)
    {
        length = 1;
        return first;
    }

    if ((first & 0xE0) == 0xC0 &&
        position + 1 < value.size())
    {
        length = 2;

        return
            ((first & 0x1F) << 6) |
            (static_cast<unsigned char>(
                value[position + 1]) & 0x3F);
    }

    if ((first & 0xF0) == 0xE0 &&
        position + 2 < value.size())
    {
        length = 3;

        return
            ((first & 0x0F) << 12) |
            ((static_cast<unsigned char>(
                value[position + 1]) & 0x3F) << 6) |
            (static_cast<unsigned char>(
                value[position + 2]) & 0x3F);
    }

    if ((first & 0xF8) == 0xF0 &&
        position + 3 < value.size())
    {
        length = 4;

        return
            ((first & 0x07) << 18) |
            ((static_cast<unsigned char>(
                value[position + 1]) & 0x3F) << 12) |
            ((static_cast<unsigned char>(
                value[position + 2]) & 0x3F) << 6) |
            (static_cast<unsigned char>(
                value[position + 3]) & 0x3F);
    }

    length = 1;
    return first;
}


std::string TrimWhitespace(std::string value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();

    while (begin < end)
    {
        std::size_t length = 1;

        const auto codepoint =
            DecodeUtf8(value, begin, length);

        if (!IsUnicodeWhitespace(codepoint))
        {
            break;
        }

        begin += length;
    }

    while (end > begin)
    {
        std::size_t position = end - 1;

        while (position > begin &&
            (static_cast<unsigned char>(
                value[position]) & 0xC0) == 0x80)
        {
            --position;
        }

        std::size_t length = 1;

        const auto codepoint =
            DecodeUtf8(value, position, length);

        if (!IsUnicodeWhitespace(codepoint))
        {
            break;
        }

        end = position;
    }

    return value.substr(begin, end - begin);
}


// =============================================================
// CSS HELPERS
// =============================================================

Rml::String ToPixels(float value)
{
    char buffer[64]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3fpx",
        static_cast<double>(value));

    return buffer;
}


// =============================================================
// RESPONSIVE VIEWPORT UNIT
// =============================================================

float GetViewportUnit(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return 0.0f;
    }

    Rml::Context* context =
        document->GetContext();

    if (context == nullptr)
    {
        return 0.0f;
    }

    const Rml::Vector2i dimensions =
        context->GetDimensions();

    const int shorter_side = std::min(
        dimensions.x,
        dimensions.y);

    if (shorter_side <= 0)
    {
        return 0.0f;
    }

    return static_cast<float>(shorter_side) / 100.0f;
}


// =============================================================
// INPUT HEIGHT AND CHAT POSITION
// =============================================================

void UpdateInput(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return;
    }

    auto* message_input =
        GetMessageInput(document);

    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (message_input == nullptr ||
        input_bar == nullptr ||
        chat_area == nullptr)
    {
        return;
    }

    const float unit =
        GetViewportUnit(document);

    if (unit <= 0.0f)
    {
        return;
    }

    const float initial_height =
        kInitialInputUnits * unit;

    const float maximum_height =
        kMaximumInputUnits * unit;

    const float minimum_bar_height =
        kMinimumBarUnits * unit;

    const float chat_bottom_offset =
        kChatBottomUnits * unit;


    // ---------------------------------------------------------
    // MEASURE TEXTAREA CONTENT
    // ---------------------------------------------------------

    message_input->SetProperty(
        "height",
        ToPixels(initial_height));

    document->UpdateDocument();

    float content_height =
        message_input->GetScrollHeight();

    content_height = std::max(
        content_height,
        initial_height);

    // Ignore tiny measurement differences caused by padding.
    // A single-line input must not increase the bar height.

    const float measurement_tolerance =
        0.75f * unit;

    if (content_height <=
        initial_height + measurement_tolerance)
    {
        content_height = initial_height;
    }

    const float new_height = std::clamp(
        content_height,
        initial_height,
        maximum_height);


    // ---------------------------------------------------------
    // APPLY TEXTAREA HEIGHT
    // ---------------------------------------------------------

    message_input->SetProperty(
        "height",
        ToPixels(new_height));

    message_input->SetProperty(
        "margin-top",
        ToPixels(-new_height * 0.5f));


    // ---------------------------------------------------------
    // INPUT BAR HEIGHT
    // ---------------------------------------------------------

    const bool expanded =
        input_bar->IsClassSet("expanded");

    // Horizontal expansion alone never increases height.
    //
    // The panel grows only when the textarea actually needs
    // additional vertical space.

    const float additional_height =
        expanded
            ? std::max(0.0f, new_height - initial_height)
            : 0.0f;

    const float bar_height =
        minimum_bar_height + additional_height;

    input_bar->SetProperty(
        "height",
        ToPixels(bar_height));


    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    chat_area->SetProperty(
        "bottom",
        ToPixels(bar_height + chat_bottom_offset));


    // ---------------------------------------------------------
    // PULSE / ARROW
    // ---------------------------------------------------------

    const bool has_text =
        !TrimWhitespace(
            message_input->GetValue()).empty();

    input_bar->SetClass(
        "has-text",
        has_text);
}


// =============================================================
// DEFERRED INPUT UPDATE
// =============================================================

void UpdateInputTask(void*)
{
    g_state.update_task_pending = false;

    Rml::ElementDocument* document =
        g_state.pending_update_document;

    g_state.pending_update_document = nullptr;

    if (IsChatDocument(document) &&
        document->IsVisible())
    {
        UpdateInput(document);
    }
}


void QueueInputUpdate(Rml::ElementDocument* document)
{
    if (!IsChatDocument(document))
    {
        return;
    }

    g_state.pending_update_document = document;

    if (g_state.update_task_pending)
    {
        return;
    }

    g_state.update_task_pending = true;

    if (!Ultron_PostToUI(
            &UpdateInputTask,
            nullptr))
    {
        g_state.update_task_pending = false;
        g_state.pending_update_document = nullptr;
    }
}


// =============================================================
// ACTION QUEUE
// =============================================================

void QueueAction(
    ActionType type,
    Rml::ElementDocument* document,
    Rml::Element* bubble = nullptr)
{
    if (!IsChatDocument(document) ||
        !document->IsVisible())
    {
        return;
    }

    g_state.pending_actions.push_back(
        PendingAction{
            type,
            document,
            bubble
        });

    if (g_state.action_task_pending)
    {
        return;
    }

    g_state.action_task_pending = true;

    if (!Ultron_PostToUI(
            &ProcessPendingActions,
            nullptr))
    {
        g_state.action_task_pending = false;
        g_state.pending_actions.clear();
    }
}


// =============================================================
// INPUT BAR
// =============================================================

void ToggleInputBar(Rml::ElementDocument* document)
{
    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    if (input_bar == nullptr)
    {
        return;
    }

    const bool expanded =
        input_bar->IsClassSet("expanded");

    input_bar->SetClass(
        "expanded",
        !expanded);

    // Recalculate after the width transition is requested.
    // Its height remains unchanged for a single-line input.

    UpdateInput(document);
}


// =============================================================
// BUBBLE TEXT
// =============================================================

void ReplaceBubbleText(
    Rml::ElementDocument* document,
    Rml::Element* bubble,
    const std::string& message)
{
    if (document == nullptr || bubble == nullptr)
    {
        return;
    }

    Rml::ElementPtr text_node =
        document->CreateTextNode(message);

    if (!text_node)
    {
        return;
    }

    while (Rml::Element* child =
        bubble->GetFirstChild())
    {
        Rml::ElementPtr removed =
            bubble->RemoveChild(child);
    }

    bubble->AppendChild(
        std::move(text_node));
}


// =============================================================
// LONG-PRESS CANCELLATION
// =============================================================

void CancelLongPress()
{
    g_state.press_generation.fetch_add(
        1,
        std::memory_order_acq_rel);

    g_state.press_active = false;

    g_state.press_source = PressSource::None;

    g_state.press_target = nullptr;

    if (g_long_press_worker.joinable())
    {
        g_long_press_worker.request_stop();
    }
}


// =============================================================
// MESSAGE EDITING
// =============================================================

void StartEdit(
    Rml::ElementDocument* document,
    Rml::Element* bubble)
{
    if (document == nullptr || bubble == nullptr)
    {
        return;
    }

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    auto* message_input =
        GetMessageInput(document);

    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    if (chat_area == nullptr ||
        message_input == nullptr ||
        input_bar == nullptr ||
        bubble->GetParentNode() != chat_area)
    {
        return;
    }


    // Retrieve original message.

    std::string message;

    const auto found =
        g_state.bubble_messages.find(bubble);

    if (found != g_state.bubble_messages.end())
    {
        message = found->second;
    }


    // Remove all messages below the selected message.

    Rml::Element* next =
        bubble->GetNextSibling();

    while (next != nullptr)
    {
        Rml::Element* following =
            next->GetNextSibling();

        g_state.bubble_messages.erase(next);

        Rml::ElementPtr removed =
            chat_area->RemoveChild(next);

        next = following;
    }


    // Discard previous detached editing bubble.

    if (g_state.editing_bubble)
    {
        g_state.bubble_messages.erase(
            g_state.editing_bubble.get());

        g_state.editing_bubble.reset();
    }


    // Detach selected bubble.

    g_state.editing_bubble =
        chat_area->RemoveChild(bubble);

    if (!g_state.editing_bubble)
    {
        return;
    }


    // Restore text into textarea.

    message_input->SetValue(message);


    // Expand input bar.

    input_bar->SetClass(
        "expanded",
        true);


    // Recalculate height and position.

    UpdateInput(document);
}


// =============================================================
// SCROLL TO LATEST MESSAGE
// =============================================================

void ScrollChatToBottom(
    Rml::ElementDocument* document)
{
    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr)
    {
        return;
    }

    document->UpdateDocument();

    chat_area->SetScrollTop(
        chat_area->GetScrollHeight());
}


// =============================================================
// MESSAGE SENDING
// =============================================================

void SendMessage(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return;
    }

    auto* message_input =
        GetMessageInput(document);

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (message_input == nullptr ||
        chat_area == nullptr)
    {
        return;
    }

    const std::string message =
        TrimWhitespace(
            message_input->GetValue());

    if (message.empty())
    {
        return;
    }


    // ---------------------------------------------------------
    // EDIT EXISTING MESSAGE
    // ---------------------------------------------------------

    if (g_state.editing_bubble)
    {
        Rml::Element* bubble =
            g_state.editing_bubble.get();

        ReplaceBubbleText(
            document,
            bubble,
            message);

        g_state.bubble_messages[bubble] =
            message;

        Rml::Element* appended =
            chat_area->AppendChild(
                std::move(g_state.editing_bubble));

        if (appended == nullptr)
        {
            return;
        }
    }

    // ---------------------------------------------------------
    // CREATE NEW MESSAGE
    // ---------------------------------------------------------

    else
    {
        Rml::ElementPtr bubble =
            document->CreateElement("div");

        if (!bubble)
        {
            return;
        }

        bubble->SetClass(
            "chat-message",
            true);

        Rml::ElementPtr text_node =
            document->CreateTextNode(message);

        if (!text_node)
        {
            return;
        }

        bubble->AppendChild(
            std::move(text_node));

        Rml::Element* appended =
            chat_area->AppendChild(
                std::move(bubble));

        if (appended == nullptr)
        {
            return;
        }

        g_state.bubble_messages[appended] =
            message;
    }


    // ---------------------------------------------------------
    // CLEAR INPUT
    // ---------------------------------------------------------

    message_input->SetValue("");


    // ---------------------------------------------------------
    // RESTORE EMPTY INPUT STATE
    // ---------------------------------------------------------

    UpdateInput(document);


    // ---------------------------------------------------------
    // SCROLL TO NEWEST MESSAGE
    // ---------------------------------------------------------

    ScrollChatToBottom(document);
}


// =============================================================
// ACTION EXECUTOR
// =============================================================

void ProcessPendingActions(void*)
{
    g_state.action_task_pending = false;

    std::vector<PendingAction> actions;

    actions.swap(
        g_state.pending_actions);


    for (const PendingAction& action : actions)
    {
        Rml::ElementDocument* document =
            action.document;

        if (!IsChatDocument(document) ||
            !document->IsVisible())
        {
            continue;
        }

        switch (action.type)
        {
            case ActionType::ToggleInput:
            {
                ToggleInputBar(document);
                break;
            }

            case ActionType::SendMessage:
            {
                SendMessage(document);
                break;
            }

            case ActionType::StartEdit:
            {
                StartEdit(
                    document,
                    action.bubble);

                break;
            }
        }
    }


    // Process actions queued during this batch.

    if (!g_state.pending_actions.empty() &&
        !g_state.action_task_pending)
    {
        g_state.action_task_pending = true;

        if (!Ultron_PostToUI(
                &ProcessPendingActions,
                nullptr))
        {
            g_state.action_task_pending = false;
            g_state.pending_actions.clear();
        }
    }
}


// =============================================================
// LONG-PRESS COMPLETION
// =============================================================

void CompleteLongPress(std::uint64_t generation)
{
    if (!g_state.press_active ||
        g_state.press_source == PressSource::None ||
        g_state.press_target == nullptr)
    {
        return;
    }

    if (g_state.press_generation.load(
            std::memory_order_acquire) != generation)
    {
        return;
    }

    const Uint64 elapsed =
        SDL_GetTicks() - g_state.press_started_at;

    if (elapsed < kLongPressDuration)
    {
        return;
    }

    Rml::Element* bubble =
        g_state.press_target;

    Rml::ElementDocument* document =
        bubble->GetOwnerDocument();

    CancelLongPress();

    if (IsChatDocument(document) &&
        document->IsVisible())
    {
        QueueAction(
            ActionType::StartEdit,
            document,
            bubble);
    }
}


// =============================================================
// LONG-PRESS TIMER TASK
// =============================================================

void LongPressTimeoutTask(void* user_data)
{
    std::unique_ptr<LongPressTicket> ticket(
        static_cast<LongPressTicket*>(user_data));

    if (!ticket)
    {
        return;
    }

    const std::uint64_t generation =
        ticket->generation;

    if (generation !=
        g_state.press_generation.load(
            std::memory_order_acquire))
    {
        return;
    }

    if (!g_state.press_active)
    {
        return;
    }

    CompleteLongPress(generation);
}


// =============================================================
// START LONG-PRESS
// =============================================================

void BeginLongPress(
    Rml::Element* bubble,
    PressSource source)
{
    if (bubble == nullptr ||
        source == PressSource::None)
    {
        return;
    }

    CancelLongPress();

    g_state.press_target = bubble;

    g_state.press_source = source;

    g_state.press_started_at =
        SDL_GetTicks();

    g_state.press_active = true;

    const std::uint64_t generation =
        g_state.press_generation.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;


    g_long_press_worker = std::jthread(
        [generation](std::stop_token stop_token)
        {
            std::mutex mutex;

            std::condition_variable_any condition;

            std::unique_lock lock(mutex);

            condition.wait_for(
                lock,
                stop_token,
                std::chrono::milliseconds(
                    kLongPressDuration),
                [] { return false; });

            if (stop_token.stop_requested())
            {
                return;
            }

            if (g_state.press_generation.load(
                    std::memory_order_acquire) != generation)
            {
                return;
            }

            auto* ticket =
                new (std::nothrow) LongPressTicket{
                    generation
                };

            if (ticket == nullptr)
            {
                return;
            }

            if (!Ultron_PostToUI(
                    &LongPressTimeoutTask,
                    ticket))
            {
                delete ticket;
            }
        });
}


// =============================================================
// MOUSE HANDLING
// =============================================================

void HandleMouseDown(Rml::Event& event)
{
    const int button =
        event.GetParameter<int>("button", 0);

    if (button != 0)
    {
        return;
    }

    Rml::ElementDocument* document =
        GetDocumentFromEvent(&event);

    if (!IsChatDocument(document))
    {
        CancelLongPress();
        return;
    }

    Rml::Element* bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            document);

    if (bubble == nullptr)
    {
        CancelLongPress();
        return;
    }

    BeginLongPress(
        bubble,
        PressSource::Mouse);
}


void HandleMouseUp(Rml::Event& event)
{
    const int button =
        event.GetParameter<int>("button", 0);

    if (button != 0 ||
        g_state.press_source != PressSource::Mouse ||
        !g_state.press_active)
    {
        return;
    }

    Rml::Element* bubble =
        g_state.press_target;

    Rml::ElementDocument* document =
        bubble != nullptr
            ? bubble->GetOwnerDocument()
            : nullptr;

    const Uint64 elapsed =
        SDL_GetTicks() - g_state.press_started_at;

    Rml::Element* released_bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            document);

    const bool released_inside =
        bubble != nullptr &&
        released_bubble == bubble;

    const bool completed =
        elapsed >= kLongPressDuration &&
        released_inside;

    CancelLongPress();

    if (completed &&
        IsChatDocument(document) &&
        document->IsVisible())
    {
        QueueAction(
            ActionType::StartEdit,
            document,
            bubble);
    }
}


void HandleMouseMove(Rml::Event& event)
{
    if (!g_state.press_active ||
        g_state.press_source != PressSource::Mouse)
    {
        return;
    }

    Rml::Element* pressed_bubble =
        g_state.press_target;

    if (pressed_bubble == nullptr)
    {
        CancelLongPress();
        return;
    }

    Rml::ElementDocument* document =
        pressed_bubble->GetOwnerDocument();

    Rml::Element* current_bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            document);

    if (current_bubble != pressed_bubble)
    {
        CancelLongPress();
    }
}


// =============================================================
// TOUCH POSITION AND HIT TESTING
// =============================================================

Rml::Vector2f GetTouchPosition(
    const SDL_Event& event,
    Rml::ElementDocument* document)
{
    Rml::Context* context =
        document != nullptr
            ? document->GetContext()
            : nullptr;

    if (context == nullptr)
    {
        return Rml::Vector2f{};
    }

    const Rml::Vector2i dimensions =
        context->GetDimensions();

    return Rml::Vector2f{
        event.tfinger.x *
            static_cast<float>(dimensions.x),

        event.tfinger.y *
            static_cast<float>(dimensions.y)
    };
}


Rml::Element* FindTouchBubble(
    Rml::ElementDocument* document,
    Rml::Vector2f position)
{
    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr ||
        !chat_area->IsPointWithinElement(position))
    {
        return nullptr;
    }

    // Newest bubble first.

    for (Rml::Element* child = chat_area->GetLastChild();
         child != nullptr;
         child = child->GetPreviousSibling())
    {
        if (child->IsClassSet("chat-message") &&
            child->IsVisible(true) &&
            child->IsPointWithinElement(position))
        {
            return child;
        }
    }

    return nullptr;
}


TouchTargetKind GetTouchTargetKind(
    Rml::ElementDocument* document,
    Rml::Vector2f position)
{
    if (!IsChatDocument(document))
    {
        return TouchTargetKind::None;
    }

    Rml::Element* wing =
        GetElement(document, "wing-hit");

    if (wing != nullptr &&
        wing->IsVisible(true) &&
        wing->IsPointWithinElement(position))
    {
        return TouchTargetKind::Wing;
    }

    Rml::Element* send =
        GetElement(document, "send-button");

    if (send != nullptr &&
        send->IsVisible(true) &&
        send->IsPointWithinElement(position))
    {
        return TouchTargetKind::Send;
    }

    if (FindTouchBubble(document, position) != nullptr)
    {
        return TouchTargetKind::Bubble;
    }

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area != nullptr &&
        chat_area->IsPointWithinElement(position))
    {
        return TouchTargetKind::Other;
    }

    return TouchTargetKind::None;
}


// =============================================================
// TOUCH STATE CLEANUP
// =============================================================

void ClearTouchState()
{
    if (g_state.press_source == PressSource::Touch)
    {
        CancelLongPress();
    }

    g_state.touch_active = false;

    g_state.touch_moved = false;

    g_state.touch_id = 0;

    g_state.finger_id = 0;

    g_state.touch_start_kind =
        TouchTargetKind::None;

    g_state.touch_start_x = 0.0f;

    g_state.touch_start_y = 0.0f;
}


// =============================================================
// SDL TOUCH EVENT HANDLER
// =============================================================

bool HandleSDLTouchEvent(
    const SDL_Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::ElementDocument* document =
        g_state.document;

    if (!IsChatDocument(document) ||
        !document->IsVisible())
    {
        return false;
    }

    switch (event->type)
    {
        // -----------------------------------------------------
        // FINGER DOWN
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_DOWN:
        {
            if (g_state.touch_active)
            {
                return false;
            }

            document->UpdateDocument();

            const Rml::Vector2f position =
                GetTouchPosition(*event, document);

            const TouchTargetKind target_kind =
                GetTouchTargetKind(document, position);

            g_state.touch_active = true;

            g_state.touch_moved = false;

            g_state.touch_id =
                event->tfinger.touchID;

            g_state.finger_id =
                event->tfinger.fingerID;

            g_state.touch_start_kind =
                target_kind;

            g_state.touch_start_x = position.x;

            g_state.touch_start_y = position.y;


            // Long-press only on a chat message.

            if (target_kind == TouchTargetKind::Bubble)
            {
                Rml::Element* bubble =
                    FindTouchBubble(document, position);

                if (bubble != nullptr)
                {
                    BeginLongPress(
                        bubble,
                        PressSource::Touch);
                }
            }

            // Do not consume the event.
            // RmlUi's SDL backend must receive it to handle
            // textarea focus and native scroll/inertia.

            return false;
        }


        // -----------------------------------------------------
        // FINGER MOTION
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_MOTION:
        {
            if (!g_state.touch_active ||
                event->tfinger.touchID != g_state.touch_id ||
                event->tfinger.fingerID != g_state.finger_id)
            {
                return false;
            }

            const Rml::Vector2f position =
                GetTouchPosition(*event, document);

            const float dx =
                position.x - g_state.touch_start_x;

            const float dy =
                position.y - g_state.touch_start_y;

            const float distance =
                std::sqrt(dx * dx + dy * dy);

            const float unit =
                GetViewportUnit(document);

            const float threshold =
                std::max(
                    4.0f,
                    kTouchMovementThresholdUnits * unit);

            if (!g_state.touch_moved &&
                distance >= threshold)
            {
                g_state.touch_moved = true;

                // A real scroll gesture cancels long-press.

                if (g_state.press_source == PressSource::Touch)
                {
                    CancelLongPress();
                }
            }

            // Let the RmlUi context process this movement.
            // It provides native scroll and inertial movement.

            return false;
        }


        // -----------------------------------------------------
        // FINGER UP
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_UP:
        {
            if (!g_state.touch_active ||
                event->tfinger.touchID != g_state.touch_id ||
                event->tfinger.fingerID != g_state.finger_id)
            {
                return false;
            }

            const Rml::Vector2f position =
                GetTouchPosition(*event, document);

            const Uint64 elapsed =
                SDL_GetTicks() - g_state.press_started_at;


            // Complete long-press on release if the timer
            // callback has not yet reached the UI queue.

            if (g_state.press_source == PressSource::Touch &&
                g_state.press_active &&
                !g_state.touch_moved &&
                elapsed >= kLongPressDuration)
            {
                const std::uint64_t generation =
                    g_state.press_generation.load(
                        std::memory_order_acquire);

                CompleteLongPress(generation);
            }
            else if (
                g_state.press_source == PressSource::Touch)
            {
                CancelLongPress();
            }


            // Normal tap actions.

            if (!g_state.touch_moved)
            {
                const TouchTargetKind release_kind =
                    GetTouchTargetKind(
                        document,
                        position);

                // The finger must start and finish on
                // the same interactive control.

                if (release_kind == g_state.touch_start_kind)
                {
                    if (release_kind == TouchTargetKind::Wing)
                    {
                        QueueAction(
                            ActionType::ToggleInput,
                            document);
                    }
                    else if (
                        release_kind == TouchTargetKind::Send)
                    {
                        QueueAction(
                            ActionType::SendMessage,
                            document);
                    }
                }
            }

            ClearTouchState();

            return false;
        }


        // -----------------------------------------------------
        // FINGER CANCEL
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_CANCELED:
        {
            if (g_state.touch_active &&
                event->tfinger.touchID == g_state.touch_id &&
                event->tfinger.fingerID == g_state.finger_id)
            {
                ClearTouchState();
            }

            return false;
        }

        default:
            return false;
    }
}


// =============================================================
// RML EVENT HANDLER
// =============================================================

bool HandleRmlEvent(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    const Rml::String& event_name =
        event->GetType();

    Rml::ElementDocument* document =
        GetDocumentFromEvent(event);


    // ---------------------------------------------------------
    // DOCUMENT SHOW
    // ---------------------------------------------------------

    if (event_name == "show")
    {
        if (IsChatDocument(document))
        {
            g_state.document = document;

            QueueInputUpdate(document);
        }

        return false;
    }


    // ---------------------------------------------------------
    // DOCUMENT HIDE
    // ---------------------------------------------------------

    if (event_name == "hide")
    {
        if (document == g_state.document)
        {
            CancelLongPress();

            ClearTouchState();

            g_state.document = nullptr;
        }

        if (g_state.pending_update_document == document)
        {
            g_state.pending_update_document = nullptr;
        }

        auto& actions =
            g_state.pending_actions;

        actions.erase(
            std::remove_if(
                actions.begin(),
                actions.end(),
                [document](const PendingAction& action)
                {
                    return action.document == document;
                }),
            actions.end());

        return false;
    }


    if (!IsChatDocument(document))
    {
        return false;
    }

    g_state.document = document;

    Rml::Element* target =
        event->GetTargetElement();


    // ---------------------------------------------------------
    // MOUSE CLICK
    // ---------------------------------------------------------

    if (event_name == "click")
    {
        if (event->GetParameter<int>("button", 0) != 0)
        {
            return false;
        }

        if (FindAncestorById(target, "wing-hit") != nullptr)
        {
            QueueAction(
                ActionType::ToggleInput,
                document);
        }
        else if (
            FindAncestorById(target, "send-button") != nullptr)
        {
            QueueAction(
                ActionType::SendMessage,
                document);
        }

        return false;
    }


    // ---------------------------------------------------------
    // MOUSE LONG-PRESS
    // ---------------------------------------------------------

    if (event_name == "mousedown")
    {
        HandleMouseDown(*event);
        return false;
    }

    if (event_name == "mouseup")
    {
        HandleMouseUp(*event);
        return false;
    }

    if (event_name == "mousemove")
    {
        HandleMouseMove(*event);
        return false;
    }


    // ---------------------------------------------------------
    // TEXT INPUT AND KEYBOARD
    // ---------------------------------------------------------

    Rml::Element* input =
        FindAncestorById(target, "message-input");

    if (input == nullptr)
    {
        return false;
    }

    if (event_name == "keydown")
    {
        const int key =
            event->GetParameter<int>(
                "key_identifier",
                Rml::Input::KI_UNKNOWN);

        const bool shift =
            event->GetParameter<bool>(
                "shift_key",
                false);

        // Enter sends.
        // Shift + Enter creates a newline.

        if ((key == Rml::Input::KI_RETURN ||
             key == Rml::Input::KI_NUMPADENTER) &&
            !shift)
        {
            QueueAction(
                ActionType::SendMessage,
                document);

            return true;
        }

        return false;
    }


    // Update arrow visibility and textarea dimensions
    // after text entry or a value change.

    if (event_name == "textinput" ||
        event_name == "keyup" ||
        event_name == "change")
    {
        QueueInputUpdate(document);
        return false;
    }

    return false;
}


// =============================================================
// EVENT REGISTRATION
// =============================================================

void RegisterRmlEvent(const char* event_name)
{
    Ultron_RegisterRmlEventHandler(
        kModuleName,
        event_name,
        &HandleRmlEvent,
        nullptr,
        100);
}


void RegisterSDLTouchEvent(Uint32 event_type)
{
    Ultron_RegisterEventHandler(
        kModuleName,
        event_type,
        &HandleSDLTouchEvent,
        nullptr,
        100);
}


// =============================================================
// MODULE REGISTRATION
// =============================================================

void RegisterObsidianWingChat()
{
    // RmlUi 6.3 has native touch input.
    //
    // Disable SDL-generated synthetic mouse events.
    // Native touch is handled by RmlUi's SDL platform backend.
    // Mouse input remains available for desktop devices.

    SDL_SetHint(
        SDL_HINT_TOUCH_MOUSE_EVENTS,
        "0");


    // Document lifecycle.

    RegisterRmlEvent("show");
    RegisterRmlEvent("hide");


    // Mouse interaction.

    RegisterRmlEvent("click");
    RegisterRmlEvent("mousedown");
    RegisterRmlEvent("mouseup");
    RegisterRmlEvent("mousemove");


    // Textarea interaction.

    RegisterRmlEvent("keydown");
    RegisterRmlEvent("keyup");
    RegisterRmlEvent("textinput");
    RegisterRmlEvent("change");


    // Native touch events.
    //
    // The handler observes taps and long-press.
    // It returns false so the RmlUi backend can process
    // native scrolling, focus, and inertial movement.

    RegisterSDLTouchEvent(SDL_EVENT_FINGER_DOWN);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_MOTION);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_UP);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_CANCELED);
}


// =============================================================
// AUTOMATIC REGISTRATION
// =============================================================

struct ObsidianWingChatRegistration
{
    ObsidianWingChatRegistration()
    {
        RegisterObsidianWingChat();
    }

    ~ObsidianWingChatRegistration()
    {
        CancelLongPress();

        ClearTouchState();
    }
};


[[maybe_unused]]
ObsidianWingChatRegistration g_registration;


} // namespace
