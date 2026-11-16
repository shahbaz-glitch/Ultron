#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";
constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kInputId[] = "message-input";
constexpr char kChatId[] = "chat-area";

// Responsive design units from the existing portrait/landscape RML.
constexpr float kControlUnits = 14.22f;
constexpr float kInitialRightUnits = 12.99f;
constexpr float kArrowSafetyUnits = 3.43f;
constexpr float kExtraGapUnits = 1.5f;
constexpr float kInputPaddingUnits = 1.23f;
constexpr float kMaximumTextUnits = 30.15f;
constexpr float kMaximumInputUnits = 28.92f;

struct InputState
{
    Rml::ElementDocument* document = nullptr;
    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;
    Rml::ElementFormControlTextArea* input = nullptr;

    bool first_line_shifted = false;
    bool update_queued = false;

    int applied_line_count = -1;

    float applied_control_height = -1.0f;
    float applied_input_height = -1.0f;
    float applied_input_right = -1.0f;
    float applied_bar_height = -1.0f;
    float applied_chat_bottom = -1.0f;
};

InputState& State()
{
    static InputState state;
    return state;
}

// -------------------------------------------------------------
// Element binding
// -------------------------------------------------------------

bool BindElements(Rml::Element* source)
{
    if (!source)
        return false;

    Rml::ElementDocument* document = source->GetOwnerDocument();

    if (!document)
        return false;

    InputState& state = State();

    if (state.document != document)
    {
        const bool pending = state.update_queued;

        state = InputState{};
        state.document = document;
        state.update_queued = pending;
    }

    state.bar = document->GetElementById(kBarId);
    state.wing_hit = document->GetElementById(kWingHitId);
    state.chat = document->GetElementById(kChatId);

    Rml::Element* input = document->GetElementById(kInputId);

    state.input =
        input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    return
        state.bar &&
        state.wing_hit &&
        state.chat &&
        state.input;
}

// -------------------------------------------------------------
// RmlUi textarea text element
// -------------------------------------------------------------

Rml::ElementText* GetTextElement(
    Rml::ElementFormControlTextArea* input)
{
    if (!input || input->GetNumChildren(true) < 1)
        return nullptr;

    // RmlUi 6.3 stores the primary text widget at child zero.
    Rml::Element* child = input->GetChild(0);

    if (!child || child->GetTagName() != "#text")
        return nullptr;

    return static_cast<Rml::ElementText*>(child);
}

// -------------------------------------------------------------
// Text state
// -------------------------------------------------------------

bool HasVisibleText(const Rml::String& value)
{
    return std::any_of(
        value.begin(),
        value.end(),
        [](unsigned char ch)
        {
            return std::isspace(ch) == 0;
        });
}

// -------------------------------------------------------------
// UI helpers
// -------------------------------------------------------------

void SetClass(
    Rml::Element* element,
    const char* element_id,
    const char* class_name,
    bool enabled)
{
    if (element && element->IsClassSet(class_name) != enabled)
    {
        Ultron_SetElementClass(
            element_id,
            class_name,
            enabled);
    }
}

std::string Pixels(float value)
{
    return
        std::to_string(std::max(value, 0.0f))
        + "px";
}

bool SetPixelPropertyIfChanged(
    const char* element_id,
    const char* property_name,
    float value,
    float& cached_value)
{
    if (cached_value >= 0.0f &&
        std::fabs(value - cached_value) < 0.5f)
    {
        return true;
    }

    const std::string pixel_value = Pixels(value);

    if (!Ultron_SetElementProperty(
            element_id,
            property_name,
            pixel_value.c_str()))
    {
        return false;
    }

    cached_value = value;
    return true;
}

void InvalidateAppliedSize(InputState& state)
{
    state.applied_line_count = -1;

    state.applied_control_height = -1.0f;
    state.applied_input_height = -1.0f;
    state.applied_input_right = -1.0f;
    state.applied_bar_height = -1.0f;
    state.applied_chat_bottom = -1.0f;
}

// -------------------------------------------------------------
// UI task scheduling
// -------------------------------------------------------------

void UpdateInputOnUI(void* user_data);

void QueueInputUpdate(InputState& state)
{
    // Coalesce multiple text changes into one UI update.
    if (state.update_queued)
        return;

    state.update_queued = true;

    if (!Ultron_PostToUI(
            &UpdateInputOnUI,
            &state))
    {
        state.update_queued = false;
    }
}

