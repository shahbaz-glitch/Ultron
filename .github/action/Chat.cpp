
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>

namespace
{

// -------------------------------------------------------------
// Configuration
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingInputSizing";

constexpr const char* kInputId = "message-input";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kChatAreaId = "chat-area";

constexpr float kControlBaseSize = 14.22f;
constexpr float kInitialTextHeight = 7.35f;
constexpr float kInitialTextPadding = 1.23f;
constexpr float kMaximumTextHeight = 30.15f;
constexpr float kExtraGap = 1.50f;
constexpr float kChatSpacing = 11.52f;
constexpr float kOverflowTolerance = 1.0f;

// -------------------------------------------------------------
// Native input state
// -------------------------------------------------------------

struct InputState
{
    Rml::ObserverPtr<Rml::Element> textarea;

    Rml::String last_value;

    std::uint64_t revision = 0;
    std::uint64_t queued_revision = 0;

    float last_bar_height = -1.0f;

    bool first_line_shifted = false;
    bool update_pending = false;
};

InputState& GetInputState()
{
    static InputState state;
    return state;
}

// -------------------------------------------------------------
// Responsive pixel conversion
// -------------------------------------------------------------

float ScaleFromControl(float control_height, float design_value)
{
    return control_height *
        (design_value / kControlBaseSize);
}

Rml::String ToPixels(float value)
{
    char buffer[48]{};

    std::snprintf(
        buffer,
        sizeof(buffer),
        "%.3fpx",
        static_cast<double>(value));

    return Rml::String(buffer);
}

// -------------------------------------------------------------
// Resolve existing RML elements
// -------------------------------------------------------------

struct InputElements
{
    Rml::Element* textarea = nullptr;
    Rml::Element* input_bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat_area = nullptr;

    bool IsValid() const noexcept
    {
        return textarea != nullptr &&
               input_bar != nullptr &&
               wing_hit != nullptr &&
               chat_area != nullptr;
    }
};

bool ResolveElements(
    Rml::Element* textarea,
    InputElements& elements)
{
    if (textarea == nullptr ||
        textarea->GetId() != kInputId)
    {
        return false;
    }

    elements.textarea = textarea;
    elements.input_bar = textarea->GetParentNode();

    if (elements.input_bar == nullptr ||
        elements.input_bar->GetId() != kInputBarId)
    {
        return false;
    }

    elements.wing_hit =
        elements.input_bar->GetElementById(kWingHitId);

    Rml::Element* document_root =
        elements.input_bar->GetParentNode();

    if (document_root != nullptr)
    {
        elements.chat_area =
            document_root->GetElementById(kChatAreaId);
    }

    return elements.IsValid();
}

// -------------------------------------------------------------
// Reset native multiline layout
// -------------------------------------------------------------

void ResetMultilineLayout(
    InputState& state,
    const InputElements& elements)
{
    elements.input_bar->SetClass("multiline", false);

    elements.textarea->RemoveProperty("height");

    elements.input_bar->RemoveProperty("height");

    elements.chat_area->RemoveProperty("bottom");

    state.first_line_shifted = false;
    state.last_value.clear();
    state.last_bar_height = -1.0f;
}

// -------------------------------------------------------------
// Deferred layout scheduling
//
// Ultron processes pending UI tasks before Context::Update().
// Two stages ensure measurements happen after the previous
// RmlUi layout update has completed.
// -------------------------------------------------------------

void ApplyInputLayout(void* user_data);

void QueueNextLayoutPass(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (state == nullptr)
    {
        return;
    }

    if (state->textarea.get() == nullptr)
    {
        state->update_pending = false;
        return;
    }

    state->queued_revision = state->revision;

    if (!Ultron_PostToUI(
            &ApplyInputLayout,
            state))
    {
        state->update_pending = false;
    }
}

// -------------------------------------------------------------
// First-line detection and dynamic sizing
// -------------------------------------------------------------

void ApplyInputLayout(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (state == nullptr)
    {
        return;
    }

    Rml::Element* textarea =
        state->textarea.get();

    if (textarea == nullptr)
    {
        state->first_line_shifted = false;
        state->last_value.clear();
        state->last_bar_height = -1.0f;
        state->update_pending = false;
        return;
    }

    /*
        If a newer text event arrived after the
        previous context update, wait for its layout.
    */

    if (state->revision != state->queued_revision)
    {
        state->queued_revision = state->revision;

        if (!Ultron_PostToUI(
                &ApplyInputLayout,
                state))
        {
            state->update_pending = false;
        }

        return;
    }

    InputElements elements;

    if (!ResolveElements(textarea, elements))
    {
        state->update_pending = false;
        return;
    }

    const Rml::String message =
        textarea->GetAttribute<Rml::String>(
            "value",
            Rml::String{});


    // ---------------------------------------------------------
    // Empty input: restore original RML dimensions
    // ---------------------------------------------------------

    if (message.empty())
    {
        ResetMultilineLayout(*state, elements);

        state->update_pending = false;
        return;
    }


    // ---------------------------------------------------------
    // Responsive dimensions
    // ---------------------------------------------------------

    const float control_height =
        elements.wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
    {
        state->update_pending = false;
        return;
    }

    const float padding_height =
        ScaleFromControl(
            control_height,
            kInitialTextPadding);

    const float base_text_height =
        ScaleFromControl(
            control_height,
            kInitialTextHeight);

    const float max_text_height =
        ScaleFromControl(
            control_height,
            kMaximumTextHeight);

    const float extra_gap =
        ScaleFromControl(
            control_height,
            kExtraGap);


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
            Shift only when the original wing-side
            input area genuinely overflows vertically.
        */

        if (scroll_height <=
            client_height + kOverflowTolerance)
        {
            state->last_value = message;
            state->update_pending = false;
            return;
        }

        /*
            Preserve the shift until the input is cleared.
        */

        state->first_line_shifted = true;

        elements.input_bar->SetClass(
            "multiline",
            true);

        /*
            The multiline CSS changes textarea width
            and vertical position.

            Wait for that layout before measuring again.
        */

        state->last_value = message;
        state->last_bar_height = -1.0f;

        if (!Ultron_PostToUI(
                &ApplyInputLayout,
                state))
        {
            state->update_pending = false;
        }

        return;
    }


