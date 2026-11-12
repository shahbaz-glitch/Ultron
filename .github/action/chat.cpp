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

// Responsive dimensions.
//
// One unit represents 1% of the shorter viewport side.
// Portrait: equivalent to vw.
// Landscape: equivalent to vh.
//
// These values match the responsive Obsidian Wing Chat RML.

constexpr float kInitialInputUnits = 7.5f;
constexpr float kMaximumInputUnits = 30.0f;

constexpr float kMinimumBarUnits = 15.0f;
constexpr float kBarExtraUnits = 8.7f;

constexpr float kChatBottomUnits = 11.7f;


// =============================================================
// INPUT SOURCE
// =============================================================

enum class PressSource
{
    None,
    Mouse
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
// LONG-PRESS TICKET
// =============================================================

// Every timer posts its own generation ticket.
//
// This avoids a shared expired-generation variable being
// overwritten by an older timer after a newer press starts.

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

    // The selected message is temporarily detached while editing.
    Rml::ElementPtr editing_bubble;

    // Stores the original text for every generated chat bubble.
    std::unordered_map<Rml::Element*, std::string> bubble_messages;

    // All actions are queued and processed on the UI thread.
    std::vector<PendingAction> pending_actions;

    bool action_task_pending = false;

    bool update_task_pending = false;

    Rml::ElementDocument* pending_update_document = nullptr;


    // Long-press state.
    //
    // These non-atomic fields are accessed only on the UI thread.

    std::atomic_uint64_t press_generation{0};

    bool press_active = false;

    PressSource press_source = PressSource::None;

    Rml::Element* press_target = nullptr;

    Uint64 press_started_at = 0;
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

    // The RML element is declared as <textarea>.
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
// UTF-8 WHITESPACE HANDLING
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

    // Preserve malformed UTF-8 bytes instead of reading past them.
    length = 1;
    return first;
}


