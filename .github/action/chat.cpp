
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

// Responsive viewport units.
// 1 unit = 1% of the shorter context dimension.
//
// Portrait  -> vw
// Landscape -> vh

constexpr float kInitialInputUnits = 7.5f;
constexpr float kMaximumInputUnits = 30.0f;

constexpr float kMinimumBarUnits = 15.0f;

constexpr float kInputBottomUnits = 9.0f;
constexpr float kChatGapUnits = 11.7f;

constexpr float kBubbleMaxWidthPercent = 78.0f;
constexpr float kBubbleRightMarginPercent = 1.25f;

constexpr float kLongPressMoveUnits = 2.0f;


// =============================================================
// ACTION TYPES
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
// LONG-PRESS TIMER TICKET
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

    std::unordered_map<
        Rml::Element*,
        std::string
    > bubble_messages;


    // UI action queue.

    std::vector<PendingAction> pending_actions;

    bool action_task_pending = false;


    // Input update queue.

    bool update_task_pending = false;

    Rml::ElementDocument* pending_update_document = nullptr;


    // Long-press state.

    std::atomic_uint64_t press_generation{0};

    bool press_active = false;

    Rml::Element* press_target = nullptr;

    Uint64 press_started_at = 0;


    // Native touch observation.

    bool touch_active = false;

    bool touch_moved = false;

    bool touch_long_press_fired = false;

    SDL_TouchID touch_id = 0;

    SDL_FingerID finger_id = 0;

    Rml::ElementDocument* touch_document = nullptr;

    Rml::Element* touch_start_target = nullptr;

    Rml::Vector2f touch_start_position{};

};


ChatState g_state;


// =============================================================
// LONG-PRESS WORKER
// =============================================================

std::jthread g_long_press_worker;


// =============================================================
// FORWARD DECLARATIONS
// =============================================================

void ProcessPendingActions(void*);

void UpdateInputTask(void*);

void LongPressTimeoutTask(void*);


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


bool IsChatDocument(
    Rml::ElementDocument* document)
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

    while (element != nullptr &&
           element != chat_area)
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


Rml::String ToPercent(float value)
{
    char buffer[64]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3f%%",
        static_cast<double>(value));

    return buffer;
}


// =============================================================
// RESPONSIVE VIEWPORT UNIT
// =============================================================

float GetViewportUnit(
    Rml::ElementDocument* document)
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

    const int shorter_side =
        std::min(dimensions.x, dimensions.y);

    if (shorter_side <= 0)
    {
        return 0.0f;
    }

    return static_cast<float>(shorter_side) / 100.0f;
}


// =============================================================
// DOCUMENT CONFIGURATION
// =============================================================

void ConfigureDocument(
    Rml::ElementDocument* document)
{
    if (!IsChatDocument(document))
    {
        return;
    }

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    if (chat_area != nullptr)
    {
        // Chat history is the only history scroll container.

        chat_area->SetProperty(
            "overflow-x",
            "hidden");

        chat_area->SetProperty(
            "overflow-y",
            "auto");

        chat_area->SetProperty(
            "overscroll-behavior",
            "contain");
    }

    if (input_bar != nullptr)
    {
        // Do not force either icon to hidden here.
        // RML CSS controls pulse/arrow using has-text.
        // UpdateInput will synchronize that class.
    }
}


// =============================================================
// CHAT BUBBLE LAYOUT
// =============================================================

void ConfigureBubbleLayout(
    Rml::Element* bubble)
{
    if (bubble == nullptr)
    {
        return;
    }

    bubble->SetProperty(
        "margin-left",
        "auto");

    bubble->SetProperty(
        "margin-right",
        ToPercent(kBubbleRightMarginPercent));

    bubble->SetProperty(
        "max-width",
        ToPercent(kBubbleMaxWidthPercent));
}


// =============================================================
// RESPONSIVE INPUT UPDATE
// =============================================================

