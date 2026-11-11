
#include "ultron_api.h"

#include <SDL3/SDL.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Input.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
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

constexpr float kInitialInputHeight = 30.0f;
constexpr float kMaximumInputHeight = 118.0f;
constexpr float kMinimumBarHeight = 58.0f;

constexpr float kBarExtraHeight = 34.0f;
constexpr float kChatBottomOffset = 47.0f;


// =============================================================
// INPUT SOURCE
// =============================================================

enum class PressSource
{
    None,
    Mouse,
    Touch
};


// =============================================================
// PENDING ACTIONS
// =============================================================

enum class ActionType
{
    ToggleInput,
    SendMessage,
    StartEdit
};

struct PendingAction
{
    ActionType type = ActionType::SendMessage;

    Rml::ElementDocument* document = nullptr;
    Rml::Element* bubble = nullptr;
};


// =============================================================
// MODULE STATE
// =============================================================

struct ChatState
{
    Rml::ElementDocument* document = nullptr;

    Rml::ElementPtr editing_bubble;

    std::unordered_map<
        Rml::Element*,
        std::string
    > bubble_messages;

    std::vector<PendingAction> pending_actions;

    bool action_task_pending = false;

    bool update_task_pending = false;

    Rml::ElementDocument* pending_update_document = nullptr;


    // Long press state.

    std::atomic_uint64_t press_generation{0};

    std::atomic_uint64_t expired_generation{0};

    bool press_active = false;

    PressSource press_source = PressSource::None;

    Rml::Element* press_target = nullptr;

    Uint64 press_started_at = 0;


    // Touch state.

    bool touch_active = false;

    bool touch_inside = false;

    SDL_TouchID touch_id = 0;

    SDL_FingerID finger_id = 0;

    Rml::Element* touch_target = nullptr;
};

ChatState g_state;


// =============================================================
// LONG PRESS WORKER
// =============================================================

std::jthread g_long_press_worker;


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

    // This element is declared as <textarea> in the RML.
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

    Rml::Element* target = event->GetTargetElement();

    return target != nullptr
        ? target->GetOwnerDocument()
        : nullptr;
}


// =============================================================
// JAVASCRIPT-LIKE TRIM
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
            (static_cast<unsigned char>(value[position + 1]) & 0x3F);
    }

    if ((first & 0xF0) == 0xE0 &&
        position + 2 < value.size())
    {
        length = 3;

        return
            ((first & 0x0F) << 12) |
            ((static_cast<unsigned char>(value[position + 1]) & 0x3F) << 6) |
            (static_cast<unsigned char>(value[position + 2]) & 0x3F);
    }

    if ((first & 0xF8) == 0xF0 &&
        position + 3 < value.size())
    {
        length = 4;

        return
            ((first & 0x07) << 18) |
            ((static_cast<unsigned char>(value[position + 1]) & 0x3F) << 12) |
            ((static_cast<unsigned char>(value[position + 2]) & 0x3F) << 6) |
            (static_cast<unsigned char>(value[position + 3]) & 0x3F);
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
            (static_cast<unsigned char>(value[position]) & 0xC0) == 0x80)
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
// CSS PROPERTY HELPERS
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
// INPUT BAR HEIGHT AND CHAT POSITION
// =============================================================

void UpdateInput(Rml::ElementDocument* document)
{
    if (document == nullptr)
    {
        return;
    }

    auto* message_input = GetMessageInput(document);

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

    // First reset to the original base height.

    message_input->SetProperty(
        "height",
        ToPixels(kInitialInputHeight));

    // Recalculate layout before reading the scroll height.

    document->UpdateDocument();

    const float content_height =
        message_input->GetScrollHeight();

    const float new_height = std::clamp(
        content_height,
        kInitialInputHeight,
        kMaximumInputHeight);

    // Apply textarea dimensions.

    message_input->SetProperty(
        "height",
        ToPixels(new_height));

    message_input->SetProperty(
        "margin-top",
        ToPixels(-new_height / 2.0f));

    // Resize the entire input bar.

    const float bar_height = std::max(
        kMinimumBarHeight,
        new_height + kBarExtraHeight);

    input_bar->SetProperty(
        "height",
        ToPixels(bar_height));

    // Move chat area according to the input bar.

    chat_area->SetProperty(
        "bottom",
        ToPixels(bar_height + kChatBottomOffset));

    // Pulse / arrow visibility.

    const bool has_text =
        !TrimWhitespace(message_input->GetValue()).empty();

    input_bar->SetClass("has-text", has_text);
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

    if (IsChatDocument(document))
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

    if (!Ultron_PostToUI(&UpdateInputTask, nullptr))
    {
        g_state.update_task_pending = false;
        g_state.pending_update_document = nullptr;
    }
}