std::string TrimWhitespace(std::string value)
{
    std::size_t begin = 0;
    std::size_t end = value.size();

    // Trim from the beginning.

    while (begin < end)
    {
        std::size_t length = 1;

        const std::uint32_t codepoint =
            DecodeUtf8(value, begin, length);

        if (!IsUnicodeWhitespace(codepoint))
        {
            break;
        }

        begin += length;
    }

    // Trim from the end.

    while (end > begin)
    {
        std::size_t position = end - 1;

        // Move to the first byte of the UTF-8 sequence.
        while (position > begin &&
            (static_cast<unsigned char>(
                value[position]) & 0xC0) == 0x80)
        {
            --position;
        }

        std::size_t length = 1;

        const std::uint32_t codepoint =
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

    // The shorter side matches:
    // portrait -> viewport width
    // landscape -> viewport height

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

    const float extra_bar_height =
        kBarExtraUnits * unit;

    const float chat_bottom_offset =
        kChatBottomUnits * unit;


    // ---------------------------------------------------------
    // TEXTAREA MEASUREMENT
    // ---------------------------------------------------------

    // Reset the textarea to its responsive base height.

    message_input->SetProperty(
        "height",
        ToPixels(initial_height));

    // Recalculate the layout before measuring content.

    document->UpdateDocument();

    const float content_height = std::max(
        message_input->GetScrollHeight(),
        initial_height);

    const float new_height = std::clamp(
        content_height,
        initial_height,
        maximum_height);


    // ---------------------------------------------------------
    // APPLY TEXTAREA DIMENSIONS
    // ---------------------------------------------------------

    message_input->SetProperty(
        "height",
        ToPixels(new_height));

    message_input->SetProperty(
        "margin-top",
        ToPixels(-new_height * 0.5f));


    // ---------------------------------------------------------
    // INPUT BAR DIMENSIONS
    // ---------------------------------------------------------

    const bool expanded =
        input_bar->IsClassSet("expanded");

    // Keep the collapsed wing button square and stable.
    //
    // Only the expanded input bar grows with its textarea.

    const float bar_height = expanded
        ? std::max(
            minimum_bar_height,
            new_height + extra_bar_height)
        : minimum_bar_height;

    input_bar->SetProperty(
        "height",
        ToPixels(bar_height));


    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    // Responsive baseline:
    // 15.0 units bar + 11.7 units offset = 26.7 units.

    chat_area->SetProperty(
        "bottom",
        ToPixels(bar_height + chat_bottom_offset));


    // ---------------------------------------------------------
    // PULSE / SEND ARROW STATE
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

    // Recalculate bar height and chat position immediately
    // through this already deferred UI action.

    UpdateInput(document);
}


// =============================================================
// BUBBLE TEXT MANAGEMENT
// =============================================================

void ReplaceBubbleText(
    Rml::ElementDocument* document,
    Rml::Element* bubble,
    const std::string& message)
{
    if (document == nullptr ||
        bubble == nullptr)
    {
        return;
    }

    Rml::ElementPtr text_node =
        document->CreateTextNode(message);

    if (!text_node)
    {
        return;
    }

    // Remove old text/content safely.

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
    // Invalidate every previously scheduled timer task.

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
    if (document == nullptr ||
        bubble == nullptr)
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


    // ---------------------------------------------------------
    // RETRIEVE ORIGINAL MESSAGE
    // ---------------------------------------------------------

    std::string message;

    const auto found =
        g_state.bubble_messages.find(bubble);

    if (found != g_state.bubble_messages.end())
    {
        message = found->second;
    }


    // ---------------------------------------------------------
    // REMOVE ALL MESSAGES BELOW THE SELECTED MESSAGE
    // ---------------------------------------------------------

    Rml::Element* next =
        bubble->GetNextSibling();

    while (next != nullptr)
    {
        Rml::Element* following =
            next->GetNextSibling();

        g_state.bubble_messages.erase(next);

        // The returned ElementPtr owns the removed element
        // and destroys it at the end of this iteration.

        Rml::ElementPtr removed =
            chat_area->RemoveChild(next);

        next = following;
    }


    // ---------------------------------------------------------
    // DISCARD PREVIOUS EDITING BUBBLE
    // ---------------------------------------------------------

    if (g_state.editing_bubble)
    {
        g_state.bubble_messages.erase(
            g_state.editing_bubble.get());

        g_state.editing_bubble.reset();
    }


    // ---------------------------------------------------------
    // DETACH SELECTED BUBBLE
    // ---------------------------------------------------------

    g_state.editing_bubble =
        chat_area->RemoveChild(bubble);

    if (!g_state.editing_bubble)
    {
        return;
    }


    // ---------------------------------------------------------
    // RESTORE MESSAGE INTO INPUT
    // ---------------------------------------------------------

    message_input->SetValue(message);


    // ---------------------------------------------------------
    // EXPAND INPUT BAR
    // ---------------------------------------------------------

    input_bar->SetClass(
        "expanded",
        true);


    // ---------------------------------------------------------
    // UPDATE RESPONSIVE HEIGHT AND POSITION
    // ---------------------------------------------------------

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
    // UPDATE INPUT BAR
    // ---------------------------------------------------------

    UpdateInput(document);


    // ---------------------------------------------------------
    // SCROLL TO THE LATEST MESSAGE
    // ---------------------------------------------------------

    document->UpdateDocument();

    chat_area->SetScrollTop(
        chat_area->GetScrollHeight());
}


// =============================================================
// PENDING ACTION EXECUTOR
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


    // If callbacks queued more actions while this batch
    // was executing, make sure another UI task is scheduled.

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

    // Reject a task belonging to an older press.

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

    // Invalidate the timer before queuing the edit.

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
    // The task owns its ticket.

    std::unique_ptr<LongPressTicket> ticket(
        static_cast<LongPressTicket*>(user_data));

    if (!ticket)
    {
        return;
    }

    const std::uint64_t generation =
        ticket->generation;

    // Ignore stale tasks from previous presses.

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
// START LONG-PRESS TIMER
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

    // Cancel any previous press first.

    CancelLongPress();


    // ---------------------------------------------------------
    // INITIALIZE PRESS STATE
    // ---------------------------------------------------------

    g_state.press_target = bubble;

    g_state.press_source = source;

    g_state.press_started_at =
        SDL_GetTicks();

    g_state.press_active = true;


    const std::uint64_t generation =
        g_state.press_generation.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;


    // ---------------------------------------------------------
    // START CANCELLABLE WORKER
    // ---------------------------------------------------------

    g_long_press_worker = std::jthread(
        [generation](std::stop_token stop_token)
        {
            std::mutex mutex;

            std::condition_variable_any condition;

            std::unique_lock lock(mutex);

            // Wait for the long-press duration or cancellation.

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


            // Reject an outdated worker.

            if (g_state.press_generation.load(
                    std::memory_order_acquire) != generation)
            {
                return;
            }


            // Each worker posts a separate generation ticket.
            // No shared expiration slot can overwrite a newer timer.

            auto* ticket =
                new (std::nothrow) LongPressTicket{
                    generation
                };

            if (ticket == nullptr)
            {
                return;
            }


            // The actual UI operation happens later on the UI thread.

            if (!Ultron_PostToUI(
                    &LongPressTimeoutTask,
                    ticket))
            {
                delete ticket;
            }
        });
}


