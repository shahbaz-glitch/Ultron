
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ElementText.h>
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

constexpr float kControlUnits = 14.22f;

constexpr float kFirstLeft = 18.63f;
constexpr float kFirstRight = 12.99f;

constexpr float kMultilineLeft = 4.17f;
constexpr float kMultilineRight = 2.45f;

constexpr float kExtraGap = 1.5f;
constexpr float kGeometryEpsilon = 0.5f;

// Keep the shift threshold within a tiny visual margin of the arrow-safe edge.
constexpr float kFirstLineSafetyRatio = 0.002f;
constexpr float kMinimumSafetyPixels = 1.0f;


struct InputState
{
    Rml::ElementDocument* document = nullptr;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;

    Rml::ElementFormControlTextArea* input = nullptr;

    bool first_line_shifted = false;

    int last_line_count = 0;

    float last_control_height = 0.0f;
    float last_panel_width = 0.0f;

    float last_input_height = -1.0f;
    float last_bar_height = -1.0f;
    float last_chat_bottom = -1.0f;

    Rml::String last_value;
};


InputState& State()
{
    static InputState state;
    return state;
}


/* ---------------------------------------------------------
   ELEMENT BINDING
--------------------------------------------------------- */

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
        state = InputState{};
        state.document = document;
    }

    state.bar = document->GetElementById(kBarId);
    state.wing_hit = document->GetElementById(kWingHitId);
    state.chat = document->GetElementById(kChatId);

    Rml::Element* input = document->GetElementById(kInputId);
    state.input = input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    return state.bar && state.wing_hit && state.chat && state.input;
}


/* ---------------------------------------------------------
   INTERNAL TEXT ELEMENT
--------------------------------------------------------- */

Rml::ElementText* GetTextElement(Rml::ElementFormControlTextArea* input)
{
    if (!input || input->GetNumChildren(true) < 1)
        return nullptr;

    Rml::Element* child = input->GetChild(0);
    if (!child || child->GetTagName() != "#text")
        return nullptr;

    return static_cast<Rml::ElementText*>(child);
}


/* ---------------------------------------------------------
   TEXT STATE
--------------------------------------------------------- */

bool HasVisibleText(const Rml::String& value)
{
    return std::any_of(value.begin(), value.end(), [](unsigned char ch)
    {
        return std::isspace(ch) == 0;
    });
}


bool IsAppendOnly(const Rml::String& previous, const Rml::String& current)
{
    return current.size() >= previous.size() &&
           std::equal(previous.begin(), previous.end(), current.begin());
}


/* ---------------------------------------------------------
   EXISTING ULTRON CLASS API
--------------------------------------------------------- */

void SetClass(
    Rml::Element* element,
    const char* id,
    const char* class_name,
    bool enabled)
{
    if (element && element->IsClassSet(class_name) != enabled)
        Ultron_SetElementClass(id, class_name, enabled);
}


/* ---------------------------------------------------------
   PIXEL CONVERSION
--------------------------------------------------------- */

Rml::String Pixels(float value)
{
    return std::to_string(std::max(value, 0.0f)) + "px";
}


/* ---------------------------------------------------------
   FIRST-LINE WIDTH

   GenerateLine with unlimited width measures the first explicit
   line without introducing an early word-wrap boundary.
--------------------------------------------------------- */

float GetUnwrappedFirstLineWidth(Rml::ElementText* text_element)
{
    if (!text_element)
        return 0.0f;

    Rml::String line;
    int line_length = 0;
    float line_width = 0.0f;

    text_element->GenerateLine(
        line,
        line_length,
        line_width,
        0,
        -1.0f,
        0.0f,
        false);

    return line_width;
}


bool IsFirstLineReadyToShift(
    Rml::ElementText* text_element,
    float available_width)
{
    if (!text_element || available_width <= 0.0f)
        return false;

    const Rml::String& text = text_element->GetText();
    if (text.empty())
        return false;

    // An explicit newline means the first row was deliberately completed.
    if (text.find('\n') != Rml::String::npos)
        return true;

    const float occupied_width = GetUnwrappedFirstLineWidth(text_element);

    const float safety_margin = std::max(
        kMinimumSafetyPixels,
        available_width * kFirstLineSafetyRatio);

    return occupied_width >= available_width - safety_margin;
}


