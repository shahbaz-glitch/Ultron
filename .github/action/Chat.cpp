
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
#include <string>
#include <vector>

namespace
{

// -------------------------------------------------------------
// MODULE CONFIGURATION
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingChat";

constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kInputViewportId = "message-input-viewport";
constexpr const char* kMessageInputId = "message-input";
constexpr const char* kChatAreaId = "chat-area";

// -------------------------------------------------------------
// RESPONSIVE DIMENSIONS
// Portrait: vw
// Landscape: vh
// -------------------------------------------------------------

constexpr float kBaseInputBarHeight = 14.22f;

constexpr float kInputBarBottom = 7.35f;
constexpr float kChatAreaGap = 4.17f;

constexpr float kVerticalInsets = 6.87f;

constexpr float kTextareaPaddingVertical = 1.23f;
constexpr float kTextareaMaxBoxHeight = 28.92f;

constexpr float kMinimumLineHeightPixels = 1.0f;

// -------------------------------------------------------------
// DEFERRED UI STATE
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;
Rml::ObserverPtr<Rml::Element> g_scrollable_textarea;
Rml::ObserverPtr<Rml::Element> g_last_input_bar;

bool g_input_refresh_queued = false;

float g_last_input_height_units = -1.0f;
float g_last_chat_bottom_units = -1.0f;

std::string g_last_responsive_unit;

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
// COUNT EXPLICIT NEWLINES
// -------------------------------------------------------------

std::size_t CountLeadingNewlines(const Rml::String& value)
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

std::size_t CountTrailingNewlines(const Rml::String& value)
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
// MEASURE ACTUAL RMLUI FORMATTED TEXT ROWS
//
// RmlUi's textarea widget creates two hidden ElementText
// children:
//
// 1. Normal text
// 2. Selected text
//
// GetLines() contains formatted line positions, including
// automatically wrapped lines and manually entered newlines.
// -------------------------------------------------------------

std::size_t MeasureVisualLineCount(
    Rml::Element* textarea_element,
    const Rml::String& value)
{
    if (textarea_element == nullptr)
        return 1;

    std::vector<float> line_positions;

    const int visible_children =
        textarea_element->GetNumChildren();

    const int all_children =
        textarea_element->GetNumChildren(true);

    // Only inspect the native textarea's hidden text children.

    for (int index = visible_children;
         index < all_children;
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

        for (const Rml::ElementText::Line& line :
             text_element->GetLines())
        {
            line_positions.push_back(line.position.y);
        }
    }

    const std::size_t leading_newlines =
        CountLeadingNewlines(value);

    const std::size_t trailing_newlines =
        CountTrailingNewlines(value);

    // No visible text geometry, such as an empty value or
    // a value containing only explicit blank lines.

    if (line_positions.empty())
    {
        const std::size_t newline_count =
            static_cast<std::size_t>(
                std::count(value.begin(), value.end(), '\n'));

        return newline_count + 1;
    }

    std::sort(
        line_positions.begin(),
        line_positions.end());

    const float line_height =
        std::max(
            kMinimumLineHeightPixels,
            textarea_element->GetLineHeight());

    const float vertical_span =
        std::max(
            0.0f,
            line_positions.back() - line_positions.front());

    // Reconstruct the number of formatted rows from the
    // vertical distance between the first and last rows.

    const std::size_t rendered_rows =
        static_cast<std::size_t>(
            std::lround(vertical_span / line_height)) + 1;

    // Blank lines at the beginning and end do not always
    // generate visible text geometry.

    return std::max<std::size_t>(
        1,
        rendered_rows +
            leading_newlines +
            trailing_newlines);
}

// -------------------------------------------------------------
// ENABLE NATIVE TEXTAREA SCROLLING ONCE
// -------------------------------------------------------------

void EnsureScrollableTextarea(Rml::Element* textarea)
{
    if (textarea == nullptr ||
        textarea->GetId() != kMessageInputId)
    {
        return;
    }

    if (g_scrollable_textarea.get() == textarea)
    {
        return;
    }

    // RmlUi's native text widget initially sets overflow-y
    // to hidden. Override it once so its internal scrollbar
    // becomes available after the maximum height is reached.

    textarea->SetProperty("overflow-y", "auto");

    g_scrollable_textarea =
        textarea->GetObserverPtr();
}

// -------------------------------------------------------------
// CONVERT RESPONSIVE DIMENSION TO RCSS LENGTH
// -------------------------------------------------------------

Rml::String ToResponsiveLength(
    float value,
    const char* unit)
{
    return std::to_string(value) + unit;
}

// -------------------------------------------------------------
// APPLY INPUT HEIGHT AND CHAT POSITION
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

    // ---------------------------------------------------------
    // NEW RML STRUCTURE:
    //
    // input-bar
    //   └── message-input-viewport
    //         └── message-input
    // ---------------------------------------------------------

    Rml::Element* viewport =
        textarea_element->GetParentNode();

    if (viewport == nullptr ||
        viewport->GetId() != kInputViewportId)
    {
        return;
    }

    Rml::Element* input_bar =
        viewport->GetParentNode();

    if (input_bar == nullptr ||
        input_bar->GetId() != kInputBarId)
    {
        return;
    }