    // ---------------------------------------------------------
    // DYNAMIC TEXTAREA HEIGHT
    // ---------------------------------------------------------

    /*
        RmlUi GetScrollHeight() includes the current
        client area and the scrollable text overflow.
    */

    const float content_height =
        textarea->GetScrollHeight();

    const float new_text_height =
        std::clamp(
            std::max(content_height, base_text_height),
            base_text_height,
            max_text_height);

    /*
        CSS height is the content height.
        Remove the responsive vertical padding.
    */

    const float textarea_css_height =
        std::max(
            0.0f,
            new_text_height - padding_height);

    /*
        Avoid rewriting the same property on every
        character when the visual height has not changed.
    */

    if (std::fabs(
            textarea->GetOffsetHeight() -
            new_text_height) > 0.5f)
    {
        textarea->SetProperty(
            "height",
            ToPixels(textarea_css_height));
    }


    // ---------------------------------------------------------
    // INPUT BAR HEIGHT + EXTRA GAP
    // ---------------------------------------------------------

    const float bar_height =
        new_text_height +
        control_height +
        extra_gap;


    if (std::fabs(
            state->last_bar_height -
            bar_height) > 0.5f)
    {
        elements.input_bar->SetProperty(
            "height",
            ToPixels(bar_height));


        // -----------------------------------------------------
        // CHAT AREA POSITION
        // -----------------------------------------------------

        const float chat_bottom =
            bar_height +
            ScaleFromControl(
                control_height,
                kChatSpacing);

        elements.chat_area->SetProperty(
            "bottom",
            ToPixels(chat_bottom));

        state->last_bar_height = bar_height;
    }


    state->last_value = message;
    state->update_pending = false;
}

// -------------------------------------------------------------
// RmlUi change event
//
// RmlUi TextArea dispatches change after text insertion
// and deletion. The event runs after the value changes.
// -------------------------------------------------------------

bool OnMessageChanged(
    Rml::Event* event,
    void* /*user_data*/)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* textarea =
        event->GetTargetElement();

    if (textarea == nullptr ||
        textarea->GetId() != kInputId)
    {
        return false;
    }

    InputState& state = GetInputState();

    /*
        Handle document replacement safely.
    */

    if (state.textarea.get() != textarea)
    {
        state.textarea = textarea->GetObserverPtr();

        state.first_line_shifted = false;
        state.last_value.clear();
        state.last_bar_height = -1.0f;
    }

    const Rml::String current_value =
        textarea->GetAttribute<Rml::String>(
            "value",
            Rml::String{});

    /*
        When text is deleted or replaced with a
        shorter value, return the textarea to its
        responsive one-line height before measuring.

        The panel itself remains at its previous height
        until the new layout has been measured.
    */

    if (state.first_line_shifted &&
        current_value != state.last_value &&
        current_value.size() <= state.last_value.size())
    {
        Rml::Element* input_bar =
            textarea->GetParentNode();

        if (input_bar != nullptr)
        {
            Rml::Element* wing_hit =
                input_bar->GetElementById(kWingHitId);

            if (wing_hit != nullptr)
            {
                const float control_height =
                    wing_hit->GetOffsetHeight();

                const float base_height =
                    ScaleFromControl(
                        control_height,
                        kInitialTextHeight);

                textarea->SetProperty(
                    "height",
                    ToPixels(base_height));
            }
        }
    }

    ++state.revision;

    /*
        Coalesce rapid input events.
        Only one deferred layout operation is queued.
    */

    if (!state.update_pending)
    {
        state.update_pending = true;

        if (!Ultron_PostToUI(
                &QueueNextLayoutPass,
                &state))
        {
            state.update_pending = false;
        }
    }

    /*
        Do not consume the RmlUi change event.
        Existing click, pulse and other handlers remain intact.
    */

    return false;
}

// -------------------------------------------------------------
// Automatic module registration
// -------------------------------------------------------------

[[maybe_unused]]
const bool kRegistered =
    Ultron_RegisterRmlEventHandler(
        kModuleName,
        "change",
        &OnMessageChanged,
        nullptr,
        0);

} // namespace