void UpdateInput(
    Rml::ElementDocument* document)
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

    const float input_bottom =
        kInputBottomUnits * unit;

    const float chat_gap =
        kChatGapUnits * unit;


    // ---------------------------------------------------------
    // MEASURE TEXTAREA CONTENT
    // ---------------------------------------------------------

    // Start with the original single-line height.

    message_input->SetProperty(
        "height",
        ToPixels(initial_height));

    document->UpdateDocument();

    const float content_height =
        message_input->GetScrollHeight();

    const float new_height = std::clamp(
        std::max(content_height, initial_height),
        initial_height,
        maximum_height);


    // ---------------------------------------------------------
    // TEXTAREA AUTO-GROW
    // ---------------------------------------------------------

    message_input->SetProperty(
        "height",
        ToPixels(new_height));

    message_input->SetProperty(
        "margin-top",
        ToPixels(-new_height * 0.5f));


    // ---------------------------------------------------------
    // INPUT PANEL AUTO-GROW
    // ---------------------------------------------------------

    // Collapsed or expanded horizontally:
    // base height remains unchanged when there is one line.
    //
    // Each additional text line adds only its extra height.
    // The bottom edge remains anchored.

    const float additional_height =
        std::max(
            0.0f,
            new_height - initial_height);

    const float bar_height =
        minimum_bar_height + additional_height;

    input_bar->SetProperty(
        "height",
        ToPixels(bar_height));


    // ---------------------------------------------------------
    // KEYBOARD GAP
    // ---------------------------------------------------------

    input_bar->SetProperty(
        "bottom",
        ToPixels(input_bottom));


    // ---------------------------------------------------------
    // CHAT POSITION
    // ---------------------------------------------------------

    chat_area->SetProperty(
        "bottom",
        ToPixels(bar_height + chat_gap));


    // ---------------------------------------------------------
    // PULSE / ARROW STATE
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


void QueueInputUpdate(
    Rml::ElementDocument* document)
{
    if (!IsChatDocument(document) ||
        !document->IsVisible())
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
// INPUT BAR TOGGLE
// =============================================================

void ToggleInputBar(
    Rml::ElementDocument* document)
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

    // Horizontal expansion never changes the empty base height.

    UpdateInput(document);
}


// =============================================================
// BUBBLE TEXT REPLACEMENT
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
    // Invalidate every outstanding timer callback.

    g_state.press_generation.fetch_add(
        1,
        std::memory_order_acq_rel);

    g_state.press_active = false;

    g_state.press_target = nullptr;

    if (g_long_press_worker.joinable())
    {
        g_long_press_worker.request_stop();
    }
}


// =============================================================
// TOUCH STATE CLEANUP
// =============================================================

void ResetTouchState()
{
    CancelLongPress();

    g_state.touch_active = false;

    g_state.touch_moved = false;

    g_state.touch_long_press_fired = false;

    g_state.touch_id = 0;

    g_state.finger_id = 0;

    g_state.touch_document = nullptr;

    g_state.touch_start_target = nullptr;

    g_state.touch_start_position = {};
}


// =============================================================
// TOUCH COORDINATES
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
        return {};
    }

    const Rml::Vector2i dimensions =
        context->GetDimensions();

    // SDL finger positions are normalized between 0 and 1.
    // Convert to RmlUi context pixel coordinates.

    return Rml::Vector2f{
        event.tfinger.x *
            static_cast<float>(dimensions.x),

        event.tfinger.y *
            static_cast<float>(dimensions.y)
    };
}


// =============================================================
// TOUCH HIT TEST
// =============================================================