/* ---------------------------------------------------------
   EXACT RMLUI 6.3 LINE COUNT

   Uses the documented seven-argument GenerateLine() API.
   No spare-row reservation: panel height tracks actual lines only.
--------------------------------------------------------- */

int CountWrappedLines(
    Rml::ElementText* text_element,
    float available_width)
{
    if (!text_element)
        return 1;

    const Rml::String& text = text_element->GetText();
    if (text.empty())
        return 1;

    int line_begin = 0;
    int line_count = 0;
    bool last_line = false;

    while (line_begin < static_cast<int>(text.size()) && !last_line)
    {
        Rml::String line;
        int line_length = 0;
        float line_width = 0.0f;

        last_line = text_element->GenerateLine(
            line,
            line_length,
            line_width,
            line_begin,
            available_width,
            0.0f,
            line_begin > 0);

        if (line_length <= 0)
            break;

        line_begin += line_length;
        ++line_count;
    }

    // A trailing explicit newline creates a final empty visual row.
    if (!text.empty() && text.back() == '\n')
        ++line_count;

    return std::max(line_count, 1);
}


/* ---------------------------------------------------------
   RESPONSIVE TEXT WIDTH

   The expanded width is calculated from the containing block,
   not the currently animated width of the panel. This prevents
   the 0.8s width transition from making the first line shift early.
--------------------------------------------------------- */

float GetTextWidth(
    InputState& state,
    float control_height,
    float left_units,
    float right_units)
{
    const float scale = control_height / kControlUnits;

    Rml::Element* containing_element = state.bar->GetParentNode();
    if (!containing_element)
        return 1.0f;

    // input-bar is an absolutely positioned child of body. Measure the
    // containing block, not the panel's animated current width.
    const float expanded_panel_width =
        containing_element->GetClientWidth() * 0.88f;

    return std::max(
        1.0f,
        expanded_panel_width - (left_units + right_units) * scale);
}


/* ---------------------------------------------------------
   RESET MULTILINE

   Leave caret and scroll ownership to RmlUi's native textarea.
   No manual selection rewrite is needed after deletion.
--------------------------------------------------------- */

void ResetMultiline(InputState& state)
{
    state.first_line_shifted = false;

    state.last_line_count = 0;
    state.last_control_height = 0.0f;
    state.last_panel_width = 0.0f;

    state.last_input_height = -1.0f;
    state.last_bar_height = -1.0f;
    state.last_chat_bottom = -1.0f;

    state.last_value.clear();

    SetClass(state.bar, kBarId, "multiline", false);
    SetClass(state.bar, kBarId, "has-text", false);

    state.input->RemoveProperty("height");
    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");
}


/* ---------------------------------------------------------
   APPLY MULTILINE GEOMETRY

   No selection rewrite and no extra blank row. The native
   RmlUi textarea keeps its caret and scroll position.
--------------------------------------------------------- */

void ApplyMultilineLayout(
    InputState& state,
    int line_count,
    float control_height,
    float full_text_width)
{
    const float padding_height =
        control_height * (1.23f / kControlUnits);

    const float maximum_text_height =
        control_height * (30.15f / kControlUnits);

    const float extra_gap =
        control_height * (kExtraGap / kControlUnits);

    const float line_height =
        std::round(state.input->GetLineHeight());

    if (line_height <= 0.0f)
        return;

    const float natural_text_height =
        static_cast<float>(line_count) * line_height + padding_height;

    const float text_height = std::min(
        natural_text_height,
        maximum_text_height);

    const float input_content_height = std::max(
        0.0f,
        text_height - padding_height);

    const float bar_height =
        text_height + control_height + extra_gap;

    const float chat_bottom =
        bar_height + control_height * (11.52f / kControlUnits);

    const bool geometry_changed =
        std::fabs(input_content_height - state.last_input_height) > kGeometryEpsilon ||
        std::fabs(bar_height - state.last_bar_height) > kGeometryEpsilon ||
        std::fabs(chat_bottom - state.last_chat_bottom) > kGeometryEpsilon;

    if (geometry_changed)
    {
        state.input->SetProperty("height", Pixels(input_content_height));
        state.bar->SetProperty("height", Pixels(bar_height));
        state.chat->SetProperty("bottom", Pixels(chat_bottom));

        state.last_input_height = input_content_height;
        state.last_bar_height = bar_height;
        state.last_chat_bottom = chat_bottom;
    }

    state.last_line_count = line_count;
    state.last_control_height = control_height;
    state.last_panel_width = full_text_width;
}


