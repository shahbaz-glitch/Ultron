
#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>

#include <algorithm>
#include <cctype>
#include <string>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";
constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kInputId[] = "message-input";
constexpr char kChatId[] = "chat-area";


struct InputState
{
    Rml::ElementDocument* document = nullptr;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;

    Rml::ElementFormControlTextArea* input = nullptr;

    Rml::String last_value;

    bool first_line_shifted = false;
    bool update_queued = false;
    bool remeasure = false;
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
    if (source == nullptr)
        return false;

    Rml::ElementDocument* document =
        source->GetOwnerDocument();

    if (document == nullptr)
        return false;

    InputState& state = State();

    if (state.document != document)
    {
        const bool task_already_queued =
            state.update_queued;

        state = InputState{};

        state.document = document;
        state.update_queued = task_already_queued;
    }

    state.bar =
        document->GetElementById(kBarId);

    state.wing_hit =
        document->GetElementById(kWingHitId);

    state.chat =
        document->GetElementById(kChatId);

    Rml::Element* input =
        document->GetElementById(kInputId);

    state.input =
        input != nullptr && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    return state.bar != nullptr &&
           state.wing_hit != nullptr &&
           state.chat != nullptr &&
           state.input != nullptr;
}


/* ---------------------------------------------------------
   TEXT STATE
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


bool IsAppendOnly(
    const Rml::String& previous,
    const Rml::String& current)
{
    return current.size() >= previous.size() &&
           std::equal(
               previous.begin(),
               previous.end(),
               current.begin());
}


/* ---------------------------------------------------------
   EXISTING ULTRON CLASS API
--------------------------------------------------------- */

void SetClass(
    Rml::Element* element,
    const char* element_id,
    const char* class_name,
    bool enabled)
{
    if (element != nullptr &&
        element->IsClassSet(class_name) != enabled)
    {
        Ultron_SetElementClass(
            element_id,
            class_name,
            enabled);
    }
}


/* ---------------------------------------------------------
   RESPONSIVE PIXEL VALUE
--------------------------------------------------------- */

std::string Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}


/* ---------------------------------------------------------
   UI TASK SCHEDULER

   Coalesces multiple text changes into one UI update.
--------------------------------------------------------- */

void UpdateInputOnUI(void* user_data);


void QueueInputUpdate(InputState& state)
{
    if (state.update_queued)
        return;

    state.update_queued = true;

    if (!Ultron_PostToUI(
            &UpdateInputOnUI,
            &state))
    {
        state.update_queued = false;
    }
}


/* ---------------------------------------------------------
   RESET MULTILINE
--------------------------------------------------------- */

void ResetMultiline(InputState& state)
{
    state.first_line_shifted = false;
    state.remeasure = false;
    state.last_value.clear();

    SetClass(
        state.bar,
        kBarId,
        "multiline",
        false);

    state.input->RemoveProperty("height");
    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");
}


/* ---------------------------------------------------------
   MULTILINE RESPONSIVE HEIGHT
--------------------------------------------------------- */

void ApplyMultilineSize(InputState& state)
{
    const float control_height =
        state.wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
        return;

    /*
        Original responsive proportions:

        Padding = 1.23vw / vh
        Max text = 30.15vw / vh
        Extra gap = 1.5vw / vh
    */

    const float padding_height =
        control_height * (1.23f / 14.22f);

    const float max_text_height =
        control_height * (30.15f / 14.22f);

    const float extra_gap =
        control_height * (1.5f / 14.22f);


    /* Actual RmlUi textarea content dimensions */

    const float content_height = std::max(
        state.input->GetScrollHeight(),
        state.input->GetClientHeight());

    const float text_height =
        std::min(content_height, max_text_height);


    /* Input bar expands upward */

    const float bar_height =
        text_height +
        control_height +
        extra_gap;


    /* Chat area positioning */

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / 14.22f);


    /* Native RmlUi element properties */

    state.input->SetProperty(
        "height",
        Pixels(text_height - padding_height));

    state.bar->SetProperty(
        "height",
        Pixels(bar_height));

    state.chat->SetProperty(
        "bottom",
        Pixels(chat_bottom));
}


/* ---------------------------------------------------------
   INPUT UPDATE - UI THREAD ONLY
--------------------------------------------------------- */

void UpdateInputOnUI(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (state == nullptr)
        return;

    state->update_queued = false;

    if (state->input == nullptr ||
        state->bar == nullptr ||
        state->wing_hit == nullptr ||
        state->chat == nullptr)
    {
        return;
    }

    const Rml::String value =
        state->input->GetValue();


    /* Pulse -> Arrow */

    SetClass(
        state->bar,
        kBarId,
        "has-text",
        HasVisibleText(value));


    /* Empty input: restore original layout */

    if (value.empty())
    {
        ResetMultiline(*state);
        return;
    }


    /* First-line overflow detection */

    if (!state->first_line_shifted)
    {
        if (state->input->GetScrollHeight() <=
            state->input->GetClientHeight() + 1.0f)
        {
            return;
        }

        state->first_line_shifted = true;
        state->remeasure = true;

        SetClass(
            state->bar,
            kBarId,
            "multiline",
            true);
    }


    /*
        Re-measure after RmlUi applies the new
        multiline position and width.

        The queued task runs in the next UI pass.
    */

    if (state->remeasure)
    {
        state->remeasure = false;

        state->input->RemoveProperty("height");

        QueueInputUpdate(*state);
        return;
    }


    ApplyMultilineSize(*state);
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER

   Handles:
   1. Wing click
   2. Textarea change
--------------------------------------------------------- */

bool HandleRmlEvent(
    Rml::Event* event,
    void*)
{
    if (event == nullptr)
        return false;

    Rml::Element* target =
        event->GetTargetElement();

    if (target == nullptr)
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

    if (event->GetType() != "change" ||
        target->GetId() != kInputId)
    {
        return false;
    }

    if (!BindElements(target))
        return false;

    InputState& state = State();

    const Rml::String value =
        state.input->GetValue();


    /* Immediate pulse / arrow state */

    SetClass(
        state.bar,
        kBarId,
        "has-text",
        HasVisibleText(value));


    /*
        If the content is edited or reduced,
        reset measurement before recalculating.
    */

    if (value != state.last_value)
    {
        if (state.first_line_shifted &&
            !IsAppendOnly(state.last_value, value))
        {
            state.remeasure = true;
        }

        state.last_value = value;
    }

    QueueInputUpdate(state);

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC MODULE REGISTRATION
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


/* Register before main() creates the RmlUi context. */

[[maybe_unused]]
const bool g_registered = RegisterObsidianWingChat();

} // namespace
