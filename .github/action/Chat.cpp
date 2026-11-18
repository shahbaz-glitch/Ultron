#include "ultron_api.h"

#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";
constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kInputId[] = "message-input";
constexpr char kChatId[] = "chat-area";

constexpr float kControlUnits = 14.22f;
constexpr float kFirstLeft = 18.63f;
constexpr float kFirstRight = 12.99f;
constexpr float kMultilineLeft = 4.17f;
constexpr float kMultilineRight = 2.45f;
constexpr float kExtraGap = 1.5f;
constexpr float kGeometryEpsilon = 0.5f;
constexpr int kMaximumVisibleRows = 4;

struct InputState
{
    Rml::ObserverPtr<Rml::Element> document;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* chat = nullptr;
    Rml::ElementFormControlTextArea* input = nullptr;

    bool word_break_configured = false;
    bool first_line_shifted = false;
    bool update_queued = false;
    bool reconcile_queued = false;
    bool reset_pending = false;

    std::uint64_t revision = 0;
    std::uint64_t layout_revision = 0;

    int last_line_count = 0;

    float last_control_height = 0.0f;
    float last_panel_width = 0.0f;

    float last_input_height = -1.0f;
    float last_bar_height = -1.0f;
    float last_chat_bottom = -1.0f;
};

InputState& State()
{
    static InputState state;
    return state;
}

Rml::ElementText* GetTextElement(
    Rml::ElementFormControlTextArea* input)
{
    if (!input || input->GetNumChildren(true) < 1)
        return nullptr;

    Rml::Element* child = input->GetChild(0);

    if (!child || child->GetTagName() != "#text")
        return nullptr;

    return static_cast<Rml::ElementText*>(child);
}

bool BindDocumentElements(
    InputState& state,
    Rml::ElementDocument* document)
{
    if (!document)
        return false;

    if (state.document.get() != document)
    {
        const bool update_already_queued =
            state.update_queued;

        const bool reconcile_already_queued =
            state.reconcile_queued;

        state = InputState{};

        state.document = document->GetObserverPtr();

        state.update_queued = update_already_queued;
        state.reconcile_queued = reconcile_already_queued;
    }

    state.bar = document->GetElementById(kBarId);
    state.wing_hit = document->GetElementById(kWingHitId);
    state.chat = document->GetElementById(kChatId);

    Rml::Element* input = document->GetElementById(kInputId);

    state.input =
        input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    if (!state.bar ||
        !state.wing_hit ||
        !state.chat ||
        !state.input)
    {
        return false;
    }

    if (!state.word_break_configured)
    {
        state.input->SetProperty(
            "word-break",
            "break-all");

        state.word_break_configured = true;
    }

    return true;
}

bool BindElements(Rml::Element* source)
{
    if (!source)
        return false;

    InputState& state = State();

    return BindDocumentElements(
        state,
        source->GetOwnerDocument());
}

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

void SetClass(
    Rml::Element* element,
    const char* class_name,
    bool enabled)
{
    if (element &&
        element->IsClassSet(class_name) != enabled)
    {
        element->SetClass(class_name, enabled);
    }
}

Rml::String Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}

float GetInitialTextWidth(
    const InputState& state,
    float control_height)
{
    const float scale =
        control_height / kControlUnits;

    const float horizontal_padding =
        state.input->GetBox().GetFrameSize(
            Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        (kFirstLeft + kFirstRight) * scale -
        horizontal_padding);
}

float GetMultilineTextWidth(
    const InputState& state,
    float control_height)
{
    const float scale =
        control_height / kControlUnits;

    const float horizontal_padding =
        state.input->GetBox().GetFrameSize(
            Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        (kMultilineLeft + kMultilineRight) * scale -
        horizontal_padding);
}

float GetActualTextWidth(const InputState& state)
{
    const float horizontal_padding =
        state.input->GetBox().GetFrameSize(
            Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.input->GetClientWidth() -
        horizontal_padding);
}

void IncludeTrailingSpaces(
    const Rml::String& text,
    int line_begin,
    const Rml::String& line_content,
    int& line_size,
    bool& last_line)
{
    int editable_length =
        static_cast<int>(line_content.size());

    if (!line_content.empty() &&
        line_content.back() == '\n')
    {
        --editable_length;
    }

    const size_t space_begin = std::min(
        text.size(),
        static_cast<size_t>(
            line_begin + std::max(editable_length, 0)));

    const size_t found =
        text.find_first_not_of(' ', space_begin);

    const size_t space_end =
        found == Rml::String::npos
        ? text.size()
        : found;

    const size_t trailing_spaces =
        space_end - space_begin;

    if (trailing_spaces > 0)
    {
        line_size += static_cast<int>(trailing_spaces);

        if (space_end < text.size() &&
            text[space_end] == '\n')
        {
            ++line_size;
        }

        if (space_end == text.size())
            last_line = true;
    }
}