// =============================================================
// PENDING ACTION EXECUTOR
// =============================================================

void ToggleInputBar(Rml::ElementDocument* document)
{
    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    if (input_bar == nullptr)
    {
        return;
    }

    input_bar->SetClass(
        "expanded",
        !input_bar->IsClassSet("expanded"));
}


void ProcessPendingActions(void*);


void QueueAction(
    ActionType type,
    Rml::ElementDocument* document,
    Rml::Element* bubble = nullptr)
{
    if (!IsChatDocument(document))
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

    if (!Ultron_PostToUI(&ProcessPendingActions, nullptr))
    {
        // During normal interaction the UI task queue is active.
        // A failed post must not leave a permanently pending action.
        g_state.action_task_pending = false;
        g_state.pending_actions.clear();
    }
}


// =============================================================
// BUBBLE TEXT MANAGEMENT
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

    while (Rml::Element* child = bubble->GetFirstChild())
    {
        bubble->RemoveChild(child);
    }

    bubble->AppendChild(std::move(text_node));
}


// =============================================================
// LONG PRESS CANCELLATION
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

    // Retrieve the original message.

    std::string message;

    const auto found =
        g_state.bubble_messages.find(bubble);

    if (found != g_state.bubble_messages.end())
    {
        message = found->second;
    }

    // Remove all messages below the selected message.

    Rml::Element* next = bubble->GetNextSibling();

    while (next != nullptr)
    {
        Rml::Element* following =
            next->GetNextSibling();

        g_state.bubble_messages.erase(next);

        chat_area->RemoveChild(next);

        next = following;
    }

    // Discard a previously detached editing bubble, if any.

    if (g_state.editing_bubble)
    {
        g_state.bubble_messages.erase(
            g_state.editing_bubble.get());

        g_state.editing_bubble.reset();
    }

    // Keep ownership of the selected bubble while editing.

    g_state.editing_bubble =
        chat_area->RemoveChild(bubble);

    if (!g_state.editing_bubble)
    {
        return;
    }

    // Restore its text into the input field.

    message_input->SetValue(message);

    // Expand input bar.

    input_bar->SetClass("expanded", true);

    // Recalculate height and position.

    UpdateInput(document);
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
        TrimWhitespace(message_input->GetValue());

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

        g_state.bubble_messages[bubble] = message;

        chat_area->AppendChild(
            std::move(g_state.editing_bubble));
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

        bubble->SetClass("chat-message", true);

        Rml::ElementPtr text_node =
            document->CreateTextNode(message);

        if (!text_node)
        {
            return;
        }

        bubble->AppendChild(std::move(text_node));

        Rml::Element* appended =
            chat_area->AppendChild(std::move(bubble));

        if (appended == nullptr)
        {
            return;
        }

        g_state.bubble_messages[appended] = message;
    }

    // Clear the input.

    message_input->SetValue("");

    // Update pulse / arrow and input dimensions.

    UpdateInput(document);

    // Force layout before calculating final scroll position.

    document->UpdateDocument();

    chat_area->SetScrollTop(
        chat_area->GetScrollHeight());
}


// =============================================================
// ACTION EXECUTOR
// =============================================================

void ProcessPendingActions(void*)
{
    g_state.action_task_pending = false;

    std::vector<PendingAction> actions;

    actions.swap(g_state.pending_actions);

    for (const PendingAction& action : actions)
    {
        if (!IsChatDocument(action.document))
        {
            continue;
        }

        switch (action.type)
        {
            case ActionType::ToggleInput:
            {
                ToggleInputBar(action.document);
                break;
            }

            case ActionType::SendMessage:
            {
                SendMessage(action.document);
                break;
            }

            case ActionType::StartEdit:
            {
                StartEdit(
                    action.document,
                    action.bubble);
                break;
            }
        }
    }

    // Protect against actions added while processing.

    if (!g_state.pending_actions.empty() &&
        !g_state.action_task_pending)
    {
        g_state.action_task_pending = true;

        if (!Ultron_PostToUI(&ProcessPendingActions, nullptr))
        {
            g_state.action_task_pending = false;
            g_state.pending_actions.clear();
        }
    }
}


