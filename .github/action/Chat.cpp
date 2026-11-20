
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Log.h>

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


/* ---------------------------------------------------------
   INPUT STATE
--------------------------------------------------------- */

struct InputState
{
    Rml::ElementDocument* document = nullptr;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;

    Rml::ElementFormControlTextArea* input = nullptr;

    bool word_break_configured = false;
    bool first_line_shifted = false;

    // Prevent recursive event handling during document update.
    bool document_update_in_progress = false;

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

    Rml::ElementDocument* document =
        source->GetOwnerDocument();

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

    Rml::Element* input =
        document->GetElementById(kInputId);

    state.input =
        input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    if (!state.bar || !state.wing_hit ||
        !state.chat || !state.input)
        return false;

    return true;
}


/* ---------------------------------------------------------
   INTERNAL TEXT ELEMENT
--------------------------------------------------------- */

Rml::ElementText* GetTextElement(
    Rml::ElementFormControlTextArea* input)
{
    if (!input || input->GetNumChildren(true) < 1)
        return nullptr;

    Rml::Element* child = input->GetChild(0);

    if (!child || child->GetTagName() != "#text")
        return nullptr;

    return static_cast<Rml::ElementText*>(child);
}


/* ---------------------------------------------------------
   MULTILINE WORD BREAK

   Preserve complete words after first-line shift.
--------------------------------------------------------- */

void ConfigureMultilineWordBreak(InputState& state)
{
    if (state.word_break_configured || !state.input)
        return;

    if (!state.input->SetProperty(
            "word-break",
            "break-word"))
    {
        Rml::Log::Message(
            Rml::Log::LT_WARNING,
            "ObsidianWingChat: Failed to configure "
            "word-break: break-word.");

        return;
    }

    state.word_break_configured = true;
}


/* ---------------------------------------------------------
   TEXT STATE
--------------------------------------------------------- */

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


bool IsAppendOnly(
    const Rml::String& previous,
    const Rml::String& current)
{
    return current.size() >= previous.size() &&
           std::equal(
               previous.begin(),
               previous.end(),
               current.begin());
}


void SetClass(
    Rml::Element* element,
    const char* id,
    const char* class_name,
    bool enabled)
{
    if (element &&
        element->IsClassSet(class_name) != enabled)
    {
        Ultron_SetElementClass(
            id,
            class_name,
            enabled);
    }
}


Rml::String Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}


/* ---------------------------------------------------------
   FIRST-LINE SHIFT DETECTION

   Shift only when current line reaches its
   arrow-safe width.
--------------------------------------------------------- */

bool IsFirstLineReadyToShift(
    Rml::ElementText* text_element,
    float available_width,
    float cursor_width)
{
    if (!text_element || available_width <= 0.0f)
        return false;

    const Rml::String& text =
        text_element->GetText();

    if (text.empty())
        return false;

    // Explicit newline means user started another row.
    const size_t newline = text.find('\n');

    if (newline != Rml::String::npos)
        return true;

    const Rml::StringView first_line(
        text,
        0,
        text.size());

    const float occupied_width =
        static_cast<float>(
            Rml::ElementUtilities::GetStringWidth(
                text_element,
                first_line));

    // Reserve only the actual native caret width.
    return occupied_width + cursor_width >= available_width;
}


/* ---------------------------------------------------------
   NATIVE WRAP DETECTION (single-line layout)
--------------------------------------------------------- */

bool HasNativeWrapped(InputState& state)
{
    const float line_height =
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return false;

    // Read client height first.
    const float client_height =
        state.input->GetClientHeight();

    // Box still collapsed / animating open: do not guess.
    if (client_height < line_height * 0.9f)
        return false;

    return state.input->GetScrollHeight() -
               client_height >=
           line_height * 0.75f;
}


/* ---------------------------------------------------------
   RMLUI-NATIVE LINE COUNT

   Matches native textarea formatting:
   - Actual available width.
   - Native caret-width reservation.
   - GenerateLine() consumed source length.
   - Native trailing-space handling.
--------------------------------------------------------- */

int CountWrappedLines(
    Rml::ElementText* text_element,
    float available_width,
    float cursor_width)
{
    if (!text_element)
        return 1;

    const Rml::String& text =
        text_element->GetText();

    if (text.empty())
        return 1;

    const float maximum_line_width =
        available_width - cursor_width;

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

        int editable_length =
            static_cast<int>(line_content.size());

        if (!line_content.empty() &&
            line_content.back() == '\n')
        {
            --editable_length;
        }

        const size_t space_begin = std::min(
            text.size(),
            static_cast<size_t>(
                line_begin + std::max(editable_length, 0)));

        const size_t found =
            text.find_first_not_of(' ', space_begin);

        const size_t space_end =
            found == Rml::String::npos
            ? text.size()
            : found;

        const size_t trailing_spaces =
            space_end - space_begin;

        if (trailing_spaces > 0)
        {
            line_size +=
                static_cast<int>(trailing_spaces);

            if (space_end < text.size() &&
                text[space_end] == '\n')
            {
                ++line_size;
            }

            if (space_end == text.size())
                last_line = true;
        }

        if (line_size <= 0)
        {
            // Final empty visual row after trailing newline.
            ++line_count;
            break;
        }

        line_begin += line_size;
        ++line_count;

    }
    while (!last_line);

    return std::max(line_count, 1);
}


