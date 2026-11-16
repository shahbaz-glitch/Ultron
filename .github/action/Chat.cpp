
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";
constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kInputId[] = "message-input";
constexpr char kChatId[] = "chat-area";

constexpr float kControlUnits = 14.22f;
constexpr float kArrowSafetyUnits = 3.43f;
constexpr float kExtraGapUnits = 1.5f;


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
    float applied_bar_height = -1.0f;
    float applied_chat_bottom = -1.0f;
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
        const bool pending = state.update_queued;

        state = InputState{};

        state.document = document;
        state.update_queued = pending;
    }

    state.bar =
        document->GetElementById(kBarId);

    state.wing_hit =
        document->GetElementById(kWingHitId);

    state.chat =
        document->GetElementById(kChatId);

    Rml::Element* input =
        document->GetElementById(kInputId);

    state.input =
        input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    return state.bar &&
           state.wing_hit &&
           state.chat &&
           state.input;
}


/* ---------------------------------------------------------
   INTERNAL TEXT ELEMENT
--------------------------------------------------------- */

Rml::ElementText* GetTextElement(
    Rml::ElementFormControlTextArea* input)
{
    if (!input || input->GetNumChildren(true) < 1)
        return nullptr;

    // RmlUi 6.3 creates its primary textarea
    // ElementText as the first internal child.

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
    return std::any_of(
        value.begin(),
        value.end(),
        [](unsigned char ch)
        {
            return std::isspace(ch) == 0;
        });
}


/* ---------------------------------------------------------
   CLASS API
--------------------------------------------------------- */

void SetClass(
    Rml::Element* element,
    const char* id,
    const char* name,
    bool enabled)
{
    if (element && element->IsClassSet(name) != enabled)
    {
        Ultron_SetElementClass(
            id,
            name,
            enabled);
    }
}


/* ---------------------------------------------------------
   PIXEL VALUE
--------------------------------------------------------- */

std::string Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}


/* ---------------------------------------------------------
   AVOID UNNECESSARY PROPERTY UPDATES
--------------------------------------------------------- */

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
    state.applied_bar_height = -1.0f;
    state.applied_chat_bottom = -1.0f;
}


/* ---------------------------------------------------------
   COALESCED UI TASK
--------------------------------------------------------- */

void UpdateInputOnUI(void* user_data);


void QueueInputUpdate(InputState& state)
{
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


/* ---------------------------------------------------------
   RESET MULTILINE
--------------------------------------------------------- */

void ResetMultiline(InputState& state)
{
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

    state.input->RemoveProperty("height");
    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");

    InvalidateAppliedSize(state);
}


/* ---------------------------------------------------------
   FIRST-LINE ARROW SAFETY DETECTION
--------------------------------------------------------- */

bool FirstLineNearArrow(
    InputState& state,
    Rml::ElementText* text_element,
    float control_height)
{
    const float scale =
        control_height / kControlUnits;

    /*
        Use the textarea's actual available content width.
        This avoids measuring the wider parent panel instead.
    */

    const float input_padding_x =
        state.input->GetBox()
            .GetFrameSize(Rml::BoxArea::Padding).x;

    const float initial_width =
        state.input->GetClientWidth() -
        input_padding_x;

    if (initial_width <= 1.0f)
        return false;


    /*
        Already wrapped by RmlUi, including
        manually entered line breaks.
    */

    if (text_element->GetLines().size() > 1)
        return true;


    /*
        Reserve the space between:

        Textarea right = 12.99vw / vh

        Arrow left = 2.45 + 7.11
                   = 9.56vw / vh

        Safety gap = 3.43vw / vh
    */

    const float arrow_safety =
        scale * kArrowSafetyUnits;

    const float cursor_width = std::round(
        Rml::ElementUtilities::
            GetDensityIndependentPixelRatio(text_element));

    const float safe_line_width = std::max(
        1.0f,
        initial_width - arrow_safety - cursor_width);


    /*
        Use RmlUi's own word-wrapping algorithm.
        No manual character-width estimation.
    */

    Rml::String first_line;

    int consumed = 0;
    float rendered_width = 0.0f;

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


/* ---------------------------------------------------------
   STABLE MULTILINE HEIGHT
--------------------------------------------------------- */

void ApplyMultilineSize(
    InputState& state,
    Rml::ElementText* text_element,
    float control_height)
{
    /*
        Use the actual formatted RmlUi lines.
        Do not estimate lines independently.
    */

    const int line_count = std::max(
        1,
        static_cast<int>(
            text_element->GetLines().size()));


    const float line_height =
        std::round(state.input->GetLineHeight());

    const float padding_height =
        control_height * (1.23f / kControlUnits);

    const float maximum_text_height =
        control_height * (30.15f / kControlUnits);

    const float extra_gap =
        control_height * (kExtraGapUnits / kControlUnits);


    /* Natural multiline height */

    const float natural_text_height =
        static_cast<float>(line_count) *
        line_height +
        padding_height;

    const float text_height =
        std::min(
            natural_text_height,
            maximum_text_height);

    const float input_height =
        std::max(
            0.0f,
            text_height - padding_height);


    /* Panel height */

    const float bar_height =
        text_height +
        control_height +
        extra_gap;


    /* Chat area position */

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / kControlUnits);


    /*
        CRITICAL:

        When the current line count and dimensions
        have not changed, do absolutely nothing.

        This prevents unnecessary textarea relayout,
        panel flicker and caret repositioning.
    */

    if (line_count == state.applied_line_count &&
        state.applied_control_height >= 0.0f &&
        std::fabs(
            control_height -
            state.applied_control_height) < 0.5f &&

        state.applied_input_height >= 0.0f &&
        std::fabs(
            input_height -
            state.applied_input_height) < 0.5f &&

        state.applied_bar_height >= 0.0f &&
        std::fabs(
            bar_height -
            state.applied_bar_height) < 0.5f &&

        state.applied_chat_bottom >= 0.0f &&
        std::fabs(
            chat_bottom -
            state.applied_chat_bottom) < 0.5f)
    {
        return;
    }


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


    if (input_ok && bar_ok && chat_ok)
    {
        state.applied_line_count = line_count;

        state.applied_control_height =
            control_height;
    }
}


/* ---------------------------------------------------------
   INPUT UPDATE
--------------------------------------------------------- */

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


    /* Pulse -> Arrow */

    SetClass(
        state->bar,
        kBarId,
        "has-text",
        HasVisibleText(value));


    /* Empty input */

    if (value.empty())
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


    /* FIRST-LINE SHIFT */

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


        /*
            Switch textarea to full-width position.

            This happens only once until input is empty.
        */

        SetClass(
            state->bar,
            kBarId,
            "multiline",
            true);


        /*
            IMPORTANT:

            Do not measure the new layout here.

            The multiline class has just changed.
            RmlUi must complete its next layout pass first.

            Queue another UI pass for accurate line count.
        */

        QueueInputUpdate(*state);

        return;
    }


    /* FULL-WIDTH MULTILINE SIZE */

    ApplyMultilineSize(
        *state,
        text_element,
        control_height);
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER
--------------------------------------------------------- */

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

        InputState& state = State();

        const Rml::String value =
            state.input->GetValue();


        /*
            Pulse/arrow updates immediately.

            Geometry updates are deferred because
            RmlUi completes textarea formatting
            after dispatching the change event.
        */

        SetClass(
            state.bar,
            kBarId,
            "has-text",
            HasVisibleText(value));

        QueueInputUpdate(state);
    }

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC MODULE REGISTRATION
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

    return click_registered && change_registered;
}


[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