/* ---------------------------------------------------------
   INPUT UPDATE

   1. Read the textarea's current value and its RmlUi text node.
   2. Shift only when the first row reaches its arrow-safe edge.
   3. Resize to the exact line count (no blank-row buffer).
   4. Leave scrolling/caret movement to RmlUi's native textarea.
--------------------------------------------------------- */

void UpdateInput(InputState& state)
{
    if (!state.input || !state.bar || !state.wing_hit || !state.chat)
        return;

    const Rml::String value = state.input->GetValue();
    Rml::ElementText* text_element = GetTextElement(state.input);

    SetClass(
        state.bar,
        kBarId,
        "has-text",
        HasVisibleText(value));

    if (value.empty())
    {
        ResetMultiline(state);
        return;
    }

    if (!text_element)
        return;

    const float control_height = state.wing_hit->GetOffsetHeight();
    if (control_height <= 0.0f)
        return;

    /* FIRST-LINE SHIFT */

    if (!state.first_line_shifted)
    {
        const float first_line_width = GetTextWidth(
            state,
            control_height,
            kFirstLeft,
            kFirstRight);

        if (!IsFirstLineReadyToShift(text_element, first_line_width))
        {
            state.last_value = value;
            return;
        }

        state.first_line_shifted = true;

        SetClass(
            state.bar,
            kBarId,
            "multiline",
            true);
    }

    /* FULL-WIDTH MULTILINE */

    const float full_width = GetTextWidth(
        state,
        control_height,
        kMultilineLeft,
        kMultilineRight);

    int line_count = CountWrappedLines(text_element, full_width);

    // Prevent temporary collapse during forward typing; deletion can shrink it.
    if (IsAppendOnly(state.last_value, value))
        line_count = std::max(line_count, state.last_line_count);

    const bool line_count_changed =
        line_count != state.last_line_count;

    const bool responsive_size_changed =
        std::fabs(control_height - state.last_control_height) > kGeometryEpsilon ||
        std::fabs(full_width - state.last_panel_width) > kGeometryEpsilon;

    if (line_count_changed || responsive_size_changed || state.last_bar_height < 0.0f)
    {
        ApplyMultilineLayout(
            state,
            line_count,
            control_height,
            full_width);
    }

    state.last_value = value;
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER

   click  = Wing expand/collapse
   change = Textarea updates
--------------------------------------------------------- */

bool HandleRmlEvent(Rml::Event* event, void*)
{
    if (!event)
        return false;

    Rml::Element* target = event->GetTargetElement();
    if (!target)
        return false;

    if (event->GetType() == "click" && target->GetId() == kWingHitId)
    {
        if (!BindElements(target))
            return false;

        InputState& state = State();
        const bool expand = !state.bar->IsClassSet("expanded");

        Ultron_SetElementClass(kBarId, "expanded", expand);

        if (expand)
            state.input->Focus();

        return false;
    }

    if (event->GetType() == "change" && target->GetId() == kInputId)
    {
        if (!BindElements(target))
            return false;

        UpdateInput(State());
        return false;
    }

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC REGISTRATION
--------------------------------------------------------- */

bool RegisterObsidianWingChat()
{
    const bool click_registered = Ultron_RegisterRmlEventHandler(
        kModuleName,
        "click",
        &HandleRmlEvent);

    const bool change_registered = Ultron_RegisterRmlEventHandler(
        kModuleName,
        "change",
        &HandleRmlEvent);

    return click_registered && change_registered;
}

[[maybe_unused]] const bool g_registered = RegisterObsidianWingChat();

} // namespace