// =============================================================
// MOUSE PRESS HANDLING
// =============================================================

void HandleMouseDown(Rml::Event& event)
{
    // Only primary mouse button.

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

    // Pressing any other UI area cancels an old timer.

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


    // Verify that release happened over the same bubble.

    Rml::Element* release_bubble =
        FindMessageBubble(
            event.GetTargetElement(),
            document);

    const bool released_inside =
        bubble != nullptr &&
        release_bubble == bubble;


    const bool completed =
        elapsed >= kLongPressDuration &&
        released_inside;


    CancelLongPress();


    // If the timer callback is still waiting in the UI queue,
    // release can still complete the long-press reliably.

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

    // Moving outside the original bubble cancels the timer.

    if (current_bubble != pressed_bubble)
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
    // DOCUMENT SHOW
    // ---------------------------------------------------------

    if (event_name == "show")
    {
        if (IsChatDocument(document))
        {
            g_state.document = document;

            // Initialize dimensions using the current viewport.

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

            g_state.document = nullptr;
        }

        // Remove pending input updates for this document.

        if (g_state.pending_update_document == document)
        {
            g_state.pending_update_document = nullptr;
        }

        // Remove pending actions belonging to this document.

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
    // CLICK EVENTS
    // ---------------------------------------------------------

    if (event_name == "click")
    {
        if (event->GetParameter<int>("button", 0) != 0)
        {
            return false;
        }

        // Wing button.

        if (FindAncestorById(
                target,
                "wing-hit") != nullptr)
        {
            QueueAction(
                ActionType::ToggleInput,
                document);
        }

        // Send button.

        else if (FindAncestorById(
                    target,
                    "send-button") != nullptr)
        {
            QueueAction(
                ActionType::SendMessage,
                document);
        }

        return false;
    }


    // ---------------------------------------------------------
    // LONG-PRESS MOUSE EVENTS
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
        FindAncestorById(
            target,
            "message-input");

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


        // Enter sends the message.
        // Shift + Enter inserts a newline.

        if ((key == Rml::Input::KI_RETURN ||
             key == Rml::Input::KI_NUMPADENTER) &&
            !shift)
        {
            QueueAction(
                ActionType::SendMessage,
                document);

            // Prevent the default textarea newline.

            return true;
        }

        return false;
    }


    // Schedule after RmlUi applies the text change.

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


void RegisterObsidianWingChat()
{
    // SDL touch generates synthetic mouse events.
    //
    // This module handles interaction through one RmlUi
    // mouse-event path, so raw SDL finger handlers are not
    // registered separately.

    SDL_SetHint(
        SDL_HINT_TOUCH_MOUSE_EVENTS,
        "1");


    // ---------------------------------------------------------
    // DOCUMENT LIFECYCLE
    // ---------------------------------------------------------

    RegisterRmlEvent("show");
    RegisterRmlEvent("hide");


    // ---------------------------------------------------------
    // MOUSE EVENTS
    // ---------------------------------------------------------

    RegisterRmlEvent("click");
    RegisterRmlEvent("mousedown");
    RegisterRmlEvent("mouseup");
    RegisterRmlEvent("mousemove");


    // ---------------------------------------------------------
    // KEYBOARD AND TEXT EVENTS
    // ---------------------------------------------------------

    RegisterRmlEvent("keydown");
    RegisterRmlEvent("keyup");
    RegisterRmlEvent("textinput");
    RegisterRmlEvent("change");
}


// =============================================================
// AUTOMATIC REGISTRATION LIFETIME
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
    }
};


[[maybe_unused]]
ObsidianWingChatRegistration g_registration;


} // namespace