    Rml::Element* chat_area =
        document->GetElementById(kChatAreaId);

    if (chat_area == nullptr)
        return;

    auto* textarea =
        static_cast<Rml::ElementFormControlTextArea*>(
            textarea_element);

    // ---------------------------------------------------------
    // HAS-TEXT STATE
    // ---------------------------------------------------------

    const Rml::String value =
        textarea->GetValue();

    const bool has_text =
        HasMeaningfulText(value);

    if (input_bar->IsClassSet("has-text") != has_text)
    {
        input_bar->SetClass("has-text", has_text);
    }

    // ---------------------------------------------------------
    // RESPONSIVE VIEWPORT
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
    // ACTUAL FORMATTED LINE COUNT
    // ---------------------------------------------------------

    const std::size_t line_count =
        MeasureVisualLineCount(
            textarea_element,
            value);

    const float line_height_pixels =
        std::max(
            kMinimumLineHeightPixels,
            textarea_element->GetLineHeight());

    const float line_height_units =
        line_height_pixels / unit_pixels;

    // ---------------------------------------------------------
    // CALCULATE REQUIRED INPUT BAR HEIGHT
    //
    // The textarea itself never changes height.
    // Only the clipping viewport and outer panel grow.
    // ---------------------------------------------------------

    const bool expanded =
        input_bar->IsClassSet("expanded");

    float desired_input_height =
        kBaseInputBarHeight;

    if (expanded)
    {
        const float required_height =
            kVerticalInsets +
            kTextareaPaddingVertical +
            static_cast<float>(line_count) *
                line_height_units;

        const float maximum_input_height =
            kVerticalInsets +
            kTextareaMaxBoxHeight;

        desired_input_height =
            std::clamp(
                std::max(
                    kBaseInputBarHeight,
                    required_height),
                kBaseInputBarHeight,
                maximum_input_height);
    }

    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    const float desired_chat_bottom =
        kInputBarBottom +
        desired_input_height +
        kChatAreaGap;

    // ---------------------------------------------------------
    // RESET CACHE WHEN DOCUMENT ELEMENT CHANGES
    // ---------------------------------------------------------

    if (g_last_input_bar.get() != input_bar)
    {
        g_last_input_bar =
            input_bar->GetObserverPtr();

        g_last_input_height_units = -1.0f;
        g_last_chat_bottom_units = -1.0f;
        g_last_responsive_unit.clear();
    }

    const bool unit_changed =
        g_last_responsive_unit != responsive_unit;

    // ---------------------------------------------------------
    // IMPORTANT:
    // Update only when the visual row count changes.
    //
    // Never change textarea height, width or margin here.
    // ---------------------------------------------------------

    if (unit_changed ||
        std::fabs(
            desired_input_height -
            g_last_input_height_units) > 0.01f)
    {
        const bool success =
            input_bar->SetProperty(
                "height",
                ToResponsiveLength(
                    desired_input_height,
                    responsive_unit));

        if (!success)
            return;

        g_last_input_height_units =
            desired_input_height;
    }

    // ---------------------------------------------------------
    // UPDATE CHAT AREA ONLY WHEN REQUIRED
    // ---------------------------------------------------------

    if (unit_changed ||
        std::fabs(
            desired_chat_bottom -
            g_last_chat_bottom_units) > 0.01f)
    {
        const bool success =
            chat_area->SetProperty(
                "bottom",
                ToResponsiveLength(
                    desired_chat_bottom,
                    responsive_unit));

        if (!success)
            return;

        g_last_chat_bottom_units =
            desired_chat_bottom;
    }

    g_last_responsive_unit =
        responsive_unit;
}

// -------------------------------------------------------------
// QUEUE REFRESH ON ULTRON UI THREAD
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

    // Coalesce textinput, keydown and change notifications
    // into a single update after native RmlUi processing.

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

    Rml::ElementDocument* document =
        input_bar->GetOwnerDocument();

    if (document != nullptr)
    {
        QueueInputRefresh(
            document->GetElementById(kMessageInputId));
    }

    return false;
}

// -------------------------------------------------------------
// TEXTAREA FOCUS — PREPARE INTERNAL SCROLLING
// -------------------------------------------------------------

bool HandleTextareaFocus(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

    Rml::Element* target =
        event->GetTargetElement();

    if (target == nullptr ||
        target->GetId() != kMessageInputId)
    {
        return false;
    }

    // Set overflow behavior before the user starts typing.
    EnsureScrollableTextarea(target);

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
        return false;

    QueueInputRefresh(
        event->GetTargetElement());

    return false;
}

// -------------------------------------------------------------
// KEYBOARD INPUT — BACKSPACE / DELETE / ENTER
// -------------------------------------------------------------

bool HandleKeyDown(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

    QueueInputRefresh(
        event->GetTargetElement());

    // Do not consume Enter or modify the textarea's value.
    // SDL3's RmlUi backend already forwards Return as '\n'.
    // The native multiline textarea handles the newline.

    return false;
}

// -------------------------------------------------------------
// CHANGE EVENT — INSERT / DELETE / PASTE / CUT
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

    const bool focus_registered =
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "focus",
            &HandleTextareaFocus);

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
           focus_registered &&
           textinput_registered &&
           keydown_registered &&
           change_registered;
}();

} // namespace