// -------------------------------------------------------------
// Reset
// -------------------------------------------------------------

void ResetMultiline(InputState& state)
{
    const bool needs_reset =
        state.first_line_shifted ||
        state.bar->IsClassSet("multiline") ||
        state.bar->IsClassSet("has-text") ||
        state.applied_line_count >= 0 ||
        state.applied_input_height >= 0.0f ||
        state.applied_input_right >= 0.0f ||
        state.applied_bar_height >= 0.0f ||
        state.applied_chat_bottom >= 0.0f;

    if (!needs_reset)
        return;

    state.first_line_shifted = false;

    SetClass(
        state.bar,
        kBarId,
        "multiline",
        false);

    SetClass(
        state.bar,
        kBarId,
        "has-text",
        false);

    // Restore original responsive RML properties.
    state.input->RemoveProperty("height");
    state.input->RemoveProperty("right");

    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");

    InvalidateAppliedSize(state);
}

// -------------------------------------------------------------
// First-line detection
// -------------------------------------------------------------

bool FirstLineNearArrow(
    InputState& state,
    Rml::ElementText* text_element,
    float control_height)
{
    const float scale =
        control_height / kControlUnits;

    // Matches RmlUi 6.3 WidgetTextInput::GetAvailableWidth().
    const float available_width =
        state.input->GetClientWidth()
        - state.input->GetBox()
              .GetFrameSize(Rml::BoxArea::Padding).x;

    if (available_width <= 1.0f)
        return false;

    // Explicit Enter or natural wrapping also activates multiline.
    if (text_element->GetLines().size() > 1)
        return true;

    // Keep enough room before the send-arrow region.
    const float cursor_width = std::round(
        Rml::ElementUtilities::
            GetDensityIndependentPixelRatio(text_element));

    const float safe_line_width = std::max(
        1.0f,
        available_width
            - scale * kArrowSafetyUnits
            - cursor_width);

    Rml::String first_line;

    int consumed = 0;
    float rendered_width = 0.0f;

    // Use the same line-generation mechanism as RmlUi.
    const bool all_text_fits_first_line =
        text_element->GenerateLine(
            first_line,
            consumed,
            rendered_width,
            0,
            safe_line_width,
            0.0f,
            false,
            false,
            false);

    return !all_text_fits_first_line;
}

// -------------------------------------------------------------
// Multiline geometry
// -------------------------------------------------------------

void ApplyMultilineSize(
    InputState& state,
    int line_count,
    float control_height)
{
    line_count = std::max(line_count, 1);

    const float scale =
        control_height / kControlUnits;

    const float line_height =
        std::round(state.input->GetLineHeight());

    if (line_height <= 0.0f)
        return;

    const float padding_height =
        scale * kInputPaddingUnits;

    const float maximum_text_height =
        scale * kMaximumTextUnits;

    const float extra_gap =
        scale * kExtraGapUnits;

    const float natural_text_height =
        static_cast<float>(line_count) * line_height
        + padding_height;

    const float text_height =
        std::min(
            natural_text_height,
            maximum_text_height);

    const float input_height =
        std::min(
            std::max(
                0.0f,
                text_height - padding_height),
            scale * kMaximumInputUnits);

    const float bar_height =
        text_height + control_height + extra_gap;

    const float chat_bottom =
        bar_height + scale * 11.52f;

    const float input_right =
        scale * kInitialRightUnits;

    // Nothing changes when the rendered geometry is unchanged.
    if (line_count == state.applied_line_count &&
        state.applied_control_height >= 0.0f &&
        std::fabs(
            control_height - state.applied_control_height) < 0.5f &&
        state.applied_input_height >= 0.0f &&
        std::fabs(
            input_height - state.applied_input_height) < 0.5f &&
        state.applied_input_right >= 0.0f &&
        std::fabs(
            input_right - state.applied_input_right) < 0.5f &&
        state.applied_bar_height >= 0.0f &&
        std::fabs(
            bar_height - state.applied_bar_height) < 0.5f &&
        state.applied_chat_bottom >= 0.0f &&
        std::fabs(
            chat_bottom - state.applied_chat_bottom) < 0.5f)
    {
        return;
    }

    // Important:
    // Multiline RML uses right: 2.45vw/vh, which overlaps the send arrow.
    // Override it with the original safe right boundary: 12.99vw/vh.
    const bool right_ok =
        SetPixelPropertyIfChanged(
            kInputId,
            "right",
            input_right,
            state.applied_input_right);

    const bool input_ok =
        SetPixelPropertyIfChanged(
            kInputId,
            "height",
            input_height,
            state.applied_input_height);

    const bool bar_ok =
        SetPixelPropertyIfChanged(
            kBarId,
            "height",
            bar_height,
            state.applied_bar_height);

    const bool chat_ok =
        SetPixelPropertyIfChanged(
            kChatId,
            "bottom",
            chat_bottom,
            state.applied_chat_bottom);

    if (right_ok &&
        input_ok &&
        bar_ok &&
        chat_ok)
    {
        state.applied_line_count = line_count;
        state.applied_control_height = control_height;
    }
}

