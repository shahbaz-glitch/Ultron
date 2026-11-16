
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace
{

constexpr const char* kModuleName =
    "ObsidianWingInputSizing";

constexpr const char* kInputId = "message-input";
constexpr const char* kBarId = "input-bar";
constexpr const char* kWingId = "wing-hit";
constexpr const char* kChatId = "chat-area";

constexpr float kControlBase = 14.22f;
constexpr float kBaseTextHeight = 7.35f;
constexpr float kPaddingHeight = 1.23f;
constexpr float kMaximumTextHeight = 30.15f;
constexpr float kExtraGap = 1.50f;
constexpr float kChatSpacing = 11.52f;
constexpr float kOverflowTolerance = 1.0f;


// -------------------------------------------------------------
// INPUT STATE
// -------------------------------------------------------------

struct InputState
{
    Rml::ObserverPtr<Rml::Element> textarea;

    Rml::String last_value;

    float last_text_height = -1.0f;
    float last_bar_height = -1.0f;

    bool first_line_shifted = false;
    bool update_pending = false;
};

InputState g_input_state;


// -------------------------------------------------------------
// RESPONSIVE HELPERS
// -------------------------------------------------------------

float ScaleValue(float control_height, float design_value)
{
    return control_height * (design_value / kControlBase);
}

Rml::String PixelValue(float value)
{
    char buffer[48]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3fpx",
        static_cast<double>(value));

    return Rml::String(buffer);
}

bool HasVisibleText(const Rml::String& value)
{
    for (unsigned char character : value)
    {
        if (!std::isspace(character))
            return true;
    }

    return false;
}


// -------------------------------------------------------------
// EXISTING ELEMENT RESOLUTION
// -------------------------------------------------------------

struct InputElements
{
    Rml::Element* textarea = nullptr;
    Rml::Element* input_bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat_area = nullptr;
    Rml::ElementDocument* document = nullptr;

    bool Valid() const
    {
        return textarea &&
               input_bar &&
               wing_hit &&
               chat_area &&
               document;
    }
};

bool ResolveElements(
    Rml::Element* textarea,
    InputElements& elements)
{
    if (!textarea || textarea->GetId() != kInputId)
        return false;

    elements.textarea = textarea;
    elements.input_bar = textarea->GetParentNode();

    if (!elements.input_bar ||
        elements.input_bar->GetId() != kBarId)
    {
        return false;
    }

    elements.document = textarea->GetOwnerDocument();

    if (!elements.document)
        return false;

    elements.wing_hit =
        elements.input_bar->GetElementById(kWingId);

    elements.chat_area =
        elements.document->GetElementById(kChatId);

    return elements.Valid();
}


// -------------------------------------------------------------
// EXPAND / COLLAPSE
//
// This handler was missing in the previous C++ version.
// It uses the existing RML classes and existing SVG.
// -------------------------------------------------------------

bool OnWingClick(
    Rml::Event* event,
    void* /*user_data*/)
{
    if (!event)
        return false;

    Rml::Element* target =
        event->GetTargetElement();

    if (!target || target->GetId() != kWingId)
        return false;

    Rml::Element* input_bar =
        target->GetParentNode();

    if (!input_bar || input_bar->GetId() != kBarId)
        return false;

    const bool should_expand =
        !input_bar->IsClassSet("expanded");

    input_bar->SetClass(
        "expanded",
        should_expand);

    if (should_expand)
    {
        Rml::Element* textarea =
            input_bar->GetElementById(kInputId);

        if (textarea)
            textarea->Focus();
    }

    return false;
}


// -------------------------------------------------------------
// RESET MULTILINE STATE
// -------------------------------------------------------------

void ResetInputLayout(
    InputState& state,
    const InputElements& elements)
{
    elements.input_bar->SetClass(
        "multiline",
        false);

    elements.textarea->RemoveProperty("height");

    elements.input_bar->RemoveProperty("height");

    elements.chat_area->RemoveProperty("bottom");

    state.first_line_shifted = false;

    state.last_value.clear();

    state.last_text_height = -1.0f;

    state.last_bar_height = -1.0f;
}


// -------------------------------------------------------------
// APPLY RESPONSIVE INPUT LAYOUT
//
// Called through Ultron's existing UI task queue.
// Geometry is refreshed before measurements.
// -------------------------------------------------------------

