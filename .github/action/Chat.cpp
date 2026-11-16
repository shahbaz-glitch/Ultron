
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

constexpr float kControlUnits = 14.22f;
constexpr float kMultilineLeft = 4.17f;
constexpr float kMultilineRight = 2.45f;
constexpr float kExtraGap = 1.5f;
constexpr float kGeometryEpsilon = 0.5f;

struct InputState
{
    Rml::ElementDocument* document = nullptr;
    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;
    Rml::ElementFormControlTextArea* input = nullptr;

    bool word_break_configured = false;
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
    auto* previous_input = state.input;

    state.input = input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    if (!state.bar || !state.wing_hit || !state.chat || !state.input)
        return false;

    if (state.input != previous_input)
        state.word_break_configured = false;

    // Fill each visual row instead of leaving a word-sized gap.
    // This affects only the textarea, not the chat bubbles.
    if (!state.word_break_configured)
    {
        state.input->SetProperty("word-break", "break-all");
        state.word_break_configured = true;
    }

    return true;
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
   SMALL HELPERS
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

void SetClass(Rml::Element* element, const char* id,
              const char* class_name, bool enabled)
{
    if (element && element->IsClassSet(class_name) != enabled)
        Ultron_SetElementClass(id, class_name, enabled);
}

Rml::String Pixels(float value)
{
    return std::to_string(std::max(value, 0.0f)) + "px";
}

/* ---------------------------------------------------------
   RMLUI 6.3 LINE COUNT

   Uses GenerateLine() and the same trailing-space handling
   as WidgetTextInput::FormatText().

   No speculative blank row is added.
--------------------------------------------------------- */

int CountWrappedLines(Rml::ElementText* text_element,
                      float available_width,
                      float cursor_width)
{
    if (!text_element)
        return 1;

    const Rml::String& text = text_element->GetText();
    if (text.empty())
        return 1;

    const float maximum_line_width = available_width - cursor_width;
    if (maximum_line_width <= 0.0f)
        return 1;

    int line_begin = 0;
    int line_count = 0;
    bool last_line = false;

    do
    {
        Rml::String line_content;
        int line_size = 0;
        float line_width = 0.0f;

        last_line = text_element->GenerateLine(
            line_content,
            line_size,
            line_width,
            line_begin,
            maximum_line_width,
            0.0f,
            false,
            false,
            false);

        int editable_length = static_cast<int>(line_content.size());
        if (!line_content.empty() && line_content.back() == '\n')
            --editable_length;

        const size_t space_begin = std::min(
            text.size(),
            static_cast<size_t>(line_begin + std::max(editable_length, 0)));

        const size_t found = text.find_first_not_of(' ', space_begin);
        const size_t space_end = found == Rml::String::npos
            ? text.size()
            : found;

        const size_t trailing_spaces = space_end - space_begin;

        if (trailing_spaces > 0)
        {
            line_size += static_cast<int>(trailing_spaces);

            if (space_end < text.size() && text[space_end] == '\n')
                ++line_size;

            if (space_end == text.size())
                last_line = true;
        }

        if (line_size <= 0)
        {
            // Count the empty visual row after a trailing newline.
            ++line_count;
            break;
        }

        line_begin += line_size;
        ++line_count;

    } while (!last_line);

    return std::max(line_count, 1);
}

/* ---------------------------------------------------------
   RESPONSIVE WIDTH
--------------------------------------------------------- */

float GetFirstLineWidth(InputState& state)
{
    // Actual wing-side textarea width.
    // Matches WidgetTextInput::GetAvailableWidth().
    const float frame = state.input->GetBox()
        .GetFrameSize(Rml::BoxArea::Padding).x;

    return std::max(1.0f, state.input->GetClientWidth() - frame);
}

float GetMultilineWidth(InputState& state, float control_height)
{
    const float scale = control_height / kControlUnits;
    const float frame = state.input->GetBox()
        .GetFrameSize(Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        (kMultilineLeft + kMultilineRight) * scale -
        frame);
}

/* ---------------------------------------------------------
   RESET
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
   MULTILINE GEOMETRY

   At max-height, RmlUi's existing overflow-y:auto and
   MoveToCursor() handle internal scrolling.
--------------------------------------------------------- */

void ApplyMultilineLayout(InputState& state,
                          int line_count,
                          float control_height,
                          float full_width)
{
    const float padding_height = control_height * (1.23f / kControlUnits);
    const float maximum_text_height = control_height * (30.15f / kControlUnits);
    const float extra_gap = control_height * (1.5f / kControlUnits);

    const float line_height = std::round(state.input->GetLineHeight());
    if (line_height <= 0.0f)
        return;

    // Exact line count. No extra blank row.
    const float natural_text_height =
        static_cast<float>(line_count) * line_height + padding_height;

    const float text_height = std::min(natural_text_height, maximum_text_height);
    const float input_height = std::max(0.0f, text_height - padding_height);
    const float bar_height = text_height + control_height + extra_gap;

    const float chat_bottom =
        bar_height + control_height * (11.52f / kControlUnits);

    const bool geometry_changed =
        std::fabs(input_height - state.last_input_height) > kGeometryEpsilon ||
        std::fabs(bar_height - state.last_bar_height) > kGeometryEpsilon ||
        std::fabs(chat_bottom - state.last_chat_bottom) > kGeometryEpsilon;

    if (geometry_changed)
    {
        state.input->SetProperty("height", Pixels(input_height));
        state.bar->SetProperty("height", Pixels(bar_height));
        state.chat->SetProperty("bottom", Pixels(chat_bottom));

        state.last_input_height = input_height;
        state.last_bar_height = bar_height;
        state.last_chat_bottom = chat_bottom;
    }

    state.last_line_count = line_count;
    state.last_control_height = control_height;
    state.last_panel_width = full_width;
}

/* ---------------------------------------------------------
   INPUT UPDATE
--------------------------------------------------------- */

void UpdateInput(InputState& state)
{
    if (!state.input || !state.bar || !state.wing_hit || !state.chat)
        return;

    const Rml::String value = state.input->GetValue();

    SetClass(state.bar, kBarId, "has-text", HasVisibleText(value));

    if (value.empty())
    {
        ResetMultiline(state);
        return;
    }

    Rml::ElementText* text_element = GetTextElement(state.input);
    if (!text_element)
        return;

    const float control_height = state.wing_hit->GetOffsetHeight();
    if (control_height <= 0.0f)
        return;

    const float cursor_width = std::round(
        Rml::ElementUtilities::GetDensityIndependentPixelRatio(text_element));

    /* FIRST LINE: shift when actual wrapping creates row two. */

    if (!state.first_line_shifted)
    {
        const int first_line_count = CountWrappedLines(
            text_element,
            GetFirstLineWidth(state),
            cursor_width);

        if (first_line_count <= 1)
        {
            state.last_value = value;
            return;
        }

        state.first_line_shifted = true;
        SetClass(state.bar, kBarId, "multiline", true);
    }

    /* Recalculate against the new full-width input area. */

    const float full_width = GetMultilineWidth(state, control_height);

    int line_count = CountWrappedLines(
        text_element,
        full_width,
        cursor_width);

    // Appending text grows the panel; deleting/replacing may shrink it.
    if (IsAppendOnly(state.last_value, value))
        line_count = std::max(line_count, state.last_line_count);

    const bool line_count_changed =
        line_count != state.last_line_count;

    const bool responsive_size_changed =
        std::fabs(control_height - state.last_control_height) > kGeometryEpsilon ||
        std::fabs(full_width - state.last_panel_width) > kGeometryEpsilon;

    if (line_count_changed ||
        responsive_size_changed ||
        state.last_input_height < 0.0f)
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
        kModuleName, "click", &HandleRmlEvent);

    const bool change_registered = Ultron_RegisterRmlEventHandler(
        kModuleName, "change", &HandleRmlEvent);

    return click_registered && change_registered;
}

[[maybe_unused]] const bool g_registered = RegisterObsidianWingChat();

} // namespace