// -------------------------------------------------------------
// Input update
// -------------------------------------------------------------

void UpdateInputOnUI(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (!state)
        return;

    state->update_queued = false;

    if (!state->bar ||
        !state->wing_hit ||
        !state->chat ||
        !state->input)
    {
        return;
    }

    const Rml::String value =
        state->input->GetValue();

    const bool has_text =
        HasVisibleText(value);

    SetClass(
        state->bar,
        kBarId,
        "has-text",
        has_text);

    // Empty and whitespace-only values reset multiline state.
    if (!has_text)
    {
        ResetMultiline(*state);
        return;
    }

    Rml::ElementText* text_element =
        GetTextElement(state->input);

    if (!text_element)
        return;

    const float control_height =
        state->wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
        return;

    // First transition into multiline mode.
    if (!state->first_line_shifted)
    {
        if (!FirstLineNearArrow(
                *state,
                text_element,
                control_height))
        {
            return;
        }

        state->first_line_shifted = true;

        InvalidateAppliedSize(*state);

        SetClass(
            state->bar,
            kBarId,
            "multiline",
            true);

        // Critical fix:
        // Expand the panel for line one in this same update.
        // The textarea must never move upward before its panel grows.
        ApplyMultilineSize(
            *state,
            1,
            control_height);

        // Multiline class changes the textarea width.
        // RmlUi applies that layout in the next Context::Update().
        // Ultron snapshots its task queue, so this follow-up executes
        // on the next frame and reads the newly formatted line layout.
        QueueInputUpdate(*state);

        return;
    }

    // RmlUi has already formatted the text for this change.
    const int rendered_line_count = std::max(
        1,
        static_cast<int>(
            text_element->GetLines().size()));

    ApplyMultilineSize(
        *state,
        rendered_line_count,
        control_height);
}

// -------------------------------------------------------------
// RmlUi events
// -------------------------------------------------------------

bool HandleRmlEvent(
    Rml::Event* event,
    void*)
{
    if (!event)
        return false;

    Rml::Element* target =
        event->GetTargetElement();

    if (!target)
        return false;

    // Wing click: preserve existing expand/collapse behaviour.
    if (event->GetType() == "click" &&
        target->GetId() == kWingHitId)
    {
        if (!BindElements(target))
            return false;

        InputState& state = State();

        const bool expand =
            !state.bar->IsClassSet("expanded");

        Ultron_SetElementClass(
            kBarId,
            "expanded",
            expand);

        if (expand)
            state.input->Focus();

        return false;
    }

    // Text changes.
    if (event->GetType() == "change" &&
        target->GetId() == kInputId)
    {
        InputState& state = State();

        // Avoid repeated document lookups on every keystroke.
        if (!state.input ||
            state.input != target ||
            state.document != target->GetOwnerDocument())
        {
            if (!BindElements(target))
                return false;
        }

        const bool has_text =
            HasVisibleText(state.input->GetValue());

        // Pulse -> arrow remains immediate.
        SetClass(
            state.bar,
            kBarId,
            "has-text",
            has_text);

        // Geometry is coalesced into one UI task.
        QueueInputUpdate(state);
    }

    return false;
}

// -------------------------------------------------------------
// Automatic module registration
// -------------------------------------------------------------

bool RegisterObsidianWingChat()
{
    const bool click_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "click",
            &HandleRmlEvent);

    const bool change_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "change",
            &HandleRmlEvent);

    return click_registered && change_registered;
}

[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