/* ---------------------------------------------------------
   RESPONSIVE TEXT WIDTH
--------------------------------------------------------- */

float GetTextWidth(
    InputState& state,
    float control_height,
    float left_units,
    float right_units)
{
    const float scale =
        control_height / kControlUnits;

    const float horizontal_frame =
        state.input->GetBox()
            .GetFrameSize(Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        (left_units + right_units) * scale -
        horizontal_frame);
}


/* ---------------------------------------------------------
   SCROLL SYNC

   IMPORTANT:
   This function runs AFTER ElementDocument::UpdateDocument().
   RmlUi's native scrollable overflow is therefore current.

   hidden_rows is measured from the actual formatted
   textarea content and viewport.
--------------------------------------------------------- */

void SyncScroll(
    InputState& state,
    bool force_top,
    bool follow_end)
{
    Rml::Element* input = state.input;

    if (force_top)
    {
        input->SetScrollTop(0.0f);
        return;
    }

    const float line_height =
        input->GetLineHeight();

    if (line_height <= 0.0f)
        return;

    // Document layout has already been refreshed.
    const float client_height =
        input->GetClientHeight();

    const float scroll_height =
        input->GetScrollHeight();

    const int hidden_rows = std::max(
        0,
        static_cast<int>(
            std::floor(
                (scroll_height - client_height) /
                    line_height +
                0.5f)));

    const float current_top =
        input->GetScrollTop();

    // Everything fits: stay at the top.
    if (hidden_rows == 0)
    {
        if (current_top > kGeometryEpsilon)
            input->SetScrollTop(0.0f);

        return;
    }

    // Follow the caret one native row at a time.
    if (follow_end)
    {
        const float target =
            static_cast<float>(hidden_rows) * line_height;

        if (std::fabs(current_top - target) >
            kGeometryEpsilon)
        {
            input->SetScrollTop(target);
        }
    }
}


/* ---------------------------------------------------------
   RESET MULTILINE
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

    if (state.word_break_configured)
    {
        state.input->SetProperty(
            "word-break",
            "break-word");
    }

    state.word_break_configured = false;

    SetClass(
        state.bar,
        kBarId,
        "multiline",
        false);

    state.input->RemoveProperty("width");
    state.input->RemoveProperty("height");

    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");

    state.input->SetScrollTop(0.0f);
    state.input->SetScrollLeft(0.0f);
}


/* ---------------------------------------------------------
   APPLY MULTILINE GEOMETRY
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
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return;

    const int maximum_rows = std::max(
        1,
        static_cast<int>(
            std::floor(
                (maximum_text_height - padding_height) /
                    line_height +
                0.05f)));

    const int visible_rows = std::min(
        std::max(line_count, 1),
        maximum_rows);

    const float input_content_height =
        static_cast<float>(visible_rows) * line_height;

    const float text_height =
        input_content_height + padding_height;

    const float bar_height =
        text_height +
        control_height +
        extra_gap;

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / kControlUnits);

    const bool width_changed =
        std::fabs(
            full_text_width -
            state.last_panel_width) > kGeometryEpsilon;

    const bool geometry_changed =
        width_changed ||

        std::fabs(
            input_content_height -
            state.last_input_height) > kGeometryEpsilon ||

        std::fabs(
            bar_height -
            state.last_bar_height) > kGeometryEpsilon ||

        std::fabs(
            chat_bottom -
            state.last_chat_bottom) > kGeometryEpsilon;

    if (geometry_changed)
    {
        state.input->SetProperty(
            "width",
            Pixels(full_text_width));

        state.input->SetProperty(
            "height",
            Pixels(input_content_height));

        state.bar->SetProperty(
            "height",
            Pixels(bar_height));

        state.chat->SetProperty(
            "bottom",
            Pixels(chat_bottom));

        state.last_input_height =
            input_content_height;

        state.last_bar_height =
            bar_height;

        state.last_chat_bottom =
            chat_bottom;

        state.last_panel_width =
            full_text_width;
    }

    state.last_line_count = line_count;
    state.last_control_height = control_height;
}


/* ---------------------------------------------------------
   INPUT UPDATE

   1. Arrow state
   2. First-line shift
   3. Preserve complete words
   4. Calculate responsive textarea width
   5. Apply multiline geometry
   6. Force RmlUi document layout update
   7. Synchronize native scroll range
--------------------------------------------------------- */

