
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
// Responsive dimensions
// Reference: Portrait = vw, Landscape = vh
// -------------------------------------------------------------

constexpr float kBaseTextareaHeight = 7.35f;
constexpr float kMaxTextareaHeight = 28.92f;

constexpr float kBaseInputBarHeight = 14.22f;

constexpr float kInputBarBottom = 7.35f;
constexpr float kChatAreaGap = 4.17f;

// -------------------------------------------------------------
// Deferred input update
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;

bool g_input_refresh_queued = false;

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
// Convert a responsive value to an RML length
// -------------------------------------------------------------

Rml::String ToResponsiveLength(
    float value,
    const char* unit)
{
    return std::to_string(value) + unit;
}

// -------------------------------------------------------------
// Apply text state and dynamic input layout
// -------------------------------------------------------------

void ApplyInputState(void*)
{
    g_input_refresh_queued = false;

    const Rml::ObserverPtr<Rml::Element> observer =
        g_pending_textarea;

    if (!observer)
    {
        return;
    }

    Rml::Element* textarea_element = observer.get();

    if (textarea_element == nullptr ||
        textarea_element->GetId() != kMessageInputId ||
        textarea_element->GetTagName() != "textarea")
    {
        return;
    }

    Rml::ElementDocument* document =
        textarea_element->GetOwnerDocument();

    if (document == nullptr)
    {
        return;
    }

    // ---------------------------------------------------------
    // IMPORTANT:
    // Refresh native RmlUi text formatting and document layout
    // before reading the textarea's scrollable content height.
    // ---------------------------------------------------------

    document->UpdateDocument();

    // Revalidate after document update.
    if (!observer)
    {
        return;
    }

    textarea_element = observer.get();

    if (textarea_element == nullptr)
    {
        return;
    }

    Rml::Element* input_bar =
        document->GetElementById(kInputBarId);

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (input_bar == nullptr || chat_area == nullptr)
    {
        return;
    }

    auto* textarea =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea_element);

    // ---------------------------------------------------------
    // PULSE / ARROW STATE
    // ---------------------------------------------------------

    const bool has_text =
        HasMeaningfulText(textarea->GetValue());

    input_bar->SetClass("has-text", has_text);

    // ---------------------------------------------------------
    // Determine responsive viewport units
    // ---------------------------------------------------------

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

    float textarea_height_units =
        kBaseTextareaHeight;

    const bool expanded =
        input_bar->IsClassSet("expanded");

    if (expanded)
    {
        // RmlUi's scrollable height includes the element's
        // padding frame. Remove it to obtain content height.

        const float padding_frame_height =
            textarea_element->GetBox().GetFrameSize(
                Rml::BoxArea::Padding).y;

        const float content_height_pixels =
            std::max(
                0.0f,
                textarea_element->GetScrollHeight() -
                    padding_frame_height);

        const float measured_height_units =
            content_height_pixels / unit_pixels;

        // Keep original minimum and maximum dimensions.

        textarea_height_units =
            std::clamp(
                measured_height_units,
                kBaseTextareaHeight,
                kMaxTextareaHeight);
    }

    // ---------------------------------------------------------
    // Calculate input bar height
    // ---------------------------------------------------------

    float input_bar_height_units =
        kBaseInputBarHeight;

    if (expanded)
    {
        input_bar_height_units =
            kBaseInputBarHeight +
            (textarea_height_units - kBaseTextareaHeight);
    }

    // ---------------------------------------------------------
    // Calculate chat area bottom position
    // Preserve original spacing.
    // ---------------------------------------------------------

    const float chat_area_bottom_units =
        kInputBarBottom +
        input_bar_height_units +
        kChatAreaGap;

    // ---------------------------------------------------------
    // Update textarea height and vertical centering
    // ---------------------------------------------------------

    textarea_element->SetProperty(
        "height",
        ToResponsiveLength(
            textarea_height_units,
            responsive_unit));

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

    // ---------------------------------------------------------
    // Recalculate layout after changing dimensions
    // ---------------------------------------------------------

    document->UpdateDocument();
}

// -------------------------------------------------------------
// Queue input refresh on the UI thread
// -------------------------------------------------------------

void QueueInputRefresh(Rml::Element* target)
{
    if (target == nullptr ||
        target->GetId() != kMessageInputId ||
        target->GetTagName() != "textarea")
    {
        return;
    }

    g_pending_textarea = target->GetObserverPtr();

    // Combine multiple events into one UI update.

    if (g_input_refresh_queued)
    {
        return;
    }

    g_input_refresh_queued = true;

    if (!Ultron_PostToUI(
            &ApplyInputState,
            nullptr))
    {
        g_input_refresh_queued = false;
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

    // Recalculate dimensions when opening or closing.
    Rml::ElementDocument* document =
        input_bar->GetOwnerDocument();

    if (document != nullptr)
    {
        QueueInputRefresh(
            document->GetElementById(kMessageInputId));
    }

    // Preserve normal RmlUi event processing.
    return false;
}

// -------------------------------------------------------------
// TEXT INPUT — PULSE / ARROW + DYNAMIC HEIGHT
// -------------------------------------------------------------

bool HandleTextInput(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueInputRefresh(
        event->GetTargetElement());

    return false;
}

// -------------------------------------------------------------
// KEYBOARD INPUT — DELETE / BACKSPACE / ENTER
// -------------------------------------------------------------

bool HandleKeyDown(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueInputRefresh(
        event->GetTargetElement());

    // Do not consume Enter.
    // SDL3's RmlUi backend forwards Return as newline text input.
    // The multiline textarea handles it natively.

    return false;
}

// -------------------------------------------------------------
// TEXTAREA CHANGE — PASTE / CUT / TEXT MODIFICATIONS
// -------------------------------------------------------------

bool HandleTextareaChange(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
    {
        return false;
    }

    QueueInputRefresh(
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