Rml::Element* FindTouchTarget(
    Rml::ElementDocument* document,
    Rml::Vector2f position)
{
    if (!IsChatDocument(document) ||
        !document->IsVisible())
    {
        return nullptr;
    }

    Rml::Element* input_bar =
        GetElement(document, "input-bar");

    Rml::Element* send_button =
        GetElement(document, "send-button");


    // ---------------------------------------------------------
    // SEND BUTTON
    // ---------------------------------------------------------

    // It is actionable only after text has been entered.

    if (input_bar != nullptr &&
        send_button != nullptr &&
        input_bar->IsClassSet("expanded") &&
        input_bar->IsClassSet("has-text") &&
        send_button->IsVisible(true) &&
        send_button->IsPointWithinElement(position))
    {
        return send_button;
    }


    // ---------------------------------------------------------
    // WING BUTTON
    // ---------------------------------------------------------

    Rml::Element* wing =
        GetElement(document, "wing-hit");

    if (wing != nullptr &&
        wing->IsVisible(true) &&
        wing->IsPointWithinElement(position))
    {
        return wing;
    }


    // ---------------------------------------------------------
    // CHAT MESSAGE
    // ---------------------------------------------------------

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr ||
        !chat_area->IsPointWithinElement(position))
    {
        return nullptr;
    }

    for (Rml::Element* child =
            chat_area->GetLastChild();
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
// LONG-PRESS MOVEMENT TOLERANCE
// =============================================================

float GetLongPressMovementTolerance(
    Rml::ElementDocument* document)
{
    return std::max(
        6.0f,
        kLongPressMoveUnits *
            GetViewportUnit(document));
}


// =============================================================
// TOUCH MOVEMENT DISTANCE
// =============================================================

float GetTouchMovementDistance(
    Rml::Vector2f first,
    Rml::Vector2f second)
{
    const float dx = second.x - first.x;

    const float dy = second.y - first.y;

    return std::sqrt(
        dx * dx + dy * dy);
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
    // GET ORIGINAL MESSAGE
    // ---------------------------------------------------------

    std::string message;

    const auto found =
        g_state.bubble_messages.find(bubble);

    if (found != g_state.bubble_messages.end())
    {
        message = found->second;
    }


    // ---------------------------------------------------------
    // REMOVE ALL MESSAGES BELOW SELECTED MESSAGE
    // ---------------------------------------------------------

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


    // ---------------------------------------------------------
    // DISCARD PREVIOUS DETACHED EDIT
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
    // RESTORE INPUT TEXT
    // ---------------------------------------------------------

    message_input->SetValue(message);

    input_bar->SetClass(
        "expanded",
        true);

    UpdateInput(document);

    document->UpdateDocument();
}


// =============================================================
// CHAT SCROLL HELPERS
// =============================================================

bool IsNearChatBottom(
    Rml::Element* chat_area,
    float threshold)
{
    if (chat_area == nullptr)
    {
        return true;
    }

    const float maximum_scroll =
        std::max(
            0.0f,
            chat_area->GetScrollHeight() -
            chat_area->GetClientHeight());

    const float remaining =
        maximum_scroll -
        chat_area->GetScrollTop();

    return remaining <= threshold;
}


void ScrollChatToBottom(
    Rml::Element* chat_area)
{
    if (chat_area == nullptr)
    {
        return;
    }

    const Rml::Vector2f destination{
        0.0f,
        chat_area->GetScrollHeight()
    };

    // Smooth programmatic scrolling.

    chat_area->ScrollTo(
        destination,
        Rml::ScrollBehavior::Smooth);
}


// =============================================================
// MESSAGE SENDING
// =============================================================

void SendMessage(
    Rml::ElementDocument* document)
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
    // DETECT CURRENT SCROLL POSITION
    // ---------------------------------------------------------

    document->UpdateDocument();

    const float unit =
        GetViewportUnit(document);

    // Preserve manually selected history position.
    // Follow new messages only when already near the bottom.

    const bool should_follow_bottom =
        IsNearChatBottom(
            chat_area,
            std::max(8.0f, unit * 3.0f));


    // ---------------------------------------------------------
    // EXISTING MESSAGE EDIT
    // ---------------------------------------------------------

    if (g_state.editing_bubble)
    {
        Rml::Element* bubble =
            g_state.editing_bubble.get();

        ReplaceBubbleText(
            document,
            bubble,
            message);

        ConfigureBubbleLayout(bubble);

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
    // NEW MESSAGE
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

        ConfigureBubbleLayout(
            bubble.get());

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
    // RESET INPUT HEIGHT / ARROW STATE
    // ---------------------------------------------------------

    UpdateInput(document);


    // ---------------------------------------------------------
    // UPDATE CHAT LAYOUT
    // ---------------------------------------------------------

    document->UpdateDocument();


    // ---------------------------------------------------------
    // FOLLOW NEW MESSAGE
    // ---------------------------------------------------------

    if (should_follow_bottom)
    {
        ScrollChatToBottom(chat_area);
    }
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


    // Process actions added while the current batch was running.

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

void CompleteLongPress(
    std::uint64_t generation)
{
    if (!g_state.touch_active ||
        g_state.touch_moved ||
        g_state.touch_long_press_fired ||
        !g_state.press_active ||
        g_state.press_target == nullptr)
    {
        return;
    }

    if (generation !=
        g_state.press_generation.load(
            std::memory_order_acquire))
    {
        return;
    }

    const Uint64 elapsed =
        SDL_GetTicks() -
        g_state.press_started_at;

    if (elapsed < kLongPressDuration)
    {
        return;
    }

    Rml::Element* bubble =
        g_state.press_target;

    Rml::ElementDocument* document =
        g_state.touch_document;

    Rml::Element* chat_area =
        GetElement(document, "chat-area");

    if (chat_area == nullptr ||
        bubble->GetParentNode() != chat_area)
    {
        CancelLongPress();
        return;
    }


    // Prevent the finger release from becoming a normal tap.

    g_state.touch_long_press_fired = true;

    CancelLongPress();

    QueueAction(
        ActionType::StartEdit,
        document,
        bubble);
}


// =============================================================
// LONG-PRESS TIMER CALLBACK
// =============================================================

void LongPressTimeoutTask(void* user_data)
{
    std::unique_ptr<LongPressTicket> ticket(
        static_cast<LongPressTicket*>(user_data));

    if (!ticket)
    {
        return;
    }

    CompleteLongPress(
        ticket->generation);
}


// =============================================================
// START LONG-PRESS TIMER
// =============================================================

void BeginLongPress(
    Rml::Element* bubble)
{
    if (bubble == nullptr)
    {
        return;
    }

    CancelLongPress();

    g_state.press_target = bubble;

    g_state.press_started_at =
        SDL_GetTicks();

    g_state.press_active = true;

    const std::uint64_t generation =
        g_state.press_generation.fetch_add(
            1,
            std::memory_order_acq_rel) + 1;


    // The worker waits only.
    // All UI operations remain on the UI thread.

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
// TOUCH STATE RESET
// =============================================================

void ResetTouchState()
{
    CancelLongPress();

    g_state.touch_active = false;

    g_state.touch_moved = false;

    g_state.touch_long_press_fired = false;

    g_state.touch_id = 0;

    g_state.finger_id = 0;

    g_state.touch_document = nullptr;

    g_state.touch_start_target = nullptr;

    g_state.touch_start_position = {};
}


// =============================================================
// NATIVE SDL TOUCH OBSERVER
// =============================================================

// IMPORTANT:
//
// RmlUi_Platform_SDL is responsible for sending native touch
// input into the RmlUi context exactly once.
//
// This module only observes the same raw SDL events for:
// - Wing taps
// - Send taps
// - 550ms message long-press editing
//
// Do not call Context::ProcessTouchStart/Move/End here.
// The platform backend already processes the scrolling gesture.

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


    // ---------------------------------------------------------
    // FINGER DOWN
    // ---------------------------------------------------------

    if (event->type == SDL_EVENT_FINGER_DOWN)
    {
        if (g_state.touch_active)
        {
            return false;
        }

        document->UpdateDocument();

        const Rml::Vector2f position =
            GetTouchPosition(*event, document);


        g_state.touch_active = true;

        g_state.touch_moved = false;

        g_state.touch_long_press_fired = false;

        g_state.touch_id =
            event->tfinger.touchID;

        g_state.finger_id =
            event->tfinger.fingerID;

        g_state.touch_document =
            document;

        g_state.touch_start_position =
            position;


        // Determine the element under the initial touch.

        Rml::Element* target =
            FindTouchTarget(
                document,
                position);

        g_state.touch_start_target =
            target;


        // Start long-press only for chat bubbles.

        Rml::Element* bubble =
            FindMessageBubble(
                target,
                document);

        if (bubble != nullptr)
        {
            BeginLongPress(bubble);
        }
        else
        {
            CancelLongPress();
        }

        return false;
    }


    // ---------------------------------------------------------
    // FINGER MOTION
    // ---------------------------------------------------------

    if (event->type == SDL_EVENT_FINGER_MOTION)
    {
        if (!g_state.touch_active ||
            event->tfinger.touchID != g_state.touch_id ||
            event->tfinger.fingerID != g_state.finger_id ||
            g_state.touch_document != document)
        {
            return false;
        }

        const Rml::Vector2f position =
            GetTouchPosition(*event, document);

        const float movement =
            GetTouchMovementDistance(
                g_state.touch_start_position,
                position);

        const float tolerance =
            GetLongPressMovementTolerance(document);

        // Dragging the chat history must never trigger editing
        // or a normal tap on release.

        if (movement > tolerance)
        {
            g_state.touch_moved = true;

            if (g_state.press_active)
            {
                CancelLongPress();
            }
        }

        return false;
    }


    // ---------------------------------------------------------
    // FINGER UP
    // ---------------------------------------------------------

    if (event->type == SDL_EVENT_FINGER_UP)
    {
        if (!g_state.touch_active ||
            event->tfinger.touchID != g_state.touch_id ||
            event->tfinger.fingerID != g_state.finger_id ||
            g_state.touch_document != document)
        {
            return false;
        }

        const Rml::Vector2f position =
            GetTouchPosition(*event, document);


        // Final movement validation.

        const float movement =
            GetTouchMovementDistance(
                g_state.touch_start_position,
                position);

        if (movement >
            GetLongPressMovementTolerance(document))
        {
            g_state.touch_moved = true;

            if (g_state.press_active)
            {
                CancelLongPress();
            }
        }


        // Complete long-press on release if the worker callback
        // has not yet reached the UI queue.

        if (g_state.press_active &&
            !g_state.touch_moved &&
            !g_state.touch_long_press_fired)
        {
            const std::uint64_t generation =
                g_state.press_generation.load(
                    std::memory_order_acquire);

            CompleteLongPress(generation);
        }


        // Find the release target.

        Rml::Element* release_target =
            FindTouchTarget(
                document,
                position);


        // -----------------------------------------------------
        // NORMAL TAP
        // -----------------------------------------------------

        if (!g_state.touch_long_press_fired &&
            !g_state.touch_moved &&
            release_target != nullptr &&
            release_target == g_state.touch_start_target)
        {
            if (FindAncestorById(
                    release_target,
                    "wing-hit") != nullptr)
            {
                QueueAction(
                    ActionType::ToggleInput,
                    document);
            }
            else if (
                release_target ==
                    GetElement(document, "send-button"))
            {
                QueueAction(
                    ActionType::SendMessage,
                    document);
            }
        }


        ResetTouchState();

        return false;
    }


    // ---------------------------------------------------------
    // FINGER CANCEL
    // ---------------------------------------------------------

    if (event->type == SDL_EVENT_FINGER_CANCELED)
    {
        if (g_state.touch_active &&
            event->tfinger.touchID == g_state.touch_id &&
            event->tfinger.fingerID == g_state.finger_id)
        {
            ResetTouchState();
        }

        return false;
    }

    return false;
}


// =============================================================
// WINDOW RESIZE HANDLER
// =============================================================

bool HandleSDLWindowEvent(
    const SDL_Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    if (event->type == SDL_EVENT_WINDOW_RESIZED ||
        event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        Rml::ElementDocument* document =
            g_state.document;

        // Recalculate dimensions after rotation or when
        // Android resizes the usable area for the keyboard.

        if (IsChatDocument(document))
        {
            QueueInputUpdate(document);
        }
    }

    return false;
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

            ConfigureDocument(document);

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
            ResetTouchState();

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


    // ---------------------------------------------------------
    // KEYBOARD INPUT
    // ---------------------------------------------------------

    Rml::Element* target =
        event->GetTargetElement();

    Rml::Element* input =
        FindAncestorById(
            target,
            "message-input");

    if (input == nullptr)
    {
        return false;
    }


    // ---------------------------------------------------------
    // ENTER TO SEND
    // ---------------------------------------------------------

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
            QueueAction(
                ActionType::SendMessage,
                document);

            // Prevent a default newline.

            return true;
        }

        return false;
    }


    // ---------------------------------------------------------
    // TEXT CHANGES
    // ---------------------------------------------------------

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
// REGISTRATION HELPERS
// =============================================================

void RegisterRmlEvent(
    const char* event_name)
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


void RegisterSDLWindowEvent(Uint32 event_type)
{
    Ultron_RegisterEventHandler(
        kModuleName,
        event_type,
        &HandleSDLWindowEvent,
        nullptr,
        100);
}


// =============================================================
// MODULE REGISTRATION
// =============================================================

void RegisterObsidianWingChat()
{
    // Disable touch-generated mouse events.
    // The module is intended for phone-native touch input.

    SDL_SetHint(
        SDL_HINT_TOUCH_MOUSE_EVENTS,
        "0");

    // Do not synthesize touch from a mouse.

    SDL_SetHint(
        SDL_HINT_MOUSE_TOUCH_EVENTS,
        "0");


    // ---------------------------------------------------------
    // DOCUMENT
    // ---------------------------------------------------------

    RegisterRmlEvent("show");
    RegisterRmlEvent("hide");


    // ---------------------------------------------------------
    // TEXT INPUT
    // ---------------------------------------------------------

    RegisterRmlEvent("keydown");
    RegisterRmlEvent("keyup");
    RegisterRmlEvent("textinput");
    RegisterRmlEvent("change");


    // ---------------------------------------------------------
    // NATIVE TOUCH OBSERVATION
    // ---------------------------------------------------------

    RegisterSDLTouchEvent(
        SDL_EVENT_FINGER_DOWN);

    RegisterSDLTouchEvent(
        SDL_EVENT_FINGER_MOTION);

    RegisterSDLTouchEvent(
        SDL_EVENT_FINGER_UP);

    RegisterSDLTouchEvent(
        SDL_EVENT_FINGER_CANCELED);


    // ---------------------------------------------------------
    // VIEWPORT RESIZE
    // ---------------------------------------------------------

    RegisterSDLWindowEvent(
        SDL_EVENT_WINDOW_RESIZED);

    RegisterSDLWindowEvent(
        SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
}


// =============================================================
// MODULE LIFETIME
// =============================================================

struct ObsidianWingChatRegistration
{
    ObsidianWingChatRegistration()
    {
        RegisterObsidianWingChat();
    }

    ~ObsidianWingChatRegistration()
    {
        ResetTouchState();
    }
};


[[maybe_unused]]
ObsidianWingChatRegistration g_registration;


} // namespace
