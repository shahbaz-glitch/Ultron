
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
constexpr float kFirstLineLeft = 4.17f;
constexpr float kFirstLineRight = 2.45f;
constexpr float kExtraGap = 1.5f;

struct InputState
{
    Rml::ElementDocument* document = nullptr;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;

    Rml::ElementFormControlTextArea* input = nullptr;

    bool first_line_shifted = false;
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

    auto* document = source->GetOwnerDocument();

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

    Rml::Element* element =
        document->GetElementById(kInputId);

    state.input =
        element && element->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(element)
        : nullptr;

    return state.bar &&
           state.wing_hit &&
           state.chat &&
           state.input;
}


/* ---------------------------------------------------------
   INTERNAL TEXT ELEMENT

   RmlUi 6.3 creates its textarea text element
   as the first internal child.
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
   CLASS API
--------------------------------------------------------- */

void SetClass(
    Rml::Element* element,
    const char* element_id,
    const char* class_name,
    bool enabled)
{
    if (element &&
        element->IsClassSet(class_name) != enabled)
    {
        Ultron_SetElementClass(
            element_id,
            class_name,
            enabled);
    }
}


/* ---------------------------------------------------------
   RESPONSIVE LENGTH
--------------------------------------------------------- */

std::string Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}


/* ---------------------------------------------------------
   EXACT RMLUI TEXT WRAP MEASUREMENT

   Uses RmlUi's own GenerateLine() algorithm.

   This avoids stale scrollHeight measurements
   before the next layout pass.
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
        std::max(
            1.0f,
            available_width - cursor_width);

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


        /*
            Match RmlUi's handling of trailing
            whitespace during word wrapping.
        */

        int editable_length =
            static_cast<int>(line_content.size());

        if (!line_content.empty() &&
            line_content.back() == '\n')
        {
            --editable_length;
        }

        const size_t space_begin =
            std::min(
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
            break;

        line_begin += line_size;
        ++line_count;

    }
    while (!last_line &&
           line_begin < static_cast<int>(text.size()));

    return std::max(line_count, 1);
}


/* ---------------------------------------------------------
   ORIGINAL INPUT LAYOUT RESET
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
}


/* ---------------------------------------------------------
   RESPONSIVE MULTILINE WIDTH
--------------------------------------------------------- */

float GetMultilineWidth(
    const InputState& state,
    float control_height)
{
    /*
        CSS:

        left  = 4.17vw / vh
        right = 2.45vw / vh

        Convert both offsets using the actual
        responsive wing-hit height.
    */

    const float left =
        control_height *
        (kFirstLineLeft / kControlUnits);

    const float right =
        control_height *
        (kFirstLineRight / kControlUnits);

    const float input_padding =
        state.input->GetBox()
            .GetFrameSize(Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        left -
        right -
        input_padding);
}


/* ---------------------------------------------------------
   MULTILINE HEIGHT
--------------------------------------------------------- */

void ApplyMultilineSize(
    InputState& state,
    Rml::ElementText* text_element,
    float control_height)
{
    const float cursor_width =
        std::round(
            Rml::ElementUtilities::
                GetDensityIndependentPixelRatio(state.input));

    /*
        After first-line shift, use the full
        textarea width rather than the wing-side width.
    */

    const float full_width =
        GetMultilineWidth(state, control_height);

    const int line_count =
        CountWrappedLines(
            text_element,
            full_width,
            cursor_width);

    /*
        Original responsive dimensions.
    */

    const float padding_height =
        control_height * (1.23f / kControlUnits);

    const float maximum_text_height =
        control_height * (30.15f / kControlUnits);

    const float extra_gap =
        control_height * (kExtraGap / kControlUnits);

    /*
        RmlUi's textarea uses a rounded line height.
    */

    const float line_height =
        std::round(state.input->GetLineHeight());

    const float natural_text_height =
        line_count * line_height + padding_height;

    const float text_height =
        std::min(
            natural_text_height,
            maximum_text_height);

    const float input_height =
        std::max(
            0.0f,
            text_height - padding_height);

    const float bar_height =
        text_height +
        control_height +
        extra_gap;

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / kControlUnits);


    /* Apply through the existing Ultron API */

    Ultron_SetElementProperty(
        kInputId,
        "height",
        Pixels(input_height).c_str());

    Ultron_SetElementProperty(
        kBarId,
        "height",
        Pixels(bar_height).c_str());

    Ultron_SetElementProperty(
        kChatId,
        "bottom",
        Pixels(chat_bottom).c_str());
}


/* ---------------------------------------------------------
   INPUT UPDATE
--------------------------------------------------------- */

void UpdateInput(InputState& state)
{
    if (!state.input ||
        !state.bar ||
        !state.wing_hit ||
        !state.chat)
    {
        return;
    }

    const Rml::String value =
        state.input->GetValue();

    const bool has_text =
        std::any_of(
            value.begin(),
            value.end(),
            [](unsigned char ch)
            {
                return std::isspace(ch) == 0;
            });

    /* Pulse -> Arrow */

    SetClass(
        state.bar,
        kBarId,
        "has-text",
        has_text);

    /* Empty input */

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
                GetDensityIndependentPixelRatio(state.input));


    /*
        FIRST-LINE DETECTION

        Measure using the actual narrow textarea
        width before multiline mode is activated.
    */

    if (!state.first_line_shifted)
    {
        const float first_line_width =
            state.input->GetClientWidth() -
            state.input->GetBox()
                .GetFrameSize(Rml::BoxArea::Padding).x;

        const int first_line_count =
            CountWrappedLines(
                text_element,
                first_line_width,
                cursor_width);

        if (first_line_count <= 1)
            return;

        /*
            First visual line is full.
            Permanently shift to full-width layout
            until the input becomes empty.
        */

        state.first_line_shifted = true;

        SetClass(
            state.bar,
            kBarId,
            "multiline",
            true);
    }


    /*
        FULL-WIDTH MULTILINE LAYOUT

        Always measure using the full-width
        textarea width after shifting.
    */

    ApplyMultilineSize(
        state,
        text_element,
        control_height);
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER

   click  -> Wing expand/collapse
   change -> Text state and multiline layout
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