void ApplyInputLayout(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (!state)
        return;

    state->update_pending = false;

    Rml::Element* textarea =
        state->textarea.get();

    if (!textarea)
    {
        state->first_line_shifted = false;
        state->last_value.clear();
        state->last_text_height = -1.0f;
        state->last_bar_height = -1.0f;
        return;
    }

    InputElements elements;

    if (!ResolveElements(textarea, elements))
        return;

    /*
        Explicitly refresh document layout before
        querying element dimensions.
    */

    elements.document->UpdateDocument();

    const Rml::String message =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea)->GetValue();


    // ---------------------------------------------------------
    // EMPTY INPUT
    // ---------------------------------------------------------

    if (message.empty())
    {
        ResetInputLayout(*state, elements);

        elements.input_bar->SetClass(
            "has-text",
            false);

        elements.document->UpdateDocument();

        return;
    }


    // ---------------------------------------------------------
    // RESPONSIVE DIMENSIONS
    // ---------------------------------------------------------

    const float control_height =
        elements.wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
        return;

    const float padding_height =
        ScaleValue(
            control_height,
            kPaddingHeight);

    const float base_text_height =
        ScaleValue(
            control_height,
            kBaseTextHeight);

    const float max_text_height =
        ScaleValue(
            control_height,
            kMaximumTextHeight);

    const float extra_gap =
        ScaleValue(
            control_height,
            kExtraGap);


    // ---------------------------------------------------------
    // DETECT CONTENT SHRINKING
    // ---------------------------------------------------------

    const bool content_shrunk =
        message.size() < state->last_value.size() ||
        (
            message.size() == state->last_value.size() &&
            message != state->last_value
        );


    // ---------------------------------------------------------
    // FIRST LINE SHIFT
    // ---------------------------------------------------------

    if (!state->first_line_shifted)
    {
        const float scroll_height =
            textarea->GetScrollHeight();

        const float client_height =
            textarea->GetClientHeight();

        /*
            The original wing-side text area must
            genuinely overflow before shifting.
        */

        if (scroll_height <=
            client_height + kOverflowTolerance)
        {
            state->last_value = message;
            return;
        }

        state->first_line_shifted = true;

        elements.input_bar->SetClass(
            "multiline",
            true);

        /*
            Reset the textarea's previous one-line
            height and refresh its new full-width layout.
        */

        textarea->SetProperty(
            "height",
            "0px");

        state->last_text_height = -1.0f;
        state->last_bar_height = -1.0f;

        elements.document->UpdateDocument();
    }

    else if (content_shrunk)
    {
        /*
            Allow the textarea to shrink naturally
            after deleting text.
        */

        textarea->SetProperty(
            "height",
            "0px");

        state->last_text_height = -1.0f;
        state->last_bar_height = -1.0f;

        elements.document->UpdateDocument();
    }


    // ---------------------------------------------------------
    // DYNAMIC TEXT HEIGHT
    // ---------------------------------------------------------

    const float content_height =
        textarea->GetScrollHeight();

    const float new_text_height =
        std::clamp(
            std::max(
                content_height,
                base_text_height),
            base_text_height,
            max_text_height);

    const float textarea_height =
        std::max(
            0.0f,
            new_text_height - padding_height);


    // ---------------------------------------------------------
    // INPUT BAR HEIGHT + EXTRA GAP
    // ---------------------------------------------------------

    const float bar_height =
        new_text_height +
        control_height +
        extra_gap;


    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    const float chat_bottom =
        bar_height +
        ScaleValue(
            control_height,
            kChatSpacing);


    // ---------------------------------------------------------
    // APPLY ONLY CHANGED PROPERTIES
    // ---------------------------------------------------------

    if (state->last_text_height < 0.0f ||
        std::fabs(
            state->last_text_height -
            textarea_height) > 0.5f)
    {
        textarea->SetProperty(
            "height",
            PixelValue(textarea_height));

        state->last_text_height =
            textarea_height;
    }

    if (state->last_bar_height < 0.0f ||
        std::fabs(
            state->last_bar_height -
            bar_height) > 0.5f)
    {
        elements.input_bar->SetProperty(
            "height",
            PixelValue(bar_height));

        elements.chat_area->SetProperty(
            "bottom",
            PixelValue(chat_bottom));

        state->last_bar_height =
            bar_height;
    }

    elements.document->UpdateDocument();

    state->last_value = message;
}


// -------------------------------------------------------------
// TEXT CHANGE EVENT
//
// Also preserves the existing has-text class behavior
// for the pulse-to-arrow CSS transition.
// -------------------------------------------------------------

bool OnInputChanged(
    Rml::Event* event,
    void* /*user_data*/)
{
    if (!event)
        return false;

    Rml::Element* textarea =
        event->GetTargetElement();

    if (!textarea || textarea->GetId() != kInputId)
        return false;

    Rml::Element* input_bar =
        textarea->GetParentNode();

    if (!input_bar || input_bar->GetId() != kBarId)
        return false;

    const Rml::String message =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea)->GetValue();

    /*
        Update the existing pulse/arrow class.
        No SVG or CSS duplication.
    */

    input_bar->SetClass(
        "has-text",
        HasVisibleText(message));

    InputState& state = g_input_state;

    state.textarea =
        textarea->GetObserverPtr();

    /*
        Coalesce rapid input events.
        At most one sizing task is queued at a time.
    */

    if (!state.update_pending)
    {
        state.update_pending = true;

        if (!Ultron_PostToUI(
                &ApplyInputLayout,
                &state))
        {
            state.update_pending = false;
        }
    }

    /*
        Allow normal RmlUi event propagation.
    */

    return false;
}


// -------------------------------------------------------------
// AUTOMATIC REGISTRATION
//
// Both events are registered under the same module.
// No main.cpp or ultron_api.h changes.
// -------------------------------------------------------------

[[maybe_unused]]
const bool kWingClickRegistered =
    Ultron_RegisterRmlEventHandler(
        kModuleName,
        "click",
        &OnWingClick,
        nullptr,
        0);

[[maybe_unused]]
const bool kInputChangeRegistered =
    Ultron_RegisterRmlEventHandler(
        kModuleName,
        "change",
        &OnInputChanged,
        nullptr,
        0);

} // namespace
