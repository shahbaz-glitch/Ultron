#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/ElementText.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <utility>

namespace
{

constexpr char kModuleName[] = "obsidian-wing-chat";
constexpr Uint32 kLongPressMilliseconds = 550;

struct ChatElements
{
    Rml::ElementDocument* document = nullptr;
    Rml::Element* input_bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* send_button = nullptr;
    Rml::Element* chat_area = nullptr;
    Rml::ElementFormControlTextArea* message_input = nullptr;

    explicit operator bool() const noexcept
    {
        return document != nullptr && input_bar != nullptr &&
               wing_hit != nullptr && send_button != nullptr &&
               chat_area != nullptr && message_input != nullptr;
    }
};

Rml::ElementDocument* g_active_document = nullptr;
Rml::ElementDocument* g_last_input_document = nullptr;
Rml::String g_last_input_value;
bool g_first_line_shifted = false;
bool g_update_queued = false;
Rml::ElementDocument* g_pending_update_document = nullptr;

Rml::ElementPtr g_editing_bubble;
Rml::ElementDocument* g_editing_document = nullptr;

struct PressState
{
    bool active = false;
    bool expired = false;
    bool is_touch = false;
    bool inside = true;
    std::uint64_t generation = 0;
    SDL_TouchID touch_id = 0;
    SDL_FingerID finger_id = 0;
    Rml::ElementDocument* document = nullptr;
    Rml::Element* bubble = nullptr;
};

std::mutex g_press_mutex;
PressState g_press;

struct LongPressTicket
{
    std::uint64_t generation = 0;
};

Rml::String ToPixels(float value)
{
    char buffer[48]{};
    std::snprintf(buffer, sizeof(buffer), "%.3fpx", value);
    return Rml::String(buffer);
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

bool DecodeUtf8(
    const Rml::String& value,
    std::size_t offset,
    std::uint32_t& code_point,
    std::size_t& byte_count)
{
    if (offset >= value.size())
    {
        return false;
    }

    const auto byte = [&value](std::size_t index)
    {
        return static_cast<unsigned char>(value[index]);
    };

    const unsigned char first = byte(offset);

    if (first < 0x80u)
    {
        code_point = first;
        byte_count = 1;
        return true;
    }

    std::size_t count = 0;
    std::uint32_t decoded = 0;

    if ((first & 0xE0u) == 0xC0u)
    {
        count = 2;
        decoded = first & 0x1Fu;
    }
    else if ((first & 0xF0u) == 0xE0u)
    {
        count = 3;
        decoded = first & 0x0Fu;
    }
    else if ((first & 0xF8u) == 0xF0u)
    {
        count = 4;
        decoded = first & 0x07u;
    }
    else
    {
        code_point = first;
        byte_count = 1;
        return true;
    }

    if (offset + count > value.size())
    {
        code_point = first;
        byte_count = 1;
        return true;
    }

    for (std::size_t index = 1; index < count; ++index)
    {
        const unsigned char continuation = byte(offset + index);
        if ((continuation & 0xC0u) != 0x80u)
        {
            code_point = first;
            byte_count = 1;
            return true;
        }

        decoded = (decoded << 6u) | (continuation & 0x3Fu);
    }

    code_point = decoded;
    byte_count = count;
    return true;
}

bool IsJavaScriptWhitespace(std::uint32_t code_point) noexcept
{
    return (code_point >= 0x0009u && code_point <= 0x000Du) ||
           code_point == 0x0020u ||
           code_point == 0x00A0u ||
           code_point == 0x1680u ||
           (code_point >= 0x2000u && code_point <= 0x200Au) ||
           code_point == 0x2028u ||
           code_point == 0x2029u ||
           code_point == 0x202Fu ||
           code_point == 0x205Fu ||
           code_point == 0x3000u ||
           code_point == 0xFEFFu;
}

Rml::String TrimMessage(const Rml::String& value)
{
    std::size_t first = 0;
    while (first < value.size())
    {
        std::uint32_t code_point = 0;
        std::size_t byte_count = 0;
        if (!DecodeUtf8(value, first, code_point, byte_count) ||
            !IsJavaScriptWhitespace(code_point))
        {
            break;
        }
        first += byte_count;
    }

    std::size_t last = value.size();
    while (last > first)
    {
        std::size_t start = last - 1;
        while (start > first &&
               (static_cast<unsigned char>(value[start]) & 0xC0u) == 0x80u)
        {
            --start;
        }

        std::uint32_t code_point = 0;
        std::size_t byte_count = 0;
        if (!DecodeUtf8(value, start, code_point, byte_count) ||
            start + byte_count != last ||
            !IsJavaScriptWhitespace(code_point))
        {
            break;
        }
        last = start;
    }

    return value.substr(first, last - first);
}

ChatElements GetChatElements(Rml::ElementDocument* document)
{
    ChatElements elements;
    if (document == nullptr)
    {
        return elements;
    }

    elements.document = document;
    elements.input_bar = document->GetElementById("input-bar");
    elements.wing_hit = document->GetElementById("wing-hit");
    elements.send_button = document->GetElementById("send-button");
    elements.chat_area = document->GetElementById("chat-area");
    elements.message_input = dynamic_cast<Rml::ElementFormControlTextArea*>(
        document->GetElementById("message-input"));

    return elements;
}

Rml::ElementDocument* ResolveDocument(Rml::Element* target)
{
    if (target == nullptr)
    {
        return nullptr;
    }

    Rml::ElementDocument* document = target->GetOwnerDocument();
    if (document == nullptr)
    {
        document = dynamic_cast<Rml::ElementDocument*>(target);
    }

    return GetChatElements(document) ? document : nullptr;
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

bool IsInsideElement(Rml::Element* element, Rml::Element* ancestor)
{
    for (Rml::Element* current = element;
         current != nullptr;
         current = current->GetParentNode())
    {
        if (current == ancestor)
        {
            return true;
        }
    }

    return false;
}

void UpdateInput(Rml::ElementDocument* document, bool force = false)
{
    ChatElements elements = GetChatElements(document);
    if (!elements)
    {
        return;
    }

    const Rml::String message = elements.message_input->GetValue();

    if (!force && document == g_last_input_document &&
        message == g_last_input_value)
    {
        return;
    }

    const float control_height = elements.wing_hit->GetOffsetHeight();
    if (control_height <= 0.0f)
    {
        return;
    }

    g_last_input_document = document;
    g_last_input_value = message;

    const float padding_height = control_height * (1.23f / 14.22f);
    const float max_text_height = control_height * (30.15f / 14.22f);
    const float extra_gap = control_height * (1.5f / 14.22f);

    float bar_height = control_height;

    if (message.empty())
    {
        g_first_line_shifted = false;
        elements.input_bar->SetClass("multiline", false);
        elements.message_input->RemoveProperty("height");
    }
    else
    {
        if (!g_first_line_shifted)
        {
            // Resolve the latest textarea value before testing the first wrap.
            document->UpdateDocument();

            if (elements.message_input->GetScrollHeight() >
                elements.message_input->GetClientHeight() + 1.0f)
            {
                g_first_line_shifted = true;
                elements.input_bar->SetClass("multiline", true);
            }
        }

        if (g_first_line_shifted)
        {
            // One layout pass applies the full-width multiline position and
            // measures the current value together, avoiding repeated reflows.
            elements.message_input->SetProperty("height", "0px");
            document->UpdateDocument();

            const float content_height =
                elements.message_input->GetScrollHeight();

            const float new_height = std::min(
                content_height,
                max_text_height);

            elements.message_input->SetProperty(
                "height",
                ToPixels(std::max(0.0f, new_height - padding_height)));

            bar_height = new_height + control_height + extra_gap;
        }
        else
        {
            elements.message_input->RemoveProperty("height");
            bar_height = control_height;
        }
    }

    elements.input_bar->SetProperty("height", ToPixels(bar_height));

    const float chat_bottom =
        bar_height + control_height * (11.52f / 14.22f);

    elements.chat_area->SetProperty("bottom", ToPixels(chat_bottom));
    elements.input_bar->SetClass("has-text", !TrimMessage(message).empty());
}

void UpdateInputTask(void* user_data)
{
    (void)user_data;

    Rml::ElementDocument* document = g_pending_update_document;
    g_pending_update_document = nullptr;
    g_update_queued = false;

    if (document != nullptr)
    {
        UpdateInput(document);
    }
}

void ScheduleInputUpdate(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return;
    }

    g_pending_update_document = document;
    if (g_update_queued)
    {
        return;
    }

    g_update_queued = true;
    if (!Ultron_PostToUI(UpdateInputTask, nullptr))
    {
        g_update_queued = false;
        g_pending_update_document = nullptr;
    }
}

void ToggleInputBar(Rml::ElementDocument* document)
{
    ChatElements elements = GetChatElements(document);
    if (!elements)
    {
        return;
    }

    const bool expanded = elements.input_bar->IsClassSet("expanded");
    elements.input_bar->SetClass("expanded", !expanded);

    if (!expanded)
    {
        elements.message_input->Focus();
    }
}

void StartEdit(Rml::ElementDocument* document, Rml::Element* bubble)
{
    ChatElements elements = GetChatElements(document);
    if (!elements || bubble == nullptr ||
        bubble->GetParentNode() != elements.chat_area)
    {
        return;
    }

    // Remove every later sibling, matching the original JS edit behavior.
    Rml::Element* next = bubble->GetNextSibling();
    while (next != nullptr)
    {
        Rml::Element* following = next->GetNextSibling();
        elements.chat_area->RemoveChild(next);
        next = following;
    }

    Rml::String message;
    if (auto* text_node = dynamic_cast<Rml::ElementText*>(bubble->GetFirstChild()))
    {
        message = text_node->GetText();
    }

    g_editing_bubble = elements.chat_area->RemoveChild(bubble);
    if (!g_editing_bubble)
    {
        g_editing_document = nullptr;
        return;
    }

    g_editing_document = document;
    elements.message_input->SetValue(message);
    g_first_line_shifted = false;
    elements.input_bar->SetClass("expanded", true);

    UpdateInput(document, true);
    elements.message_input->Focus();
}

void SendMessage(Rml::ElementDocument* document)
{
    ChatElements elements = GetChatElements(document);
    if (!elements)
    {
        return;
    }

    const Rml::String message = TrimMessage(
        elements.message_input->GetValue());

    if (message.empty())
    {
        return;
    }

    if (g_editing_bubble && g_editing_document == document)
    {
        g_editing_bubble->SetInnerRML(EscapeRmlText(message));
        elements.chat_area->AppendChild(std::move(g_editing_bubble));
        g_editing_document = nullptr;
    }
    else
    {
        g_editing_bubble.reset();
        g_editing_document = nullptr;

        Rml::ElementPtr bubble = document->CreateElement("div");
        if (!bubble)
        {
            return;
        }

        bubble->SetClass("chat-message", true);
        bubble->SetInnerRML(EscapeRmlText(message));
        elements.chat_area->AppendChild(std::move(bubble));
    }

    elements.message_input->SetValue("");
    g_first_line_shifted = false;
    UpdateInput(document, true);

    // Reflow once after appending/reusing the message, then scroll to the newest one.
    document->UpdateDocument();
    elements.chat_area->SetScrollTop(elements.chat_area->GetScrollHeight());
}

void CancelPress(bool force = false)
{
    std::lock_guard lock(g_press_mutex);

    if (!g_press.active)
    {
        return;
    }

    // Once the 550ms timer has expired, allow its queued UI callback to finish.
    if (g_press.expired && !force)
    {
        return;
    }

    g_press.active = false;
    g_press.bubble = nullptr;
    g_press.document = nullptr;
    ++g_press.generation;
}

Uint32 SDLCALL OnLongPressTimer(
    void* user_data,
    SDL_TimerID timer_id,
    Uint32 interval)
{
    (void)timer_id;
    (void)interval;

    auto* ticket = static_cast<LongPressTicket*>(user_data);
    if (ticket == nullptr)
    {
        return 0;
    }

    bool valid = false;
    {
        std::lock_guard lock(g_press_mutex);
        valid = g_press.active &&
                !g_press.expired &&
                g_press.generation == ticket->generation;

        if (valid)
        {
            g_press.expired = true;
        }
    }

    if (!valid || !Ultron_PostToUI(
            [](void* data)
            {
                std::unique_ptr<LongPressTicket> owned_ticket(
                    static_cast<LongPressTicket*>(data));

                Rml::ElementDocument* document = nullptr;
                Rml::Element* bubble = nullptr;

                {
                    std::lock_guard lock(g_press_mutex);
                    if (g_press.active && g_press.expired &&
                        g_press.generation == owned_ticket->generation)
                    {
                        document = g_press.document;
                        bubble = g_press.bubble;
                        g_press.active = false;
                        g_press.document = nullptr;
                        g_press.bubble = nullptr;
                    }
                }

                if (document != nullptr && bubble != nullptr)
                {
                    StartEdit(document, bubble);
                }
            },
            ticket))
    {
        delete ticket;
    }

    return 0;
}

void BeginLongPress(
    Rml::ElementDocument* document,
    Rml::Element* bubble,
    bool is_touch,
    SDL_TouchID touch_id = 0,
    SDL_FingerID finger_id = 0)
{
    if (document == nullptr || bubble == nullptr)
    {
        return;
    }

    CancelPress(true);

    auto* ticket = new (std::nothrow) LongPressTicket;
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
        g_press.inside = true;
        g_press.touch_id = touch_id;
        g_press.finger_id = finger_id;
        g_press.document = document;
        g_press.bubble = bubble;
        ticket->generation = g_press.generation;
    }

    if (SDL_AddTimer(kLongPressMilliseconds, OnLongPressTimer, ticket) == 0)
    {
        delete ticket;
        CancelPress(true);
    }
}

bool ContainsPoint(
    Rml::Element* element,
    float x,
    float y)
{
    if (element == nullptr || element->GetOffsetWidth() <= 0.0f ||
        element->GetOffsetHeight() <= 0.0f)
    {
        return false;
    }

    const float left = element->GetAbsoluteLeft();
    const float top = element->GetAbsoluteTop();
    const float right = left + element->GetOffsetWidth();
    const float bottom = top + element->GetOffsetHeight();

    return x >= left && x <= right && y >= top && y <= bottom;
}

enum class TouchTarget
{
    None,
    Wing,
    Send,
    TextInput,
    Bubble
};

struct TouchHit
{
    TouchTarget target = TouchTarget::None;
    Rml::Element* bubble = nullptr;
};

TouchHit HitTestTouch(
    Rml::ElementDocument* document,
    float x,
    float y)
{
    ChatElements elements = GetChatElements(document);
    if (!elements)
    {
        return {};
    }

    if (ContainsPoint(elements.send_button, x, y))
    {
        return {TouchTarget::Send, nullptr};
    }

    if (ContainsPoint(elements.wing_hit, x, y))
    {
        return {TouchTarget::Wing, nullptr};
    }

    if (ContainsPoint(elements.message_input, x, y))
    {
        return {TouchTarget::TextInput, nullptr};
    }

    for (Rml::Element* child = elements.chat_area->GetFirstChild();
         child != nullptr;
         child = child->GetNextSibling())
    {
        if (child->IsClassSet("chat-message") && ContainsPoint(child, x, y))
        {
            return {TouchTarget::Bubble, child};
        }
    }

    return {};
}

struct TouchTapState
{
    TouchTarget target = TouchTarget::None;
};

TouchTapState g_touch_tap;

bool BeginTouch(const SDL_Event& event)
{
    Rml::ElementDocument* document = g_active_document;
    if (document == nullptr || document->GetContext() == nullptr)
    {
        return false;
    }

    Rml::Context* context = document->GetContext();
    const Rml::Vector2i dimensions = context->GetDimensions();
    const float x = event.tfinger.x * static_cast<float>(dimensions.x);
    const float y = event.tfinger.y * static_cast<float>(dimensions.y);
    const TouchHit hit = HitTestTouch(document, x, y);

    CancelPress(true);
    g_touch_tap = {};

    if (hit.target == TouchTarget::Bubble)
    {
        BeginLongPress(
            document,
            hit.bubble,
            true,
            event.tfinger.touchID,
            event.tfinger.fingerID);
        return false;
    }

    if (hit.target == TouchTarget::TextInput)
    {
        // RmlUi 6.3 does not dispatch DOM touch events. Route this single
        // textarea gesture through its native mouse input path so focus,
        // caret placement, and drag selection behave like a normal tap.
        context->ProcessMouseMove(
            static_cast<int>(x),
            static_cast<int>(y),
            0);
        context->ProcessMouseButtonDown(0, 0);

        g_touch_tap.target = TouchTarget::TextInput;

        std::lock_guard lock(g_press_mutex);
        ++g_press.generation;
        g_press.active = true;
        g_press.expired = false;
        g_press.is_touch = true;
        g_press.touch_id = event.tfinger.touchID;
        g_press.finger_id = event.tfinger.fingerID;
        g_press.document = document;
        g_press.bubble = nullptr;
        return true;
    }

    if (hit.target == TouchTarget::Wing || hit.target == TouchTarget::Send)
    {
        g_touch_tap.target = hit.target;

        std::lock_guard lock(g_press_mutex);
        ++g_press.generation;
        g_press.active = true;
        g_press.expired = false;
        g_press.is_touch = true;
        g_press.touch_id = event.tfinger.touchID;
        g_press.finger_id = event.tfinger.fingerID;
        g_press.document = document;
        g_press.bubble = nullptr;
    }

    return false;
}

bool MoveTouch(const SDL_Event& event)
{
    Rml::ElementDocument* document = nullptr;
    Rml::Element* bubble = nullptr;

    {
        std::lock_guard lock(g_press_mutex);
        if (!g_press.active || !g_press.is_touch ||
            g_press.touch_id != event.tfinger.touchID ||
            g_press.finger_id != event.tfinger.fingerID)
        {
            return false;
        }

        document = g_press.document;
        bubble = g_press.bubble;
    }

    if (document == nullptr || document->GetContext() == nullptr)
    {
        return false;
    }

    const Rml::Vector2i dimensions = document->GetContext()->GetDimensions();
    const float x = event.tfinger.x * static_cast<float>(dimensions.x);
    const float y = event.tfinger.y * static_cast<float>(dimensions.y);

    if (bubble != nullptr)
    {
        if (!ContainsPoint(bubble, x, y))
        {
            CancelPress();
        }
        return false;
    }

    if (g_touch_tap.target == TouchTarget::TextInput)
    {
        document->GetContext()->ProcessMouseMove(
            static_cast<int>(x),
            static_cast<int>(y),
            0);
        return true;
    }

    if (g_touch_tap.target == TouchTarget::Wing ||
        g_touch_tap.target == TouchTarget::Send)
    {
        const TouchHit current_hit = HitTestTouch(document, x, y);
        const bool still_inside = current_hit.target == g_touch_tap.target;

        std::lock_guard lock(g_press_mutex);
        if (g_press.active && g_press.is_touch &&
            g_press.touch_id == event.tfinger.touchID &&
            g_press.finger_id == event.tfinger.fingerID)
        {
            g_press.inside = still_inside;
        }
    }

    return false;
}

bool EndTouch(const SDL_Event& event)
{
    Rml::ElementDocument* document = nullptr;
    TouchTarget target = TouchTarget::None;
    bool trigger = false;

    {
        std::lock_guard lock(g_press_mutex);
        if (!g_press.active || !g_press.is_touch ||
            g_press.touch_id != event.tfinger.touchID ||
            g_press.finger_id != event.tfinger.fingerID)
        {
            return false;
        }

        if (g_press.expired)
        {
            // The long-press callback is already queued on the UI thread.
            return false;
        }

        document = g_press.document;
        trigger = g_press.inside;
        target = g_touch_tap.target;
        g_press.active = false;
        g_press.document = nullptr;
        g_press.bubble = nullptr;
        ++g_press.generation;
    }

    g_touch_tap = {};

    if (document == nullptr)
    {
        return target == TouchTarget::TextInput;
    }

    if (target == TouchTarget::TextInput)
    {
        if (Rml::Context* context = document->GetContext())
        {
            const Rml::Vector2i dimensions = context->GetDimensions();
            const float x = event.tfinger.x * static_cast<float>(dimensions.x);
            const float y = event.tfinger.y * static_cast<float>(dimensions.y);
            context->ProcessMouseMove(
                static_cast<int>(x),
                static_cast<int>(y),
                0);
            context->ProcessMouseButtonUp(0, 0);
        }
        return true;
    }

    if (trigger)
    {
        if (target == TouchTarget::Wing)
        {
            ToggleInputBar(document);
        }
        else if (target == TouchTarget::Send)
        {
            SendMessage(document);
        }
    }

    return false;
}

bool CancelTouch(const SDL_Event& event)
{
    Rml::ElementDocument* document = nullptr;
    bool textarea_gesture = false;

    {
        std::lock_guard lock(g_press_mutex);
        if (g_press.active && g_press.is_touch &&
            g_press.touch_id == event.tfinger.touchID &&
            g_press.finger_id == event.tfinger.fingerID)
        {
            document = g_press.document;
            textarea_gesture = g_touch_tap.target == TouchTarget::TextInput;
        }
    }

    CancelPress(true);
    g_touch_tap = {};

    if (textarea_gesture && document != nullptr)
    {
        if (Rml::Context* context = document->GetContext())
        {
            context->ProcessMouseMove(-1, -1, 0);
            context->ProcessMouseButtonUp(0, 0);
            context->ProcessMouseLeave();
        }
        return true;
    }

    return false;
}

bool HandleSdlTouch(
    const SDL_Event* event,
    void* user_data)
{
    (void)user_data;
    if (event == nullptr)
    {
        return false;
    }

    switch (event->type)
    {
        case SDL_EVENT_FINGER_DOWN:
            return BeginTouch(*event);

        case SDL_EVENT_FINGER_MOTION:
            return MoveTouch(*event);

        case SDL_EVENT_FINGER_UP:
            return EndTouch(*event);

        case SDL_EVENT_FINGER_CANCELED:
            return CancelTouch(*event);

        default:
            return false;
    }
}

bool HandleRmlEvent(
    Rml::Event* event,
    void* user_data)
{
    (void)user_data;
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* target = event->GetTargetElement();
    Rml::ElementDocument* document = ResolveDocument(target);
    if (document != nullptr)
    {
        g_active_document = document;
    }

    const Rml::String& type = event->GetType();

    if (type == "show")
    {
        if (document != nullptr)
        {
            g_active_document = document;
            UpdateInput(document, true);
        }
        return false;
    }

    if (type == "hide")
    {
        if (document != nullptr && document == g_active_document)
        {
            CancelPress(true);
            g_active_document = nullptr;
            g_last_input_document = nullptr;
            g_last_input_value.clear();
            g_first_line_shifted = false;
            g_editing_bubble.reset();
            g_editing_document = nullptr;
        }
        return false;
    }

    if (type == "click")
    {
        if (document == nullptr)
        {
            return false;
        }

        if (FindAncestorById(target, "wing-hit") != nullptr)
        {
            ToggleInputBar(document);
            return true;
        }

        if (FindAncestorById(target, "send-button") != nullptr)
        {
            SendMessage(document);
            return true;
        }

        return false;
    }

    if (type == "textinput" || type == "keydown" || type == "change")
    {
        if (document != nullptr)
        {
            ChatElements elements = GetChatElements(document);
            if (elements && IsInsideElement(target, elements.message_input))
            {
                ScheduleInputUpdate(document);
            }
        }
        return false;
    }

    if (type == "mousedown")
    {
        if (document != nullptr &&
            event->GetParameter<int>("button", 0) == 0)
        {
            if (Rml::Element* bubble = FindChatBubble(target))
            {
                BeginLongPress(document, bubble, false);
            }
        }
        return false;
    }

    if (type == "mouseup")
    {
        CancelPress();
        return false;
    }

    if (type == "mouseout")
    {
        Rml::Element* bubble = FindChatBubble(target);
        if (bubble != nullptr)
        {
            bool should_cancel = false;
            {
                std::lock_guard lock(g_press_mutex);
                should_cancel = g_press.active && !g_press.is_touch &&
                                g_press.bubble == bubble;
            }
            if (should_cancel)
            {
                CancelPress();
            }
        }
        return false;
    }

    return false;
}

struct AutoRegistration
{
    AutoRegistration()
    {
        const char* const rml_events[] = {
            "show",
            "hide",
            "click",
            "textinput",
            "keydown",
            "change",
            "mousedown",
            "mouseup",
            "mouseout"
        };

        for (const char* event_name : rml_events)
        {
            if (!Ultron_RegisterRmlEventHandler(
                    kModuleName,
                    event_name,
                    HandleRmlEvent,
                    nullptr,
                    0))
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "ObsidianWingChat: failed to register RmlUi event '%s'.",
                    event_name);
            }
        }

        const Uint32 touch_events[] = {
            SDL_EVENT_FINGER_DOWN,
            SDL_EVENT_FINGER_MOTION,
            SDL_EVENT_FINGER_UP,
            SDL_EVENT_FINGER_CANCELED
        };

        for (const Uint32 event_type : touch_events)
        {
            if (!Ultron_RegisterEventHandler(
                    kModuleName,
                    event_type,
                    HandleSdlTouch,
                    nullptr,
                    0))
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "ObsidianWingChat: failed to register SDL touch event %u.",
                    event_type);
            }
        }

        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "ObsidianWingChat: native C++20 event handlers registered.");
    }
};

const AutoRegistration g_auto_registration{};

} // namespace
