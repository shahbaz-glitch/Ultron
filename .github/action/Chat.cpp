
#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <string>

namespace
{

// -------------------------------------------------------------
// MODULE CONFIGURATION
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingChat";

constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kMessageInputId = "message-input";
constexpr const char* kChatAreaId = "chat-area";

// -------------------------------------------------------------
// RESPONSIVE DIMENSIONS
// Portrait: vw
// Landscape: vh
// -------------------------------------------------------------

constexpr float kBaseTextareaHeight = 7.35f;
constexpr float kMaxTextareaHeight = 28.92f;

constexpr float kBaseInputBarHeight = 14.22f;
constexpr float kInputBarBottom = 7.35f;
constexpr float kChatAreaGap = 4.17f;

// -------------------------------------------------------------
// DEFERRED INPUT STATE
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;

bool g_input_refresh_queued = false;

// -------------------------------------------------------------
// TEXT CONTENT CHECK
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
// RESPONSIVE LENGTH
// -------------------------------------------------------------

Rml::String ToResponsiveLength(
    float value,
    const char* unit)
{
    return std::to_string(value) + unit;
}

// -------------------------------------------------------------
// COUNT LEADING NEWLINES
// -------------------------------------------------------------

std::size_t CountLeadingNewlines(
    const Rml::String& value)
{
    std::size_t count = 0;

    for (char character : value)
    {
        if (character != '\n')
            break;

        ++count;
    }

    return count;
}

// -------------------------------------------------------------
// COUNT TRAILING NEWLINES
// -------------------------------------------------------------

std::size_t CountTrailingNewlines(
    const Rml::String& value)
{
    std::size_t count = 0;

    for (auto iterator = value.rbegin();
         iterator != value.rend();
         ++iterator)
    {
        if (*iterator != '\n')
            break;

        ++count;
    }

    return count;
}

// -------------------------------------------------------------
// MEASURE ACTUAL FORMATTED TEXT LINES
//
// RmlUi textarea internally creates two ElementText children:
// 0 = normal text
// 1 = selected text
//
// Their line positions include actual word wrapping.
// -------------------------------------------------------------

std::size_t MeasureVisualLineCount(
    Rml::Element* textarea_element,
    const Rml::String& value)
{
    if (textarea_element == nullptr)
        return 1;

    const int child_count =
        textarea_element->GetNumChildren(true);

    float minimum_y = 0.0f;
    float maximum_y = 0.0f;

    bool found_line = false;

    // Inspect the two native text-rendering elements.

    for (int index = 0;
         index < std::min(child_count, 2);
         ++index)
    {
        Rml::Element* child =
            textarea_element->GetChild(index);

        if (child == nullptr ||
            child->GetTagName() != "#text")
        {
            continue;
        }

        auto* text_element =
            static_cast<Rml::ElementText*>(child);

        const auto& lines = text_element->GetLines();

        for (const auto& line : lines)
        {
            const float y = line.position.y;

            if (!found_line)
            {
                minimum_y = y;
                maximum_y = y;
                found_line = true;
            }
            else
            {
                minimum_y = std::min(minimum_y, y);
                maximum_y = std::max(maximum_y, y);
            }
        }
    }

    // Empty text or entirely blank lines.

    if (!found_line)
    {
        const std::size_t newline_count =
            static_cast<std::size_t>(
                std::count(value.begin(), value.end(), '\n'));

        return newline_count + 1;
    }

    // Match the rounded line height used by RmlUi's
    // native WidgetTextInput implementation.

    const float line_height =
        std::max(
            1.0f,
            std::round(textarea_element->GetLineHeight()));

    const float vertical_span =
        std::max(0.0f, maximum_y - minimum_y);

    const std::size_t rendered_rows =
        static_cast<std::size_t>(
            std::lround(vertical_span / line_height)) + 1;

    // Blank lines before and after rendered text do not
    // necessarily generate ElementText geometry.

    return rendered_rows +
           CountLeadingNewlines(value) +
           CountTrailingNewlines(value);
}

// -------------------------------------------------------------
// APPLY TEXT STATE AND DYNAMIC INPUT HEIGHT
// -------------------------------------------------------------

void ApplyInputState(void*)
{
    g_input_refresh_queued = false;

    const Rml::ObserverPtr<Rml::Element> observer =
        g_pending_textarea;

    if (!observer)
        return;

    Rml::Element* textarea_element =
        observer.get();

    if (textarea_element == nullptr ||
        textarea_element->GetId() != kMessageInputId ||
        textarea_element->GetTagName() != "textarea")
    {
        return;
    }

    Rml::ElementDocument* document =
        textarea_element->GetOwnerDocument();

    if (document == nullptr)
        return;

    Rml::Element* input_bar =
        document->GetElementById(kInputBarId);

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (input_bar == nullptr || chat_area == nullptr)
        return;

    auto* textarea =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea_element);

