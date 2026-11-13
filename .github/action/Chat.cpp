
#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace
{

// -------------------------------------------------------------
// Module configuration
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingChat";

constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kMessageInputId = "message-input";
constexpr const char* kChatAreaId = "chat-area";

// -------------------------------------------------------------
// Responsive input dimensions
// Reference: portrait vw / landscape vh
// -------------------------------------------------------------

constexpr float kBaseTextareaHeight = 7.35f;
constexpr float kMaxTextareaHeight = 28.92f;
constexpr float kTextareaVerticalPadding = 1.23f;

constexpr float kBaseInputBarHeight = 14.22f;
constexpr float kInputBarBottom = 7.35f;
constexpr float kChatAreaGap = 4.17f;

// -------------------------------------------------------------
// Deferred text-state and layout updates
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;

bool g_text_refresh_queued = false;
bool g_input_layout_refresh_queued = false;

// -------------------------------------------------------------
// Check whether text contains non-whitespace characters
// -------------------------------------------------------------

bool HasMeaningfulText(const Rml::String& value)
{
    return std::any_of(
        value.begin(),
        value.end(),
        [](unsigned char character)
        {
            return std::isspace(character) == 0;
        });
}

// -------------------------------------------------------------
// Convert responsive value into an RCSS length
// -------------------------------------------------------------

Rml::String ToResponsiveLength(
    float value,
    const char* unit)
{
    return std::to_string(value) + unit;
}

// -------------------------------------------------------------
// Apply dynamic input height after text layout
// -------------------------------------------------------------

void ApplyDynamicInputLayout(void*)
{
    g_input_layout_refresh_queued = false;

    const Rml::ObserverPtr<Rml::Element> textarea_observer =
        g_pending_textarea;

    if (!textarea_observer)
    {
        return;
    }

    Rml::Element* textarea_element =
        textarea_observer.get();

    if (textarea_element == nullptr ||
        textarea_element->GetId() != kMessageInputId ||
        textarea_element->GetTagName() != "textarea")
    {
        return;
    }

    Rml::Element* input_bar =
        textarea_element->GetParentNode();

    if (input_bar == nullptr ||
        input_bar->GetId() != kInputBarId)
    {
        return;
    }

    Rml::ElementDocument* document =
        textarea_element->GetOwnerDocument();

    if (document == nullptr)
    {
        return;
    }

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (chat_area == nullptr)
    {
        return;
    }

    Rml::Context* context =
        textarea_element->GetContext();

    if (context == nullptr)
    {
        return;
    }

    const Rml::Vector2i dimensions =
        context->GetDimensions();

    if (dimensions.x <= 0 || dimensions.y <= 0)
    {
        return;
    }

    // Use the shorter viewport axis:
    // Portrait  = vw
    // Landscape = vh

    const bool portrait =
        dimensions.x <= dimensions.y;

    const float reference_dimension =
        static_cast<float>(
            portrait ? dimensions.x : dimensions.y);

    const float unit_pixels =
        reference_dimension / 100.0f;

    if (unit_pixels <= 0.0f)
    {
        return;
    }

    const char* responsive_unit =
        portrait ? "vw" : "vh";

    // ---------------------------------------------------------
    // Read actual textarea content height
    // ---------------------------------------------------------

    auto* textarea =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea_element);

    // GetScrollHeight includes padding.
    // Remove the original top and bottom textarea padding.

    const float measured_content_height =
        std::max(
            kBaseTextareaHeight * unit_pixels,
            textarea_element->GetScrollHeight() -
                (kTextareaVerticalPadding * unit_pixels));

    // Clamp textarea height to the existing maximum.
    // Beyond this limit, the textarea scrolls internally.

    const float target_textarea_height =
        std::clamp(
            measured_content_height,
            kBaseTextareaHeight * unit_pixels,
            kMaxTextareaHeight * unit_pixels);

    // ---------------------------------------------------------
    // Calculate responsive dimensions
    // ---------------------------------------------------------

    const float textarea_height_units =
        target_textarea_height / unit_pixels;

    // Keep the original input-bar height for one-line text.
    // Additional textarea height expands the bar upward.

    const float input_bar_height_units =
        kBaseInputBarHeight +
        (textarea_height_units - kBaseTextareaHeight);

    // Preserve the original gap between chat area and input bar.

    const float chat_area_bottom_units =
        kInputBarBottom +
        input_bar_height_units +
        kChatAreaGap;

    // ---------------------------------------------------------
    // Update textarea
    // ---------------------------------------------------------

    textarea_element->SetProperty(
        "height",
        ToResponsiveLength(
            textarea_height_units,
            responsive_unit));

    // Keep the textarea vertically centered in the growing bar.
    // The input bar itself remains anchored at its bottom.

    textarea_element->SetProperty(
        "margin-top",
        ToResponsiveLength(
            -textarea_height_units / 2.0f,
            responsive_unit));

    // ---------------------------------------------------------
    // Update input bar height
    // ---------------------------------------------------------

    input_bar->SetProperty(
        "height",
        ToResponsiveLength(
            input_bar_height_units,
            responsive_unit));

    // ---------------------------------------------------------
    // Update chat area position
    // ---------------------------------------------------------

    chat_area->SetProperty(
        "bottom",
        ToResponsiveLength(
            chat_area_bottom_units,
            responsive_unit));
}