// =============================================================
// LONG PRESS COMPLETION
// =============================================================

void CompleteLongPress()
{
    if (!g_state.press_active ||
        g_state.press_target == nullptr ||
        g_state.press_source == PressSource::None)
    {
        return;
    }

    const Uint64 elapsed =
        SDL_GetTicks64() - g_state.press_started_at;

    if (elapsed < kLongPressDuration)
    {
        return;
    }

    Rml::Element* bubble =
        g_state.press_target;

    Rml::ElementDocument* document =
        bubble->GetOwnerDocument();

    CancelLongPress();

    if (document != nullptr)
    {
        StartEdit(document, bubble);
    }
}


// =============================================================
// LONG PRESS TIMER TASK
// =============================================================

void LongPressTimeoutTask(void*)
{
    const std::uint64_t expired =
        g_state.expired_generation.exchange(
            0,
            std::memory_order_acq_rel);

    if (expired == 0)
    {
        return;
    }

    const std::uint64_t current =
        g_state.press_generation.load(
            std::memory_order_acquire);

    if (expired != current ||
        !g_state.press_active)
    {
        return;
    }

    CompleteLongPress();
}


// =============================================================
// START LONG PRESS TIMER
// =============================================================

void BeginLongPress(
    Rml::Element* bubble,
    PressSource source)
{
    if (bubble == nullptr)
    {
        return;
    }

    CancelLongPress();

    g_state.press_target = bubble;

    g_state.press_source = source;

    g_state.press_started_at = SDL_GetTicks64();

    g_state.press_active = true;

    const std::uint64_t generation =
        g_state.press_generation.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;

    // One cancellable worker handles the 550ms delay.
    // All RmlUi modifications happen later on the UI thread.

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

            g_state.expired_generation.store(
                generation,
                std::memory_order_release);

            if (!Ultron_PostToUI(
                    &LongPressTimeoutTask,
                    nullptr))
            {
                g_state.expired_generation.store(
                    0,
                    std::memory_order_release);
            }
        });
}


// =============================================================
// MOUSE PRESS HANDLING
// =============================================================

void HandleMouseDown(Rml::Event& event)
{
    // Only the primary mouse button starts the timer.

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
        return;
    }

    Rml::Element* bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            document);

    if (bubble != nullptr)
    {
        BeginLongPress(
            bubble,
            PressSource::Mouse);
    }
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
        SDL_GetTicks64() - g_state.press_started_at;

    if (elapsed >= kLongPressDuration &&
        bubble != nullptr &&
        document != nullptr)
    {
        CancelLongPress();

        QueueAction(
            ActionType::StartEdit,
            document,
            bubble);
    }
    else
    {
        CancelLongPress();
    }
}


void HandleMouseMove(Rml::Event& event)
{
    if (!g_state.press_active ||
        g_state.press_source != PressSource::Mouse)
    {
        return;
    }

    Rml::Element* bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            g_state.press_target->GetOwnerDocument());

    // Equivalent to pointerleave cancellation.

    if (bubble != g_state.press_target)
    {
        CancelLongPress();
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
    // DOCUMENT SHOW / HIDE
    // ---------------------------------------------------------

    if (event_name == "show")
    {
        if (IsChatDocument(document))
        {
            g_state.document = document;
        }

        return false;
    }

    if (event_name == "hide")
    {
        if (document == g_state.document)
        {
            CancelLongPress();

            g_state.document = nullptr;
        }

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
    // INPUT BAR CLICK
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
    // LONG PRESS MOUSE EVENTS
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

        if ((key == Rml::Input::KI_RETURN ||
             key == Rml::Input::KI_NUMPADENTER) &&
            !shift)
        {
            // Prevent the textarea newline.

            QueueAction(
                ActionType::SendMessage,
                document);

            return true;
        }

        // Schedule after the RmlUi default keyboard action.

        QueueInputUpdate(document);

        return false;
    }

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
// TOUCH HIT TESTING
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
        event.tfinger.x * static_cast<float>(dimensions.x),
        event.tfinger.y * static_cast<float>(dimensions.y)
    };
}


