
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";

constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kInputId[] = "message-input";


/* ---------------------------------------------------------
   ELEMENT BINDING
--------------------------------------------------------- */

bool BindElements(
    Rml::Element* source,
    Rml::Element*& bar,
    Rml::ElementFormControlTextArea*& input)
{
    if (!source)
        return false;

    Rml::ElementDocument* document =
        source->GetOwnerDocument();

    if (!document)
        return false;

    bar = document->GetElementById(kBarId);

    Rml::Element* input_element =
        document->GetElementById(kInputId);

    input =
        input_element &&
        input_element->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(
              input_element)
        : nullptr;

    return bar && input;
}


/* ---------------------------------------------------------
   TEXT DETECTION
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
   PULSE <-> ARROW
--------------------------------------------------------- */

void UpdateArrowState(
    Rml::Element* bar,
    Rml::ElementFormControlTextArea* input)
{
    if (!bar || !input)
        return;

    const bool has_text =
        HasVisibleText(input->GetValue());

    if (bar->IsClassSet("has-text") != has_text)
    {
        Ultron_SetElementClass(
            kBarId,
            "has-text",
            has_text);
    }
}


/* ---------------------------------------------------------
   RML EVENT HANDLER
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


    /* WING CLICK: EXPAND / COLLAPSE */

    if (event->GetType() == "click" &&
        target->GetId() == kWingHitId)
    {
        Rml::Element* bar = nullptr;

        Rml::ElementFormControlTextArea* input = nullptr;

        if (!BindElements(target, bar, input))
            return false;

        const bool expand =
            !bar->IsClassSet("expanded");

        Ultron_SetElementClass(
            kBarId,
            "expanded",
            expand);

        if (expand)
            input->Focus();

        return false;
    }


    /* TEXTAREA CHANGE: DIAGNOSTIC + PULSE / ARROW */

    if (event->GetType() == "change" &&
        target->GetId() == kInputId)
    {
        Rml::Element* bar = nullptr;

        Rml::ElementFormControlTextArea* input = nullptr;

        if (!BindElements(target, bar, input))
            return false;

        // Read actual textarea value.
        const Rml::String value =
            input->GetValue();

        // Prepare diagnostic message.
        const Rml::String log =
            "Textarea value [" +
            std::to_string(value.size()) +
            "]: " + value;

        // FIX: Explicit format string.
        Rml::Log::Message(
            Rml::Log::LT_INFO,
            "%s",
            log.c_str());

        // Preserve original arrow behaviour.
        UpdateArrowState(bar, input);

        return false;
    }

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC REGISTRATION
--------------------------------------------------------- */

bool RegisterObsidianWingChat()
{
    return Ultron_RegisterRmlEventHandler(
        kModuleName,
        "click",
        &HandleRmlEvent)
        &&
        Ultron_RegisterRmlEventHandler(
            kModuleName,
            "change",
            &HandleRmlEvent);
}


[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