bool IsFirstLineComplete(
    Rml::ElementText* text_element,
    float available_width,
    float cursor_width)
{
    if (!text_element ||
        available_width <= cursor_width)
    {
        return false;
    }

    const Rml::String& text =
        text_element->GetText();

    if (text.empty())
        return false;

    Rml::String first_line;

    int line_size = 0;
    float line_width = 0.0f;

    bool last_line = text_element->GenerateLine(
        first_line,
        line_size,
        line_width,
        0,
        available_width - cursor_width,
        0.0f,
        false,
        false,
        false);

    if (first_line.find('\n') != Rml::String::npos)
        return true;

    IncludeTrailingSpaces(
        text,
        0,
        first_line,
        line_size,
        last_line);

    return !last_line &&
        line_size < static_cast<int>(text.size());
}

int CountWrappedLines(
    Rml::ElementText* text_element,
    float available_width,
    float cursor_width)
{
    if (!text_element)
        return 1;

    const Rml::String& text =
        text_element->GetText();

    if (text.empty())
        return 1;

    const float maximum_line_width =
        available_width - cursor_width;

    if (maximum_line_width <= 0.0f)
        return 1;

    int line_begin = 0;
    int line_count = 0;

    bool last_line = false;

    do
    {
        Rml::String line_content;

        int line_size = 0;
        float line_width = 0.0f;

        last_line = text_element->GenerateLine(
            line_content,
            line_size,
            line_width,
            line_begin,
            maximum_line_width,
            0.0f,
            false,
            false,
            false);

        IncludeTrailingSpaces(
            text,
            line_begin,
            line_content,
            line_size,
            last_line);

        if (line_size <= 0)
        {
            ++line_count;
            break;
        }

        line_begin += line_size;
        ++line_count;
    }
    while (!last_line);

    return std::max(line_count, 1);
}

void ResetMultilineLayout(InputState& state)
{
    state.first_line_shifted = false;
    state.reset_pending = false;

    state.last_line_count = 0;

    state.last_control_height = 0.0f;
    state.last_panel_width = 0.0f;

    state.last_input_height = -1.0f;
    state.last_bar_height = -1.0f;
    state.last_chat_bottom = -1.0f;

    SetClass(state.bar, "multiline", false);

    state.input->RemoveProperty("height");
    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");

    state.input->SetScrollLeft(0.0f);
    state.input->SetScrollTop(0.0f);
}

bool ApplyMultilineLayout(
    InputState& state,
    int line_count,
    float control_height,
    float full_text_width)
{
    const float padding_height =
        control_height * (1.23f / kControlUnits);

    const float extra_gap =
        control_height * (kExtraGap / kControlUnits);

    const float line_height =
        std::round(state.input->GetLineHeight());

    if (line_height <= 0.0f)
        return false;

    // Exactly four actual RmlUi visual line-heights.
    const float maximum_text_height =
        static_cast<float>(kMaximumVisibleRows) *
        line_height +
        padding_height;

    const float natural_text_height =
        static_cast<float>(line_count) *
        line_height +
        padding_height;

    const float text_height =
        std::min(
            natural_text_height,
            maximum_text_height);

    const float input_content_height =
        std::max(
            0.0f,
            text_height - padding_height);

    const float bar_height =
        text_height + control_height + extra_gap;

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / kControlUnits);

    const bool geometry_changed =
        std::fabs(
            input_content_height -
            state.last_input_height) > kGeometryEpsilon ||

        std::fabs(
            bar_height -
            state.last_bar_height) > kGeometryEpsilon ||

        std::fabs(
            chat_bottom -
            state.last_chat_bottom) > kGeometryEpsilon;

    if (geometry_changed)
    {
        state.input->SetProperty(
            "height",
            Pixels(input_content_height));

        state.bar->SetProperty(
            "height",
            Pixels(bar_height));

        state.chat->SetProperty(
            "bottom",
            Pixels(chat_bottom));

        state.last_input_height =
            input_content_height;

        state.last_bar_height =
            bar_height;

        state.last_chat_bottom =
            chat_bottom;
    }

    state.last_line_count = line_count;
    state.last_control_height = control_height;
    state.last_panel_width = full_text_width;

    return geometry_changed;
}

void QueueInputUpdate(InputState& state);
void QueueLayoutReconcile(InputState& state);