    // ---------------------------------------------------------
    // PULSE / ARROW STATE
    // ---------------------------------------------------------

    const Rml::String value =
        textarea->GetValue();

    const bool has_text =
        HasMeaningfulText(value);

    input_bar->SetClass("has-text", has_text);

    // ---------------------------------------------------------
    // DETERMINE RESPONSIVE UNIT
    // ---------------------------------------------------------

    Rml::Context* context =
        textarea_element->GetContext();

    if (context == nullptr)
        return;

    const Rml::Vector2i dimensions =
        context->GetDimensions();

    if (dimensions.x <= 0 || dimensions.y <= 0)
        return;

    const bool portrait =
        dimensions.x <= dimensions.y;

    const float reference_dimension =
        static_cast<float>(
            portrait ? dimensions.x : dimensions.y);

    const float unit_pixels =
        reference_dimension / 100.0f;

    if (unit_pixels <= 0.0f)
        return;

    const char* responsive_unit =
        portrait ? "vw" : "vh";

    // ---------------------------------------------------------
    // MEASURE NATIVE FORMATTED LINES
    // ---------------------------------------------------------

    const std::size_t visual_line_count =
        MeasureVisualLineCount(
            textarea_element,
            value);

    const float line_height_pixels =
        std::max(
            1.0f,
            std::round(textarea_element->GetLineHeight()));

    const float measured_content_height_pixels =
        static_cast<float>(visual_line_count) *
        line_height_pixels;

    // ---------------------------------------------------------
    // CALCULATE RESPONSIVE TEXTAREA HEIGHT
    // ---------------------------------------------------------

    const float measured_height_units =
        measured_content_height_pixels / unit_pixels;

    const float target_textarea_height =
        std::clamp(
            measured_height_units,
            kBaseTextareaHeight,
            kMaxTextareaHeight);

    // ---------------------------------------------------------
    // MAXIMUM HEIGHT / INTERNAL SCROLLING
    // ---------------------------------------------------------

    const bool needs_internal_scroll =
        measured_height_units > kMaxTextareaHeight;

    textarea_element->SetProperty(
        "overflow-y",
        needs_internal_scroll ? "auto" : "hidden");

    // ---------------------------------------------------------
    // INPUT BAR HEIGHT
    // ---------------------------------------------------------

    const bool expanded =
        input_bar->IsClassSet("expanded");

    const float input_bar_height =
        expanded
            ? kBaseInputBarHeight +
                (target_textarea_height -
                 kBaseTextareaHeight)
            : kBaseInputBarHeight;

    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    const float chat_area_bottom =
        kInputBarBottom +
        input_bar_height +
        kChatAreaGap;

    // ---------------------------------------------------------
    // UPDATE TEXTAREA
    // ---------------------------------------------------------

    textarea_element->SetProperty(
        "height",
        ToResponsiveLength(
            expanded
                ? target_textarea_height
                : kBaseTextareaHeight,
            responsive_unit));

    textarea_element->SetProperty(
        "margin-top",
        ToResponsiveLength(
            -(expanded
                ? target_textarea_height
                : kBaseTextareaHeight) / 2.0f,
            responsive_unit));

    // ---------------------------------------------------------
    // UPDATE INPUT BAR
    // ---------------------------------------------------------

    input_bar->SetProperty(
        "height",
        ToResponsiveLength(
            input_bar_height,
            responsive_unit));

    // ---------------------------------------------------------
    // UPDATE CHAT AREA
    // ---------------------------------------------------------

    chat_area->SetProperty(
        "bottom",
        ToResponsiveLength(
            chat_area_bottom,
            responsive_unit));
}

// -------------------------------------------------------------
// QUEUE INPUT UPDATE ON ULTRON UI THREAD
// -------------------------------------------------------------

void QueueInputRefresh(Rml::Element* target)
{
    if (target == nullptr ||
        target->GetId() != kMessageInputId ||
        target->GetTagName() != "textarea")
    {
        return;
    }

    g_pending_textarea =
        target->GetObserverPtr();

    if (g_input_refresh_queued)
        return;

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
        return false;

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

    return false;
}

// -------------------------------------------------------------
// TEXT INPUT — PULSE / ARROW + HEIGHT
// -------------------------------------------------------------

bool HandleTextInput(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

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
        return false;

    QueueInputRefresh(
        event->GetTargetElement());

    // Never consume Enter.
    // Multiline textarea keeps its native newline behaviour.

    return false;
}

// -------------------------------------------------------------
// CHANGE EVENT — TEXT MODIFICATION
// -------------------------------------------------------------

bool HandleTextareaChange(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

    QueueInputRefresh(
        event->GetTargetElement());

    return false;
}

// -------------------------------------------------------------
// AUTOMATIC MODULE REGISTRATION
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