void UpdateInput(InputState& state)
{
    if (!state.input ||
        !state.bar ||
        !state.wing_hit ||
        !state.chat ||
        !state.document)
    {
        return;
    }

    // Do not recursively update the document during layout.
    if (state.document_update_in_progress)
        return;

    const Rml::String value =
        state.input->GetValue();

    // Pulse -> Arrow
    SetClass(
        state.bar,
        kBarId,
        "has-text",
        HasVisibleText(value));

    // Empty input: reset completely.
    if (value.empty())
    {
        ResetMultiline(state);
        return;
    }

    Rml::ElementText* text_element =
        GetTextElement(state.input);

    if (!text_element)
        return;

    const float control_height =
        state.wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
        return;

    const float cursor_width =
        std::round(
            Rml::ElementUtilities::
                GetDensityIndependentPixelRatio(text_element));

    bool just_shifted = false;


    /* FIRST LINE */

    if (!state.first_line_shifted)
    {
        const float first_line_width =
            GetTextWidth(
                state,
                control_height,
                kFirstLeft,
                kFirstRight);

        const bool reached_width =
            IsFirstLineReadyToShift(
                text_element,
                first_line_width,
                cursor_width);

        const bool already_wrapped =
            !reached_width && HasNativeWrapped(state);

        if (!reached_width && !already_wrapped)
        {
            state.last_value = value;
            return;
        }

        state.first_line_shifted = true;
        just_shifted = true;

        SetClass(
            state.bar,
            kBarId,
            "multiline",
            true);

        ConfigureMultilineWordBreak(state);
    }


    /* FULL-WIDTH MULTILINE */

    const float full_width =
        GetTextWidth(
            state,
            control_height,
            kMultilineLeft,
            kMultilineRight);

    const bool responsive_size_changed =
        std::fabs(
            control_height -
            state.last_control_height) > kGeometryEpsilon ||

        std::fabs(
            full_width -
            state.last_panel_width) > kGeometryEpsilon;

    const int line_count =
        CountWrappedLines(
            text_element,
            full_width,
            cursor_width);

    const bool append_only =
        IsAppendOnly(state.last_value, value);

    int final_line_count = line_count;

    // Do not collapse during ordinary forward typing.
    if (append_only && !responsive_size_changed)
    {
        final_line_count = std::max(
            final_line_count,
            state.last_line_count);
    }

    const bool line_count_changed =
        final_line_count != state.last_line_count;

    if (just_shifted ||
        line_count_changed ||
        responsive_size_changed)
    {
        ApplyMultilineLayout(
            state,
            final_line_count,
            control_height,
            full_width);
    }


    /* NATIVE RMLUI LAYOUT REFRESH */

    /*
        Critical ordering fix:

        The change event is dispatched from AddCharacters()
        BEFORE WidgetTextInput finishes its normal layout.

        UpdateDocument() now processes the changed textarea
        dimensions and recalculates its native scrollable
        overflow rectangle before SyncScroll() runs.
    */

    state.document_update_in_progress = true;

    state.document->UpdateDocument();

    state.document_update_in_progress = false;


    /* SCROLL SYNC */

    SyncScroll(
        state,
        just_shifted && final_line_count == 1,
        append_only);

    state.last_value = value;
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER

   click         = Wing expand/collapse
   change        = Textarea updates
   transitionend = Re-sync after animated resize
--------------------------------------------------------- */

bool HandleRmlEvent(
    Rml::Event* event,
    void*)
{
    if (!event)
        return false;

    InputState& current_state = State();

    // Ignore recursive events dispatched during manual layout.
    if (current_state.document_update_in_progress)
        return false;

    Rml::Element* target =
        event->GetTargetElement();

    if (!target)
        return false;


    /* WING CLICK */

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


    /* TEXTAREA CHANGE */

    if (event->GetType() == "change" &&
        target->GetId() == kInputId)
    {
        if (!BindElements(target))
            return false;

        UpdateInput(State());
        return false;
    }


    /* ANIMATED RESIZE FINISHED */

    if (event->GetType() == "transitionend")
    {
        const Rml::String& id =
            target->GetId();

        if (id == kInputId ||
            id == kBarId ||
            id == kChatId)
        {
            if (BindElements(target))
            {
                InputState& state = State();

                if (state.first_line_shifted)
                {
                    if (id == kBarId)
                    {
                        UpdateInput(state);
                    }
                    else
                    {
                        SyncScroll(
                            state,
                            false,
                            false);
                    }
                }
            }
        }

        return false;
    }

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC REGISTRATION
--------------------------------------------------------- */

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

    /*
        Best effort only: the module works without it,
        so the result does not affect registration success.
    */

    (void)Ultron_RegisterRmlEventHandler(
        kModuleName,
        "transitionend",
        &HandleRmlEvent);

    return click_registered && change_registered;
}


[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