void ReconcileInputLayout(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (!state)
        return;

    state->reconcile_queued = false;

    Rml::Element* document_element =
        state->document.get();

    if (!document_element ||
        !BindDocumentElements(
            *state,
            static_cast<Rml::ElementDocument*>(document_element)))
    {
        return;
    }

    if (state->revision != state->layout_revision)
    {
        QueueInputUpdate(*state);
        return;
    }

    if (state->reset_pending)
        return;

    const Rml::String value =
        state->input->GetValue();

    if (value.empty() ||
        !state->first_line_shifted)
    {
        return;
    }

    Rml::ElementText* text_element =
        GetTextElement(state->input);

    const float control_height =
        state->wing_hit->GetOffsetHeight();

    if (!text_element || control_height <= 0.0f)
        return;

    const float cursor_width =
        std::round(
            Rml::ElementUtilities::
                GetDensityIndependentPixelRatio(text_element));

    const float actual_width =
        GetActualTextWidth(*state);

    const int line_count =
        CountWrappedLines(
            text_element,
            actual_width,
            cursor_width);

    const bool width_changed =
        std::fabs(
            actual_width -
            state->last_panel_width) > kGeometryEpsilon;

    const bool rows_changed =
        line_count != state->last_line_count;

    const bool control_changed =
        std::fabs(
            control_height -
            state->last_control_height) > kGeometryEpsilon;

    if (width_changed ||
        rows_changed ||
        control_changed)
    {
        const bool geometry_changed =
            ApplyMultilineLayout(
                *state,
                line_count,
                control_height,
                actual_width);

        state->layout_revision = state->revision;

        if (geometry_changed)
        {
            QueueLayoutReconcile(*state);
            return;
        }
    }
}

void UpdateInputOnUI(void* user_data)
{
    auto* state =
        static_cast<InputState*>(user_data);

    if (!state)
        return;

    state->update_queued = false;

    Rml::Element* document_element =
        state->document.get();

    if (!document_element ||
        !BindDocumentElements(
            *state,
            static_cast<Rml::ElementDocument*>(document_element)))
    {
        return;
    }

    const std::uint64_t current_revision =
        state->revision;

    const Rml::String value =
        state->input->GetValue();

    SetClass(
        state->bar,
        "has-text",
        HasVisibleText(value));

    if (state->reset_pending)
        ResetMultilineLayout(*state);

    if (value.empty())
    {
        if (state->first_line_shifted ||
            state->last_input_height >= 0.0f)
        {
            ResetMultilineLayout(*state);
        }

        state->layout_revision = current_revision;
        return;
    }

    Rml::ElementText* text_element =
        GetTextElement(state->input);

    if (!text_element)
    {
        state->layout_revision = current_revision;
        return;
    }

    const float control_height =
        state->wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
    {
        state->layout_revision = current_revision;
        return;
    }

    const float cursor_width =
        std::round(
            Rml::ElementUtilities::
                GetDensityIndependentPixelRatio(text_element));

    bool just_shifted = false;

    if (!state->first_line_shifted)
    {
        const float first_line_width =
            GetInitialTextWidth(
                *state,
                control_height);

        if (!IsFirstLineComplete(
                text_element,
                first_line_width,
                cursor_width))
        {
            state->layout_revision = current_revision;
            return;
        }

        state->first_line_shifted = true;
        just_shifted = true;

        SetClass(
            state->bar,
            "multiline",
            true);

        state->last_line_count = 0;

        state->last_input_height = -1.0f;
        state->last_bar_height = -1.0f;
        state->last_chat_bottom = -1.0f;
    }

    const float full_width =
        just_shifted
        ? GetMultilineTextWidth(
            *state,
            control_height)
        : GetActualTextWidth(*state);

    const int line_count =
        CountWrappedLines(
            text_element,
            full_width,
            cursor_width);

    const bool line_count_changed =
        line_count != state->last_line_count;

    const bool responsive_size_changed =
        std::fabs(
            control_height -
            state->last_control_height) > kGeometryEpsilon ||

        std::fabs(
            full_width -
            state->last_panel_width) > kGeometryEpsilon;

    if (just_shifted ||
        line_count_changed ||
        responsive_size_changed)
    {
        ApplyMultilineLayout(
            *state,
            line_count,
            control_height,
            full_width);
    }

    state->layout_revision = current_revision;

    QueueLayoutReconcile(*state);
}

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

void QueueLayoutReconcile(InputState& state)
{
    if (state.reconcile_queued)
        return;

    state.reconcile_queued = true;

    if (!Ultron_PostToUI(
            &ReconcileInputLayout,
            &state))
    {
        state.reconcile_queued = false;
    }
}

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

    const Rml::String& event_type =
        event->GetType();

    if (event_type == "click")
    {
        if (target->GetId() == kWingHitId ||
            target->GetId() == kInputId)
        {
            if (!BindElements(target))
                return false;
        }

        if (target->GetId() == kWingHitId)
        {
            InputState& state = State();

            const bool expand =
                !state.bar->IsClassSet("expanded");

            SetClass(
                state.bar,
                "expanded",
                expand);

            if (expand)
                state.input->Focus();
        }

        return false;
    }

    if (event_type == "change" &&
        target->GetId() == kInputId)
    {
        if (!BindElements(target))
            return false;

        InputState& state = State();

        const Rml::String value =
            state.input->GetValue();

        SetClass(
            state.bar,
            "has-text",
            HasVisibleText(value));

        ++state.revision;

        // Native RmlUi caret-follow owns scrolling.
        // Never restore or overwrite its scroll position here.
        if (value.empty())
        {
            state.first_line_shifted = false;
            state.reset_pending = true;
        }

        QueueInputUpdate(state);
        return false;
    }

    return false;
}

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

[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
