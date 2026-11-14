#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cmath>
#include <cctype>
#include <string>

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

constexpr float kViewportInsets = 6.87f;

constexpr float kBaseViewportHeight = 7.35f;
constexpr float kTextareaPadding = 1.23f;

constexpr float kLineHeightReference = 5.88f;

constexpr std::size_t kMaximumVisibleLines = 8;

// -------------------------------------------------------------
// DEFERRED UI STATE
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;
Rml::ObserverPtr<Rml::Element> g_last_input_bar;

bool g_input_refresh_queued = false;

float g_last_input_height = -1.0f;
float g_last_chat_bottom = -1.0f;

std::string g_last_unit;

// -------------------------------------------------------------
// CHECK NON-WHITESPACE CONTENT
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
// COUNT LEADING NEWLINES
// -------------------------------------------------------------

std::size_t CountLeadingNewlines(
    const Rml::String& value)
{
    std::size_t count = 0;

    for (char character : value)
    {
        if (character != '\n' ||
            count >= kMaximumVisibleLines)
        {
            break;
        }

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
         iterator != value.rend(); ++iterator)
    {
        if (*iterator != '\n' ||
            count >= kMaximumVisibleLines)
        {
            break;
        }

        ++count;
    }

    return count;
}

// -------------------------------------------------------------
// MEASURE ACTUAL FORMATTED RMLUI LINES
//
// Textarea internal elements:
//  - Normal ElementText
//  - Selected ElementText
//
// Their line positions represent native formatted text,
// including automatically wrapped lines.
// -------------------------------------------------------------

