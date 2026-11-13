
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Event.h>

#include <algorithm>
#include <cctype>

namespace
{

// -------------------------------------------------------------
// Module configuration
// -------------------------------------------------------------

constexpr const char* kModuleName = "ObsidianWingChat";

constexpr const char* kWingHitId = "wing-hit";
constexpr const char* kInputBarId = "input-bar";
constexpr const char* kMessageInputId = "message-input";

// -------------------------------------------------------------
// Deferred text-state update
// -------------------------------------------------------------

Rml::ObserverPtr<Rml::Element> g_pending_textarea;

bool g_text_refresh_queued = false;

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
        textarea_observer.operator->();

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
// KEYBOARD INPUT — BACKSPACE / DELETE / PASTE / ETC.
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

    return click_registered &&
           textinput_registered &&
           keydown_registered;
}();

} // namespace