// -------------------------------------------------------------
// Schedule dynamic layout update after RmlUi layout update
// -------------------------------------------------------------

void QueueDynamicInputLayout()
{
    if (g_input_layout_refresh_queued)
    {
        return;
    }

    g_input_layout_refresh_queued = true;

    // This second-stage task runs on the next UI-loop iteration,
    // after the current RmlUi Context::Update() has processed layout.

    if (!Ultron_PostToUI(
            &ApplyDynamicInputLayout,
            nullptr))
    {
        g_input_layout_refresh_queued = false;
    }
}

// -------------------------------------------------------------
// Apply has-text class after native text processing
// -------------------------------------------------------------

void ApplyHasTextState(void*)
{
    g_text_refresh_queued = false;

    const Rml::ObserverPtr<Rml::Element> textarea_observer =
        g_pending_textarea;

    if (!textarea_observer)
    {
        return;
    }

    Rml::Element* textarea_element =
        textarea_observer.get();

    if (textarea_element == nullptr ||
        textarea_element->GetId() != kMessageInputId ||
        textarea_element->GetTagName() != "textarea")
    {
        return;
    }

    Rml::Element* input_bar =
        textarea_element->GetParentNode();

    if (input_bar == nullptr ||
        input_bar->GetId() != kInputBarId)
    {
        return;
    }

    auto* textarea =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea_element);

    const bool has_text =
        HasMeaningfulText(textarea->GetValue());

    input_bar->SetClass("has-text", has_text);

    // Schedule height calculation after native text layout.
    QueueDynamicInputLayout();
}

// -------------------------------------------------------------
// Schedule text-state update on Ultron UI thread
// -------------------------------------------------------------

void QueueHasTextRefresh(Rml::Element* target)
{
    if (target == nullptr ||
        target->GetId() != kMessageInputId ||
        target->GetTagName() != "textarea")
    {
        return;
    }

    // ObserverPtr prevents access to a destroyed RmlUi element.
    g_pending_textarea = target->GetObserverPtr();

    // Coalesce multiple keyboard/text input events into one update.
    if (g_text_refresh_queued)
    {
        return;
    }

    g_text_refresh_queued = true;

    if (!Ultron_PostToUI(
            &ApplyHasTextState,
            nullptr))
    {
        g_text_refresh_queued = false;
    }
}

// -------------------------------------------------------------
// WING CLICK — OPEN / CLOSE INPUT BAR
// -------------------------------------------------------------

bool HandleWingClick(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    Rml::Element* target =
        event->GetTargetElement();

    if (target == nullptr ||
        target->GetId() != kWingHitId)
    {
        return false;
    }

    Rml::Element* input_bar =
        target->GetParentNode();

    if (input_bar == nullptr ||
        input_bar->GetId() != kInputBarId)
    {
        return false;
    }

    const bool currently_expanded =
        input_bar->IsClassSet("expanded");

    input_bar->SetClass(
        "expanded",
        !currently_expanded);

    // Preserve normal RmlUi event processing.
    return false;
}

// -------------------------------------------------------------
// TEXT INPUT — PULSE / ARROW
// -------------------------------------------------------------

bool HandleTextInput(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueHasTextRefresh(
        event->GetTargetElement());

    return false;
}

// -------------------------------------------------------------
// KEYBOARD INPUT — BACKSPACE / DELETE / ETC.
// -------------------------------------------------------------

bool HandleKeyDown(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueHasTextRefresh(
        event->GetTargetElement());

    // Do not consume Enter.
    // RmlUi's SDL3 backend forwards Return as '\n' text input.
    // The native multiline textarea handles the newline.

    return false;
}

// -------------------------------------------------------------
// TEXTAREA CHANGE — RELIABLE CONTENT UPDATE
// -------------------------------------------------------------

bool HandleTextareaChange(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueHasTextRefresh(
        event->GetTargetElement());

    return false;
}

// -------------------------------------------------------------
// Automatic module registration
// -------------------------------------------------------------

[[maybe_unused]]
const bool g_handlers_registered = []()
{
    const bool click_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "click",
            &HandleWingClick);

    const bool textinput_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "textinput",
            &HandleTextInput);

    const bool keydown_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "keydown",
            &HandleKeyDown);

    const bool change_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "change",
            &HandleTextareaChange);

    return click_registered &&
           textinput_registered &&
           keydown_registered &&
           change_registered;
}();

} // namespace