Rml::Element* FindTouchTarget(
    Rml::ElementDocument* document,
    Rml::Vector2f position)
{
    if (!IsChatDocument(document) ||
        !document->IsVisible())
    {
        return nullptr;
    }

    // Wing hit area.

    Rml::Element* wing =
        GetElement(document, "wing-hit");

    if (wing != nullptr &&
        wing->IsVisible(true) &&
        wing->IsPointWithinElement(position))
    {
        return wing;
    }

    // Send button.

    Rml::Element* send =
        GetElement(document, "send-button");

    if (send != nullptr &&
        send->IsVisible(true) &&
        send->IsPointWithinElement(position))
    {
        return send;
    }

    // Chat bubbles: inspect from newest to oldest.

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr ||
        !chat_area->IsPointWithinElement(position))
    {
        return nullptr;
    }

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

    g_state.touch_inside = false;

    g_state.touch_target = nullptr;
}


// =============================================================
// RAW SDL TOUCH EVENT HANDLER
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
            // Handle one interaction pointer for this feature.
            if (g_state.touch_active)
            {
                return false;
            }

            document->UpdateDocument();

            const Rml::Vector2f position =
                GetTouchPosition(*event, document);

            Rml::Element* target =
                FindTouchTarget(document, position);

            g_state.touch_active = true;

            g_state.touch_inside = (target != nullptr);

            g_state.touch_id = event->tfinger.touchID;

            g_state.finger_id = event->tfinger.fingerID;

            g_state.touch_target = target;

            // Start long press only on a chat bubble.

            Rml::Element* bubble =
                FindMessageBubble(target, document);

            if (bubble != nullptr)
            {
                BeginLongPress(
                    bubble,
                    PressSource::Touch);
            }

            // Preserve normal RmlUi scrolling and touch handling.

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

            if (!g_state.touch_inside ||
                g_state.touch_target == nullptr)
            {
                return false;
            }

            const Rml::Vector2f position =
                GetTouchPosition(*event, document);

            // Leaving the pressed element cancels long press.

            if (!g_state.touch_target->IsPointWithinElement(position))
            {
                g_state.touch_inside = false;

                if (g_state.press_source == PressSource::Touch)
                {
                    CancelLongPress();
                }
            }

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

            Rml::Element* target =
                g_state.touch_target;

            const bool inside =
                g_state.touch_inside &&
                target != nullptr &&
                target->IsPointWithinElement(position);

            // Complete a long press if release occurs after 550ms,
            // even when the timer task is still waiting in the UI queue.

            if (g_state.press_source == PressSource::Touch &&
                g_state.press_active &&
                SDL_GetTicks64() - g_state.press_started_at >=
                    kLongPressDuration)
            {
                Rml::Element* bubble =
                    g_state.press_target;

                CancelLongPress();

                if (bubble != nullptr)
                {
                    QueueAction(
                        ActionType::StartEdit,
                        document,
                        bubble);
                }
            }
            else if (
                g_state.press_source == PressSource::Touch)
            {
                CancelLongPress();
            }

            // Process a normal touch tap.

            if (inside && target != nullptr)
            {
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
// AUTOMATIC MODULE REGISTRATION
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


void RegisterObsidianWingChat()
{
    // Document lifecycle.

    RegisterRmlEvent("show");
    RegisterRmlEvent("hide");

    // Mouse events.

    RegisterRmlEvent("click");
    RegisterRmlEvent("mousedown");
    RegisterRmlEvent("mouseup");
    RegisterRmlEvent("mousemove");

    // Keyboard and form-control events.

    RegisterRmlEvent("keydown");
    RegisterRmlEvent("keyup");
    RegisterRmlEvent("textinput");
    RegisterRmlEvent("change");

    // Native Android / SDL touch events.

    RegisterSDLTouchEvent(SDL_EVENT_FINGER_DOWN);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_MOTION);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_UP);
    RegisterSDLTouchEvent(SDL_EVENT_FINGER_CANCELED);
}


// Static registration: called before main() initializes RmlUi.
// main.cpp will attach these handlers when its RmlUi context is ready.

struct ObsidianWingChatRegistration
{
    ObsidianWingChatRegistration()
    {
        RegisterObsidianWingChat();
    }

    ~ObsidianWingChatRegistration()
    {
        CancelLongPress();
    }
};

[[maybe_unused]]
ObsidianWingChatRegistration g_registration;

} // namespace