std::size_t MeasureVisualLineCount(
    Rml::Element* textarea_element,
    const Rml::String& value,
    float line_height)
{
    if (textarea_element == nullptr)
        return 1;

    const int dom_children =
        textarea_element->GetNumChildren();

    const int total_children =
        textarea_element->GetNumChildren(true);

    float minimum_y = 0.0f;
    float maximum_y = 0.0f;

    bool found_text_line = false;

    // Inspect only the non-DOM text children generated
    // internally by RmlUi's native textarea widget.

    for (int index = dom_children;
         index < total_children;
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

        const auto& lines =
            text_element->GetLines();

        if (lines.empty())
            continue;

        // ElementText lines are generated in vertical order.
        // Only the first and last positions are required.

        const float first_y =
            lines.front().position.y;

        const float last_y =
            lines.back().position.y;

        if (!found_text_line)
        {
            minimum_y = first_y;
            maximum_y = last_y;
            found_text_line = true;
        }
        else
        {
            minimum_y =
                std::min(minimum_y, first_y);

            maximum_y =
                std::max(maximum_y, last_y);
        }

        // No need to count more than nine rows.
        // The panel can only display eight rows.
        if ((maximum_y - minimum_y) >=
            line_height *
                static_cast<float>(kMaximumVisibleLines))
        {
            return kMaximumVisibleLines + 1;
        }
    }

    // Empty text or only newline characters.

    if (!found_text_line)
    {
        std::size_t newlines = 0;

        for (char character : value)
        {
            if (character == '\n')
                ++newlines;

            if (newlines >= kMaximumVisibleLines)
                return kMaximumVisibleLines + 1;
        }

        return newlines + 1;
    }

    const float vertical_span =
        std::max(0.0f, maximum_y - minimum_y);

    const std::size_t rendered_lines =
        static_cast<std::size_t>(
            std::lround(vertical_span / line_height)) + 1;

    // Leading and trailing blank rows do not necessarily
    // produce visible ElementText geometry.

    const std::size_t total_lines =
        rendered_lines +
        CountLeadingNewlines(value) +
        CountTrailingNewlines(value);

    return std::max<std::size_t>(
        1,
        std::min(
            total_lines,
            kMaximumVisibleLines + 1));
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
// APPLY INPUT STATE
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
    // VERIFY INPUT ELEMENT HIERARCHY
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
    // PULSE / ARROW STATE
    // ---------------------------------------------------------

    const Rml::String value =
        textarea->GetValue();

    input_bar->SetClass(
        "has-text",
        HasMeaningfulText(value));

    // ---------------------------------------------------------
    // RESPONSIVE VIEWPORT
    // ---------------------------------------------------------

    const Rml::Vector2i dimensions =
        textarea_element->GetContext()->GetDimensions();

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

    const char* unit =
        portrait ? "vw" : "vh";

    // ---------------------------------------------------------
    // ACTUAL NATIVE LINE HEIGHT
    // Matches RmlUi's rounded textarea line formatting.
    // ---------------------------------------------------------

    const float line_height_pixels =
        std::max(
            1.0f,
            std::round(textarea_element->GetLineHeight()));

    const float line_height_units =
        line_height_pixels / unit_pixels;

    // ---------------------------------------------------------
    // MEASURE FORMATTED LINES
    // ---------------------------------------------------------

    const std::size_t line_count =
        MeasureVisualLineCount(
            textarea_element,
            value,
            line_height_pixels);

    // ---------------------------------------------------------
    // VISIBLE TEXT VIEWPORT HEIGHT
    // 1 line = minimum original size.
    // 8 lines = maximum visible size.
    // ---------------------------------------------------------

    const float calculated_viewport_height =
        kTextareaPadding +
        static_cast<float>(line_count) *
            line_height_units;

    const float viewport_height =
        std::clamp(
            std::max(
                kBaseViewportHeight,
                calculated_viewport_height),
            kBaseViewportHeight,
            kTextareaPadding +
                kLineHeightReference *
                static_cast<float>(kMaximumVisibleLines));

    // ---------------------------------------------------------
    // INPUT BAR HEIGHT
    // ---------------------------------------------------------

    const bool expanded =
        input_bar->IsClassSet("expanded");

    const float desired_input_height =
        expanded
            ? kViewportInsets + viewport_height
            : kBaseInputBarHeight;

    // ---------------------------------------------------------
    // CHAT AREA POSITION
    // ---------------------------------------------------------

    const float desired_chat_bottom =
        kInputBarBottom +
        desired_input_height +
        kChatAreaGap;

    // ---------------------------------------------------------
    // RESET CACHE FOR NEW DOCUMENT
    // ---------------------------------------------------------

    if (g_last_input_bar.get() != input_bar)
    {
        g_last_input_bar =
            input_bar->GetObserverPtr();

        g_last_input_height = -1.0f;
        g_last_chat_bottom = -1.0f;
        g_last_unit.clear();
    }

    const bool unit_changed =
        g_last_unit != unit;

    // ---------------------------------------------------------
    // UPDATE OUTER PANEL ONLY WHEN HEIGHT CHANGES
    //
    // Never modify the textarea's height, width, margin,
    // cursor, value or scroll position during typing.
    // ---------------------------------------------------------

    if (unit_changed ||
        std::fabs(
            desired_input_height -
            g_last_input_height) > 0.01f)
    {
        if (!input_bar->SetProperty(
                "height",
                ToResponsiveLength(
                    desired_input_height,
                    unit)))
        {
            return;
        }

        g_last_input_height =
            desired_input_height;
    }

    // ---------------------------------------------------------
    // UPDATE CHAT AREA ONLY WHEN POSITION CHANGES
    // ---------------------------------------------------------

    if (unit_changed ||
        std::fabs(
            desired_chat_bottom -
            g_last_chat_bottom) > 0.01f)
    {
        if (!chat_area->SetProperty(
                "bottom",
                ToResponsiveLength(
                    desired_chat_bottom,
                    unit)))
        {
            return;
        }

        g_last_chat_bottom =
            desired_chat_bottom;
    }

    g_last_unit = unit;
}

// -------------------------------------------------------------
// QUEUE INPUT UPDATE ON UI THREAD
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

    // Coalesce all related events into one update.
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
// WING CLICK — OPEN / CLOSE
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

    input_bar->SetClass(
        "expanded",
        !input_bar->IsClassSet("expanded"));

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
// TEXT INPUT
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
// KEYBOARD INPUT
// BACKSPACE / DELETE / ENTER / NAVIGATION
// -------------------------------------------------------------

bool HandleKeyDown(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

    QueueInputRefresh(
        event->GetTargetElement());

    // Preserve all native RmlUi textarea key handling.
    // Never consume Enter or modify the cursor.

    return false;
}

// -------------------------------------------------------------
// CHANGE EVENT
// INSERT / DELETE / PASTE / CUT
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
