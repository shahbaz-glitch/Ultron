#include "ultron_api.h"

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/ElementUtilities.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/Log.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <string>
#include <utility>

namespace
{

constexpr char kModuleName[] = "ObsidianWingChat";
constexpr char kBarId[] = "input-bar";
constexpr char kWingHitId[] = "wing-hit";
constexpr char kScrollId[] = "input-scroll";
constexpr char kInputId[] = "message-input";
constexpr char kSendButtonId[] = "send-button";
constexpr char kChatId[] = "chat-area";

constexpr float kControlUnits = 14.22f;
constexpr float kFirstLeft = 18.63f;
constexpr float kFirstRight = 12.99f;
constexpr float kMultilineLeft = 4.17f;
constexpr float kMultilineRight = 2.45f;
constexpr float kExtraGap = 1.5f;
constexpr float kGeometryEpsilon = 0.5f;

// Gemini-style smooth scroll physics constants.
constexpr size_t kVelocitySampleCapacity = 8;
constexpr Uint64 kVelocityWindowMs = 100;
constexpr Uint64 kFlingStationaryCutoffMs = 65;
constexpr float kMomentumFriction = 3.2f;
constexpr float kAutoScrollSnappiness = 16.0f;

enum class TouchTarget
{
    None,
    InputScroll,
    ChatArea
};

enum class ScrollAnimMode
{
    None,
    Momentum,
    EaseToTarget
};

struct TouchSample
{
    Uint64 time_ms = 0;
    float y = 0.0f;
};


/* ---------------------------------------------------------
   INPUT & CHAT STATE
--------------------------------------------------------- */

struct InputState
{
    Rml::ElementDocument* document = nullptr;

    Rml::Element* bar = nullptr;
    Rml::Element* wing_hit = nullptr;
    Rml::Element* scroll = nullptr;
    Rml::Element* chat = nullptr;

    Rml::ElementFormControlTextArea* input = nullptr;

    bool word_break_configured = false;
    bool chat_area_configured = false;
    bool first_line_shifted = false;
    bool updating = false;

    int last_line_count = 0;
    int last_max_rows = 1;

    float last_control_height = 0.0f;
    float last_panel_width = 0.0f;
    float last_chat_width = 0.0f;

    float last_scroll_height = -1.0f;
    float last_textarea_height = -1.0f;
    float last_bar_height = -1.0f;
    float last_chat_bottom = -1.0f;

    // Isolated mobile touch scroll state for #input-scroll and #chat-area.
    TouchTarget touch_target = TouchTarget::None;
    bool touch_active = false;
    bool is_scrolling = false;
    bool touch_moved = false;
    bool caught_momentum = false;
    SDL_TouchID touch_id = 0;
    SDL_FingerID finger_id = 0;
    float touch_start_x = 0.0f;
    float touch_start_y = 0.0f;
    float touch_start_scroll_top = 0.0f;
    Uint64 last_motion_ms = 0;

    std::array<TouchSample, kVelocitySampleCapacity> velocity_samples{};
    size_t velocity_sample_count = 0;

    // VSync-locked smooth momentum & ease-to-bottom animation state.
    ScrollAnimMode anim_mode = ScrollAnimMode::None;
    TouchTarget anim_target = TouchTarget::None;
    float anim_scroll_pos = 0.0f;
    float anim_velocity = 0.0f;
    float anim_goal_scroll = 0.0f;
    Uint64 anim_last_ms = 0;
    bool anim_posted = false;

    Rml::String last_value;
};


InputState& State()
{
    static InputState state;
    return state;
}


void StopScrollAnimation(InputState& state)
{
    state.anim_mode = ScrollAnimMode::None;
    state.anim_target = TouchTarget::None;
    state.anim_velocity = 0.0f;
}


void ResetTouchScroll(InputState& state)
{
    state.touch_target = TouchTarget::None;
    state.touch_active = false;
    state.is_scrolling = false;
    state.touch_moved = false;
    state.caught_momentum = false;
    state.touch_id = 0;
    state.finger_id = 0;
    state.touch_start_x = 0.0f;
    state.touch_start_y = 0.0f;
    state.touch_start_scroll_top = 0.0f;
    state.last_motion_ms = 0;
    state.velocity_sample_count = 0;
}


void PushVelocitySample(
    InputState& state,
    Uint64 time_ms,
    float y)
{
    if (state.velocity_sample_count < kVelocitySampleCapacity)
    {
        state.velocity_samples[state.velocity_sample_count++] =
            TouchSample{time_ms, y};
        return;
    }

    for (size_t i = 1; i < kVelocitySampleCapacity; ++i)
    {
        state.velocity_samples[i - 1] = state.velocity_samples[i];
    }

    state.velocity_samples[kVelocitySampleCapacity - 1] =
        TouchSample{time_ms, y};
}


bool IsInsideElement(
    Rml::Element* element,
    const char* target_id)
{
    while (element != nullptr)
    {
        if (element->GetId() == target_id)
            return true;

        element = element->GetParentNode();
    }

    return false;
}


bool IsInsideInput(Rml::Element* element)
{
    return IsInsideElement(element, kInputId) ||
           IsInsideElement(element, kScrollId);
}


void ClearTextSelection(InputState& state)
{
    if (!state.input)
        return;

    int sel_start = 0;
    int sel_end = 0;
    state.input->GetSelection(&sel_start, &sel_end, nullptr);

    if (sel_start != sel_end)
    {
        state.input->SetSelectionRange(sel_end, sel_end);
    }
}


/* ---------------------------------------------------------
   ELEMENT BINDING
--------------------------------------------------------- */

void ConfigureChatArea(InputState& state);

bool BindElements(Rml::Element* source)
{
    if (!source)
        return false;

    Rml::ElementDocument* document =
        source->GetOwnerDocument();

    if (!document)
        return false;

    InputState& state = State();

    if (state.document != document)
    {
        state = InputState{};
        state.document = document;
    }

    state.bar = document->GetElementById(kBarId);
    state.wing_hit = document->GetElementById(kWingHitId);
    state.scroll = document->GetElementById(kScrollId);
    state.chat = document->GetElementById(kChatId);

    Rml::Element* input =
        document->GetElementById(kInputId);

    state.input =
        input && input->GetTagName() == "textarea"
        ? static_cast<Rml::ElementFormControlTextArea*>(input)
        : nullptr;

    if (!state.bar || !state.wing_hit ||
        !state.scroll || !state.chat || !state.input)
        return false;

    ConfigureChatArea(state);
    return true;
}


/* ---------------------------------------------------------
   INTERNAL TEXT ELEMENT
--------------------------------------------------------- */

Rml::ElementText* GetTextElement(
    Rml::Element* element)
{
    if (!element || element->GetNumChildren(true) < 1)
        return nullptr;

    Rml::Element* child = element->GetChild(0);

    if (!child || child->GetTagName() != "#text")
        return nullptr;

    return static_cast<Rml::ElementText*>(child);
}


/* ---------------------------------------------------------
   CHAT AREA CONFIGURATION

   Locks #chat-area width to 88% (matching left: 6%; right: 6%)
   and hides RmlUi's default desktop scrollbars so bubbles
   always have full horizontal width.
--------------------------------------------------------- */

void HideNativeScrollbars(Rml::Element* container)
{
    if (!container)
        return;

    const int total_children = container->GetNumChildren(true);

    for (int i = 0; i < total_children; ++i)
    {
        Rml::Element* child = container->GetChild(i);

        if (!child)
            continue;

        const Rml::String& tag = child->GetTagName();

        if (tag == "scrollbarvertical")
        {
            child->SetProperty("width", "0px");
            child->SetProperty("visibility", "hidden");
        }
        else if (tag == "scrollbarhorizontal")
        {
            child->SetProperty("height", "0px");
            child->SetProperty("visibility", "hidden");
        }
    }
}


void ConfigureChatArea(InputState& state)
{
    if (!state.chat)
        return;

    if (!state.chat_area_configured)
    {
        state.chat->SetProperty("width", "88%");
        state.chat->SetProperty("overflow-x", "hidden");
        state.chat_area_configured = true;
    }

    HideNativeScrollbars(state.chat);
}


/* ---------------------------------------------------------
   MULTILINE WORD BREAK
--------------------------------------------------------- */

void ConfigureMultilineWordBreak(InputState& state)
{
    if (state.word_break_configured || !state.input)
        return;

    if (!state.input->SetProperty(
            "word-break",
            "break-word"))
    {
        Rml::Log::Message(
            Rml::Log::LT_WARNING,
            "ObsidianWingChat: Failed to configure "
            "word-break: break-word.");
        return;
    }

    state.word_break_configured = true;
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


Rml::String TrimMessage(const Rml::String& value)
{
    size_t start = 0;

    while (start < value.size() &&
           std::isspace(static_cast<unsigned char>(value[start])) != 0)
    {
        ++start;
    }

    size_t end = value.size();

    while (end > start &&
           std::isspace(static_cast<unsigned char>(value[end - 1])) != 0)
    {
        --end;
    }

    return value.substr(start, end - start);
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


void SetClass(
    Rml::Element* element,
    const char* id,
    const char* class_name,
    bool enabled)
{
    if (element &&
        element->IsClassSet(class_name) != enabled)
    {
        Ultron_SetElementClass(
            id,
            class_name,
            enabled);
    }
}


Rml::String Pixels(float value)
{
    return std::to_string(
        std::max(value, 0.0f)) + "px";
}


float GetDpRatio(const InputState& state)
{
    if (state.document)
    {
        if (Rml::Context* context = state.document->GetContext())
        {
            return std::max(
                context->GetDensityIndependentPixelRatio(),
                1.0f);
        }
    }

    return 1.0f;
}


/* ---------------------------------------------------------
   HORIZONTAL CHAT BUBBLE SIZING

   Measures the message's natural horizontal width AND accounts
   for both padding + border frame so bubbles fit cleanly inside
   #chat-area right up to the right edge with zero clipping.
--------------------------------------------------------- */

float GetChatContentWidth(const InputState& state)
{
    if (!state.chat)
        return 0.0f;

    const float content_width =
        state.chat->GetBox().GetSize(Rml::BoxArea::Content).x;

    const float client_width =
        state.chat->GetClientWidth();

    float width = 0.0f;

    if (content_width > kGeometryEpsilon && client_width > kGeometryEpsilon)
    {
        width = std::min(content_width, client_width);
    }
    else if (content_width > kGeometryEpsilon)
    {
        width = content_width;
    }
    else if (client_width > kGeometryEpsilon)
    {
        width = client_width;
    }
    else if (state.document)
    {
        if (Rml::Context* context = state.document->GetContext())
        {
            width = static_cast<float>(context->GetDimensions().x) * 0.88f;
        }
    }

    return std::max(width, 0.0f);
}


void LayoutChatBubble(
    Rml::Element* bubble,
    float chat_content_width)
{
    if (!bubble || chat_content_width <= kGeometryEpsilon)
        return;

    Rml::ElementText* text_el = GetTextElement(bubble);

    if (!text_el)
        return;

    const Rml::String& text = text_el->GetText();
    float widest_line = 0.0f;
    size_t line_start = 0;

    while (line_start <= text.size())
    {
        const size_t newline_pos = text.find('\n', line_start);
        const size_t line_end =
            newline_pos == Rml::String::npos
            ? text.size()
            : newline_pos;

        if (line_end > line_start)
        {
            const Rml::StringView segment(
                text,
                line_start,
                line_end - line_start);

            const float seg_width = static_cast<float>(
                Rml::ElementUtilities::GetStringWidth(
                    text_el,
                    segment));

            if (seg_width > widest_line)
                widest_line = seg_width;
        }

        if (newline_pos == Rml::String::npos)
            break;

        line_start = newline_pos + 1;
    }

    const float dp_ratio = std::max(
        Rml::ElementUtilities::GetDensityIndependentPixelRatio(text_el),
        1.0f);

    // Include BOTH horizontal padding and horizontal border thickness.
    const Rml::Box& box = bubble->GetBox();
    const float horizontal_frame =
        box.GetFrameSize(Rml::BoxArea::Padding).x +
        box.GetFrameSize(Rml::BoxArea::Border).x;

    const float max_outer_width = chat_content_width * 0.88f;
    const float max_content_width =
        std::max(1.0f, max_outer_width - horizontal_frame);

    const float line_height = std::max(bubble->GetLineHeight(), 1.0f);
    const float min_content_width =
        std::min(max_content_width, line_height * 0.7f);

    const float safety_px = std::ceil(dp_ratio * 3.0f);
    const float target_content_width = std::ceil(std::clamp(
        widest_line + safety_px,
        min_content_width,
        max_content_width));

    const float bubble_outer_width =
        target_content_width + horizontal_frame;

    const float left_margin = std::floor(
        std::max(0.0f, chat_content_width - bubble_outer_width));

    bubble->SetProperty("box-sizing", "content-box");
    bubble->SetProperty("width", Pixels(target_content_width));
    bubble->SetProperty("max-width", Pixels(max_content_width));
    bubble->SetProperty("margin-left", Pixels(left_margin));
    bubble->SetProperty("margin-right", "0px");
}


void RelayoutAllChatBubbles(InputState& state)
{
    if (!state.chat || !state.document)
        return;

    ConfigureChatArea(state);

    const float chat_width = GetChatContentWidth(state);

    if (chat_width <= kGeometryEpsilon)
        return;

    const int child_count = state.chat->GetNumChildren(false);

    for (int i = 0; i < child_count; ++i)
    {
        Rml::Element* child = state.chat->GetChild(i);

        if (child && child->IsClassSet("chat-message"))
        {
            LayoutChatBubble(child, chat_width);
        }
    }

    state.last_chat_width = chat_width;
}


/* ---------------------------------------------------------
   FIRST-LINE SHIFT DETECTION
--------------------------------------------------------- */

bool IsFirstLineReadyToShift(
    Rml::ElementText* text_element,
    float available_width,
    float cursor_width)
{
    if (!text_element || available_width <= 0.0f)
        return false;

    const Rml::String& text =
        text_element->GetText();

    if (text.empty())
        return false;

    if (text.find('\n') != Rml::String::npos)
        return true;

    const Rml::StringView first_line(
        text,
        0,
        text.size());

    const float occupied_width =
        static_cast<float>(
            Rml::ElementUtilities::GetStringWidth(
                text_element,
                first_line));

    return occupied_width + cursor_width >= available_width;
}


/* ---------------------------------------------------------
   NATIVE WRAP DETECTION (single-line layout)
--------------------------------------------------------- */

bool HasNativeWrapped(InputState& state)
{
    const float line_height =
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return false;

    const float client_height =
        state.input->GetClientHeight();

    if (client_height < line_height * 0.9f)
        return false;

    return state.input->GetScrollHeight() -
               client_height >=
           line_height * 0.75f;
}


/* ---------------------------------------------------------
   RMLUI-NATIVE LINE COUNT
--------------------------------------------------------- */

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
    bool counted_trailing_empty = false;

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
            line_size +=
                static_cast<int>(trailing_spaces);

            if (space_end < text.size() &&
                text[space_end] == '\n')
            {
                ++line_size;
            }

            if (space_end == text.size())
                last_line = true;
        }

        if (line_size <= 0)
        {
            ++line_count;
            counted_trailing_empty = true;
            break;
        }

        line_begin += line_size;
        ++line_count;

    }
    while (!last_line);

    if (!counted_trailing_empty &&
        !text.empty() &&
        text.back() == '\n' &&
        line_begin >= static_cast<int>(text.size()))
    {
        ++line_count;
    }

    return std::max(line_count, 1);
}


/* ---------------------------------------------------------
   RESPONSIVE TEXT WIDTH
--------------------------------------------------------- */

float GetTextWidth(
    InputState& state,
    float control_height,
    float left_units,
    float right_units)
{
    const float scale =
        control_height / kControlUnits;

    const float horizontal_frame =
        state.input->GetBox()
            .GetFrameSize(Rml::BoxArea::Padding).x;

    return std::max(
        1.0f,
        state.bar->GetClientWidth() -
        (left_units + right_units) * scale -
        horizontal_frame);
}


/* ---------------------------------------------------------
   MANUAL SCROLL RANGES
--------------------------------------------------------- */

float GetMaxInputScroll(const InputState& state)
{
    if (!state.input || !state.scroll || !state.first_line_shifted)
        return 0.0f;

    const float line_height =
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return 0.0f;

    const int hidden_rows = std::max(
        0,
        state.last_line_count - state.last_max_rows);

    if (hidden_rows <= 0)
        return 0.0f;

    const float logical_max =
        static_cast<float>(hidden_rows) * line_height;

    const float native_max = std::max(
        0.0f,
        state.scroll->GetScrollHeight() -
        state.scroll->GetClientHeight());

    return std::min(logical_max, native_max);
}


float GetMaxChatScroll(const InputState& state)
{
    if (!state.chat)
        return 0.0f;

    return std::max(
        0.0f,
        state.chat->GetScrollHeight() -
        state.chat->GetClientHeight());
}


/* ---------------------------------------------------------
   VSYNC-LOCKED SMOOTH SCROLL ENGINE (MOMENTUM + AUTO-GLIDE)
--------------------------------------------------------- */

void StepScrollAnimationTask(void*);

void ScheduleScrollAnimation(InputState& state)
{
    if (state.anim_posted ||
        state.anim_mode == ScrollAnimMode::None)
    {
        return;
    }

    if (Ultron_PostToUI(&StepScrollAnimationTask, nullptr))
    {
        state.anim_posted = true;
    }
}


void StartMomentumScroll(
    InputState& state,
    TouchTarget target,
    float velocity_px_per_sec)
{
    if (target == TouchTarget::None)
        return;

    const float dp_ratio = GetDpRatio(state);
    const float min_fling_velocity = 110.0f * dp_ratio;

    if (std::fabs(velocity_px_per_sec) < min_fling_velocity)
    {
        StopScrollAnimation(state);
        return;
    }

    const float max_fling_velocity = 3600.0f * dp_ratio;
    const float clamped_velocity = std::clamp(
        velocity_px_per_sec,
        -max_fling_velocity,
        max_fling_velocity);

    const float current_scroll =
        target == TouchTarget::InputScroll
        ? (state.scroll ? state.scroll->GetScrollTop() : 0.0f)
        : (state.chat ? state.chat->GetScrollTop() : 0.0f);

    state.anim_mode = ScrollAnimMode::Momentum;
    state.anim_target = target;
    state.anim_scroll_pos = current_scroll;
    state.anim_velocity = clamped_velocity;
    state.anim_last_ms = SDL_GetTicks();

    ScheduleScrollAnimation(state);
}


void StartSmoothScrollTo(
    InputState& state,
    TouchTarget target,
    float goal_scroll)
{
    if (target == TouchTarget::None)
        return;

    const float current_scroll =
        target == TouchTarget::InputScroll
        ? (state.scroll ? state.scroll->GetScrollTop() : 0.0f)
        : (state.chat ? state.chat->GetScrollTop() : 0.0f);

    if (std::fabs(goal_scroll - current_scroll) <= kGeometryEpsilon)
    {
        StopScrollAnimation(state);

        if (target == TouchTarget::ChatArea && state.chat)
        {
            state.chat->SetScrollTop(goal_scroll);
        }
        else if (target == TouchTarget::InputScroll && state.scroll)
        {
            state.scroll->SetScrollTop(goal_scroll);
        }

        return;
    }

    state.anim_mode = ScrollAnimMode::EaseToTarget;
    state.anim_target = target;
    state.anim_scroll_pos = current_scroll;
    state.anim_goal_scroll = goal_scroll;
    state.anim_velocity = 0.0f;
    state.anim_last_ms = SDL_GetTicks();

    ScheduleScrollAnimation(state);
}


void StepScrollAnimation(InputState& state)
{
    state.anim_posted = false;

    if (state.anim_mode == ScrollAnimMode::None ||
        !state.document ||
        !state.document->IsVisible())
    {
        StopScrollAnimation(state);
        return;
    }

    Rml::Element* container =
        state.anim_target == TouchTarget::InputScroll
        ? state.scroll
        : state.chat;

    if (!container)
    {
        StopScrollAnimation(state);
        return;
    }

    const float max_scroll =
        state.anim_target == TouchTarget::InputScroll
        ? GetMaxInputScroll(state)
        : GetMaxChatScroll(state);

    if (max_scroll <= kGeometryEpsilon)
    {
        container->SetScrollTop(0.0f);
        StopScrollAnimation(state);
        return;
    }

    const Uint64 now = SDL_GetTicks();
    const Uint64 elapsed_ms =
        now >= state.anim_last_ms ? (now - state.anim_last_ms) : 0;

    if (elapsed_ms > 180)
    {
        StopScrollAnimation(state);
        return;
    }

    if (elapsed_ms == 0)
    {
        ScheduleScrollAnimation(state);
        return;
    }

    state.anim_last_ms = now;
    const float dt = std::min(
        static_cast<float>(elapsed_ms) * 0.001f,
        0.05f);

    if (state.anim_mode == ScrollAnimMode::Momentum)
    {
        // Frame-rate independent exponential deceleration.
        const float decay = std::exp(-kMomentumFriction * dt);
        const float delta =
            state.anim_velocity * (1.0f - decay) / kMomentumFriction;

        state.anim_scroll_pos += delta;
        state.anim_velocity *= decay;

        bool hit_edge = false;

        if (state.anim_scroll_pos <= 0.0f)
        {
            state.anim_scroll_pos = 0.0f;
            hit_edge = true;
        }
        else if (state.anim_scroll_pos >= max_scroll)
        {
            state.anim_scroll_pos = max_scroll;
            hit_edge = true;
        }

        container->SetScrollTop(state.anim_scroll_pos);

        if (state.anim_target == TouchTarget::InputScroll && state.input)
        {
            state.input->SetScrollTop(0.0f);
        }

        const float stop_threshold = 18.0f * GetDpRatio(state);

        if (hit_edge || std::fabs(state.anim_velocity) < stop_threshold)
        {
            StopScrollAnimation(state);
            return;
        }

        ScheduleScrollAnimation(state);
        return;
    }

    if (state.anim_mode == ScrollAnimMode::EaseToTarget)
    {
        const float goal = std::clamp(
            state.anim_goal_scroll,
            0.0f,
            max_scroll);

        const float alpha =
            1.0f - std::exp(-kAutoScrollSnappiness * dt);

        state.anim_scroll_pos +=
            (goal - state.anim_scroll_pos) * alpha;

        if (std::fabs(goal - state.anim_scroll_pos) <= kGeometryEpsilon)
        {
            container->SetScrollTop(goal);
            StopScrollAnimation(state);
            return;
        }

        container->SetScrollTop(state.anim_scroll_pos);
        ScheduleScrollAnimation(state);
    }
}


void StepScrollAnimationTask(void*)
{
    StepScrollAnimation(State());
}


/* ---------------------------------------------------------
   SCROLL SYNC (#input-scroll)

   Textarea itself stays pinned at (0, 0).
   Only the outer #input-scroll container scrolls 1 row at a time:
       4 lines -> 0 hidden rows
       5 lines -> 1 hidden row
       6 lines -> 2 hidden rows
--------------------------------------------------------- */

void SyncScroll(
    InputState& state,
    bool force_top,
    bool follow_end)
{
    if (!state.scroll || !state.input)
        return;

    if (state.anim_target == TouchTarget::InputScroll)
    {
        StopScrollAnimation(state);
    }

    // Keep textarea internal scroll pinned at top-left.
    state.input->SetScrollTop(0.0f);
    state.input->SetScrollLeft(0.0f);

    if (force_top)
    {
        state.scroll->SetScrollTop(0.0f);
        return;
    }

    const float line_height =
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return;

    const int hidden_rows = std::max(
        0,
        state.last_line_count - state.last_max_rows);

    const float current_top =
        state.scroll->GetScrollTop();

    if (hidden_rows == 0)
    {
        if (current_top > kGeometryEpsilon)
            state.scroll->SetScrollTop(0.0f);

        return;
    }

    const float target =
        static_cast<float>(hidden_rows) * line_height;

    if (follow_end)
    {
        const float max_scroll = std::max(
            0.0f,
            state.scroll->GetScrollHeight() -
            state.scroll->GetClientHeight());

        if (max_scroll + kGeometryEpsilon < target &&
            state.document)
        {
            state.document->UpdateDocument();
        }

        if (std::fabs(current_top - target) > kGeometryEpsilon)
        {
            state.scroll->SetScrollTop(target);
        }
    }
    else if (current_top > target + kGeometryEpsilon)
    {
        state.scroll->SetScrollTop(target);
    }
}


/* ---------------------------------------------------------
   RESET MULTILINE
--------------------------------------------------------- */

void ResetMultiline(InputState& state)
{
    if (state.touch_target == TouchTarget::InputScroll)
    {
        ResetTouchScroll(state);
    }

    if (state.anim_target == TouchTarget::InputScroll)
    {
        StopScrollAnimation(state);
    }

    state.first_line_shifted = false;

    state.last_line_count = 0;
    state.last_max_rows = 1;

    state.last_control_height = 0.0f;
    state.last_panel_width = 0.0f;

    state.last_scroll_height = -1.0f;
    state.last_textarea_height = -1.0f;
    state.last_bar_height = -1.0f;
    state.last_chat_bottom = -1.0f;

    state.last_value.clear();

    if (state.word_break_configured)
    {
        state.input->SetProperty(
            "word-break",
            "break-word");
    }

    state.word_break_configured = false;

    SetClass(
        state.bar,
        kBarId,
        "multiline",
        false);

    state.scroll->RemoveProperty("width");
    state.scroll->RemoveProperty("height");

    state.input->RemoveProperty("width");
    state.input->RemoveProperty("height");

    state.bar->RemoveProperty("height");
    state.chat->RemoveProperty("bottom");

    state.scroll->SetScrollTop(0.0f);
    state.scroll->SetScrollLeft(0.0f);

    state.input->SetScrollTop(0.0f);
    state.input->SetScrollLeft(0.0f);
}


/* ---------------------------------------------------------
   SEND CURRENT MESSAGE (#send-button arrow)
--------------------------------------------------------- */

void SendCurrentMessage(InputState& state)
{
    if (!state.document ||
        !state.bar ||
        !state.chat ||
        !state.input)
    {
        return;
    }

    if (!state.bar->IsClassSet("expanded"))
        return;

    const Rml::String message =
        TrimMessage(state.input->GetValue());

    if (message.empty())
        return;

    ConfigureChatArea(state);

    Rml::ElementPtr bubble =
        state.document->CreateElement("div");

    Rml::Element* appended_bubble = nullptr;

    if (bubble)
    {
        bubble->SetClass("chat-message", true);
        bubble->SetProperty("word-break", "break-word");

        if (Rml::ElementPtr text_node =
                state.document->CreateTextNode(message))
        {
            bubble->AppendChild(std::move(text_node));
        }

        appended_bubble =
            state.chat->AppendChild(std::move(bubble));
    }

    state.input->SetValue("");
    ResetMultiline(state);
    SetClass(state.bar, kBarId, "has-text", false);

    if (SDL_Window* window = SDL_GetKeyboardFocus())
    {
        SDL_ClearComposition(window);
    }

    // First layout pass resolves font size, padding, and chat-area width.
    state.document->UpdateDocument();
    HideNativeScrollbars(state.chat);

    const float chat_width = GetChatContentWidth(state);

    if (std::fabs(chat_width - state.last_chat_width) > kGeometryEpsilon)
    {
        RelayoutAllChatBubbles(state);
    }
    else if (appended_bubble != nullptr)
    {
        LayoutChatBubble(appended_bubble, chat_width);
    }

    // Second layout pass locks in exact horizontal bubble heights & scrollHeight.
    state.document->UpdateDocument();
    HideNativeScrollbars(state.chat);

    StartSmoothScrollTo(
        state,
        TouchTarget::ChatArea,
        GetMaxChatScroll(state));
}


/* ---------------------------------------------------------
   APPLY MULTILINE GEOMETRY

   #input-scroll caps at maximum_rows (4).
   #message-input always grows to (line_count + 1) rows once
   at/above maximum_rows so it NEVER overflows internally.
--------------------------------------------------------- */

void ApplyMultilineLayout(
    InputState& state,
    int line_count,
    float control_height,
    float full_text_width)
{
    const float padding_height =
        control_height * (1.23f / kControlUnits);

    const float maximum_text_height =
        control_height * (30.15f / kControlUnits);

    const float extra_gap =
        control_height * (kExtraGap / kControlUnits);

    const float line_height =
        state.input->GetLineHeight();

    if (line_height <= 0.0f)
        return;

    const int maximum_rows = std::max(
        1,
        static_cast<int>(
            std::floor(
                (maximum_text_height - padding_height) /
                    line_height +
                0.05f)));

    const int clamped_lines = std::max(line_count, 1);

    const int visible_rows = std::min(
        clamped_lines,
        maximum_rows);

    const int textarea_rows =
        clamped_lines >= maximum_rows
        ? clamped_lines + 1
        : visible_rows;

    const float scroll_box_height =
        static_cast<float>(visible_rows) * line_height;

    const float textarea_height =
        static_cast<float>(textarea_rows) * line_height;

    const float text_height =
        scroll_box_height + padding_height;

    const float bar_height =
        text_height +
        control_height +
        extra_gap;

    const float chat_bottom =
        bar_height +
        control_height * (11.52f / kControlUnits);

    const bool width_changed =
        std::fabs(
            full_text_width -
            state.last_panel_width) > kGeometryEpsilon;

    const bool scroll_height_changed =
        std::fabs(
            scroll_box_height -
            state.last_scroll_height) > kGeometryEpsilon;

    const bool textarea_height_changed =
        std::fabs(
            textarea_height -
            state.last_textarea_height) > kGeometryEpsilon;

    const bool bar_height_changed =
        std::fabs(
            bar_height -
            state.last_bar_height) > kGeometryEpsilon;

    const bool chat_bottom_changed =
        std::fabs(
            chat_bottom -
            state.last_chat_bottom) > kGeometryEpsilon;

    if (width_changed)
    {
        state.scroll->SetProperty(
            "width",
            Pixels(full_text_width));

        state.input->SetProperty(
            "width",
            Pixels(full_text_width));

        state.last_panel_width = full_text_width;
    }

    if (scroll_height_changed)
    {
        state.scroll->SetProperty(
            "height",
            Pixels(scroll_box_height));

        state.last_scroll_height = scroll_box_height;
    }

    if (textarea_height_changed)
    {
        state.input->SetProperty(
            "height",
            Pixels(textarea_height));

        state.last_textarea_height = textarea_height;
    }

    if (bar_height_changed)
    {
        state.bar->SetProperty(
            "height",
            Pixels(bar_height));

        state.last_bar_height = bar_height;
    }

    if (chat_bottom_changed)
    {
        state.chat->SetProperty(
            "bottom",
            Pixels(chat_bottom));

        state.last_chat_bottom = chat_bottom;
    }

    state.last_line_count = clamped_lines;
    state.last_max_rows = maximum_rows;
    state.last_control_height = control_height;
}


/* ---------------------------------------------------------
   INPUT UPDATE
--------------------------------------------------------- */

void UpdateInput(InputState& state)
{
    if (state.updating ||
        !state.input ||
        !state.scroll ||
        !state.bar ||
        !state.wing_hit ||
        !state.chat)
    {
        return;
    }

    struct UpdateGuard
    {
        bool& flag;

        explicit UpdateGuard(bool& value)
            : flag(value)
        {
            flag = true;
        }

        ~UpdateGuard()
        {
            flag = false;
        }
    };

    UpdateGuard guard(state.updating);

    const Rml::String value =
        state.input->GetValue();

    SetClass(
        state.bar,
        kBarId,
        "has-text",
        HasVisibleText(value));

    if (value.empty())
    {
        ResetMultiline(state);
        return;
    }

    Rml::ElementText* text_element =
        GetTextElement(state.input);

    if (!text_element)
        return;

    const float control_height =
        state.wing_hit->GetOffsetHeight();

    if (control_height <= 0.0f)
        return;

    const float cursor_width =
        std::round(
            Rml::ElementUtilities::
                GetDensityIndependentPixelRatio(text_element));

    bool just_shifted = false;

    /* FIRST LINE */

    if (!state.first_line_shifted)
    {
        const float first_line_width =
            GetTextWidth(
                state,
                control_height,
                kFirstLeft,
                kFirstRight);

        const bool reached_width =
            IsFirstLineReadyToShift(
                text_element,
                first_line_width,
                cursor_width);

        const bool already_wrapped =
            !reached_width && HasNativeWrapped(state);

        if (!reached_width && !already_wrapped)
        {
            state.last_value = value;
            return;
        }

        state.first_line_shifted = true;
        just_shifted = true;

        SetClass(
            state.bar,
            kBarId,
            "multiline",
            true);

        ConfigureMultilineWordBreak(state);
    }

    /* FULL-WIDTH MULTILINE */

    const float full_width =
        GetTextWidth(
            state,
            control_height,
            kMultilineLeft,
            kMultilineRight);

    const bool responsive_size_changed =
        std::fabs(
            control_height -
            state.last_control_height) > kGeometryEpsilon ||

        std::fabs(
            full_width -
            state.last_panel_width) > kGeometryEpsilon;

    const int corrected_line_count =
        CountWrappedLines(
            text_element,
            full_width,
            cursor_width);

    const bool append_only =
        IsAppendOnly(state.last_value, value);

    int final_line_count = corrected_line_count;

    if (append_only && !responsive_size_changed)
    {
        final_line_count = std::max(
            final_line_count,
            state.last_line_count);
    }

    const bool line_count_changed =
        final_line_count != state.last_line_count;

    if (just_shifted ||
        line_count_changed ||
        responsive_size_changed)
    {
        ApplyMultilineLayout(
            state,
            final_line_count,
            control_height,
            full_width);
    }

    SyncScroll(
        state,
        just_shifted && final_line_count == 1,
        append_only);

    state.last_value = value;
}


/* ---------------------------------------------------------
   FLING VELOCITY COMPUTATION
--------------------------------------------------------- */

float ComputeFlingVelocity(
    const InputState& state,
    Uint64 release_ms)
{
    if (state.velocity_sample_count < 2)
        return 0.0f;

    // If finger paused before lifting, stop cleanly without drifting.
    if (release_ms > state.last_motion_ms &&
        release_ms - state.last_motion_ms > kFlingStationaryCutoffMs)
    {
        return 0.0f;
    }

    const TouchSample& newest =
        state.velocity_samples[state.velocity_sample_count - 1];

    const TouchSample* oldest = &newest;

    for (size_t i = state.velocity_sample_count; i-- > 0;)
    {
        const TouchSample& sample = state.velocity_samples[i];

        if (newest.time_ms >= sample.time_ms &&
            newest.time_ms - sample.time_ms <= kVelocityWindowMs)
        {
            oldest = &sample;
        }
        else
        {
            break;
        }
    }

    if (newest.time_ms <= oldest->time_ms)
        return 0.0f;

    const float dt_sec =
        static_cast<float>(newest.time_ms - oldest->time_ms) * 0.001f;

    if (dt_sec < 0.004f)
        return 0.0f;

    const float dy = newest.y - oldest->y;
    return -dy / dt_sec;
}


/* ---------------------------------------------------------
   MOBILE TOUCH HANDLER (#input-scroll + #chat-area)

   Isolates touch dragging from RmlUi's desktop mouse-drag
   selection, drives 1:1 finger scrolling while touching,
   and launches VSync-locked momentum glide on release.
--------------------------------------------------------- */

bool HandleTouchScrollEvent(
    const SDL_Event* event,
    void*)
{
    if (!event)
        return false;

    InputState& state = State();

    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND)
    {
        StopScrollAnimation(state);
        ResetTouchScroll(state);
        return false;
    }

    if (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
    {
        if (state.document && state.document->IsVisible() && state.chat)
        {
            state.document->UpdateDocument();
            RelayoutAllChatBubbles(state);
        }

        return false;
    }

    if (event->type == SDL_EVENT_FINGER_CANCELED)
    {
        if (state.touch_active &&
            event->tfinger.touchID == state.touch_id &&
            event->tfinger.fingerID == state.finger_id)
        {
            ResetTouchScroll(state);
            return true;
        }

        return false;
    }

    if (!state.document ||
        !state.document->IsVisible() ||
        !state.bar ||
        !state.scroll ||
        !state.chat ||
        !state.input)
    {
        return false;
    }

    Rml::Context* context = state.document->GetContext();

    if (!context)
        return false;

    const Rml::Vector2i dims = context->GetDimensions();

    if (dims.x <= 0 || dims.y <= 0)
        return false;

    const float x =
        event->tfinger.x * static_cast<float>(dims.x);

    const float y =
        event->tfinger.y * static_cast<float>(dims.y);

    const auto is_point_inside =
        [x, y](Rml::Element* element)
    {
        if (!element)
            return false;

        const Rml::Vector2f box_pos =
            element->GetAbsoluteOffset(Rml::BoxArea::Border);

        const Rml::Vector2f box_size =
            element->GetBox().GetSize(Rml::BoxArea::Border);

        return x >= box_pos.x &&
               x <= box_pos.x + box_size.x &&
               y >= box_pos.y &&
               y <= box_pos.y + box_size.y;
    };

    if (event->type == SDL_EVENT_FINGER_DOWN)
    {
        if (state.touch_active)
            return false;

        TouchTarget target = TouchTarget::None;
        float start_scroll_top = 0.0f;

        if (state.bar->IsClassSet("expanded") &&
            is_point_inside(state.scroll))
        {
            target = TouchTarget::InputScroll;
            start_scroll_top = state.scroll->GetScrollTop();
        }
        else if (is_point_inside(state.chat))
        {
            target = TouchTarget::ChatArea;
            start_scroll_top = state.chat->GetScrollTop();
        }

        if (target == TouchTarget::None)
            return false;

        const bool was_gliding =
            state.anim_mode == ScrollAnimMode::Momentum &&
            state.anim_target == target &&
            std::fabs(state.anim_velocity) > 80.0f * GetDpRatio(state);

        if (state.anim_target == target)
        {
            StopScrollAnimation(state);
        }

        const Uint64 now = SDL_GetTicks();

        state.touch_target = target;
        state.touch_active = true;
        state.is_scrolling = false;
        state.touch_moved = false;
        state.caught_momentum = was_gliding;
        state.touch_id = event->tfinger.touchID;
        state.finger_id = event->tfinger.fingerID;
        state.touch_start_x = x;
        state.touch_start_y = y;
        state.touch_start_scroll_top = start_scroll_top;
        state.last_motion_ms = now;
        state.velocity_sample_count = 0;

        PushVelocitySample(state, now, y);
        return true;
    }

    if (!state.touch_active ||
        event->tfinger.touchID != state.touch_id ||
        event->tfinger.fingerID != state.finger_id)
    {
        return false;
    }

    if (event->type == SDL_EVENT_FINGER_MOTION)
    {
        const Uint64 now = SDL_GetTicks();
        state.last_motion_ms = now;
        PushVelocitySample(state, now, y);

        const float control_height =
            state.wing_hit ? state.wing_hit->GetOffsetHeight() : 0.0f;

        const float drag_threshold = std::max(
            10.0f,
            control_height * 0.11f);

        const float dx = x - state.touch_start_x;
        const float dy = y - state.touch_start_y;

        const float max_scroll =
            state.touch_target == TouchTarget::InputScroll
            ? GetMaxInputScroll(state)
            : GetMaxChatScroll(state);

        if (!state.is_scrolling)
        {
            if (std::fabs(dx) >= drag_threshold ||
                std::fabs(dy) >= drag_threshold)
            {
                state.touch_moved = true;
            }

            if (max_scroll > kGeometryEpsilon &&
                std::fabs(dy) >= drag_threshold)
            {
                state.is_scrolling = true;
                state.touch_start_y = y;
                state.touch_start_scroll_top =
                    state.touch_target == TouchTarget::InputScroll
                    ? state.scroll->GetScrollTop()
                    : state.chat->GetScrollTop();
            }

            return true;
        }

        if (max_scroll <= kGeometryEpsilon)
            return true;

        const float delta_y = y - state.touch_start_y;
        const float target_scroll = std::clamp(
            state.touch_start_scroll_top - delta_y,
            0.0f,
            max_scroll);

        if (state.touch_target == TouchTarget::InputScroll)
        {
            state.scroll->SetScrollTop(target_scroll);
            state.input->SetScrollTop(0.0f);
        }
        else if (state.touch_target == TouchTarget::ChatArea)
        {
            state.chat->SetScrollTop(target_scroll);
        }

        return true;
    }

    if (event->type == SDL_EVENT_FINGER_UP)
    {
        const Uint64 now = SDL_GetTicks();
        const TouchTarget target = state.touch_target;
        const bool was_scrolling = state.is_scrolling;
        const bool was_moved = state.touch_moved;
        const bool was_catching = state.caught_momentum;
        const float tap_x = state.touch_start_x;
        const float tap_y = state.touch_start_y;
        const float fling_velocity =
            was_scrolling ? ComputeFlingVelocity(state, now) : 0.0f;

        ResetTouchScroll(state);

        if (was_scrolling)
        {
            StartMomentumScroll(state, target, fling_velocity);
        }
        else if (!was_moved && !was_catching)
        {
            // Forward clean taps only (never drags or momentum catches).
            const int px = static_cast<int>(std::round(tap_x));
            const int py = static_cast<int>(std::round(tap_y));

            context->ProcessMouseMove(px, py, 0);
            context->ProcessMouseButtonDown(0, 0);
            context->ProcessMouseButtonUp(0, 0);

            if (target == TouchTarget::InputScroll)
            {
                if (context->GetFocusElement() != state.input)
                {
                    state.input->Focus();
                }

                ClearTextSelection(state);
            }
        }

        return true;
    }

    return false;
}


/* ---------------------------------------------------------
   SINGLE RML EVENT HANDLER
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

    const Rml::String& type = event->GetType();

    /* BLOCK DOUBLE-TAP WORD SELECTION INSIDE INPUT & CHAT */

    if (type == "dblclick" &&
        (IsInsideInput(target) || IsInsideElement(target, kChatId)))
    {
        ClearTextSelection(State());
        return true;
    }

    /* WING CLICK */

    if (type == "click" &&
        IsInsideElement(target, kWingHitId))
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
        {
            state.input->Focus();
        }
        else
        {
            if (state.touch_target == TouchTarget::InputScroll)
                ResetTouchScroll(state);

            if (state.anim_target == TouchTarget::InputScroll)
                StopScrollAnimation(state);
        }

        return false;
    }

    /* SEND BUTTON (ARROW) CLICK */

    if (type == "click" &&
        IsInsideElement(target, kSendButtonId))
    {
        if (!BindElements(target))
            return false;

        SendCurrentMessage(State());
        return true;
    }

    /* TEXTAREA CHANGE */

    if (type == "change" &&
        target->GetId() == kInputId)
    {
        if (!BindElements(target))
            return false;

        UpdateInput(State());
        return false;
    }

    /* ANIMATED RESIZE FINISHED */

    if (type == "transitionend")
    {
        const Rml::String& id = target->GetId();

        if (id == kInputId ||
            id == kScrollId ||
            id == kBarId ||
            id == kChatId)
        {
            if (BindElements(target))
            {
                InputState& state = State();

                if (state.first_line_shifted &&
                    !state.updating)
                {
                    if (id == kBarId)
                    {
                        UpdateInput(state);
                    }
                    else
                    {
                        SyncScroll(state, false, false);
                    }
                }
            }
        }

        return false;
    }

    return false;
}


/* ---------------------------------------------------------
   AUTOMATIC REGISTRATION
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

    (void)Ultron_RegisterRmlEventHandler(
        kModuleName,
        "transitionend",
        &HandleRmlEvent);

    (void)Ultron_RegisterRmlEventHandler(
        kModuleName,
        "dblclick",
        &HandleRmlEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_FINGER_DOWN,
        &HandleTouchScrollEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_FINGER_MOTION,
        &HandleTouchScrollEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_FINGER_UP,
        &HandleTouchScrollEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_FINGER_CANCELED,
        &HandleTouchScrollEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_WILL_ENTER_BACKGROUND,
        &HandleTouchScrollEvent);

    (void)Ultron_RegisterEventHandler(
        kModuleName,
        SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED,
        &HandleTouchScrollEvent);

    return click_registered && change_registered;
}


[[maybe_unused]]
const bool g_registered =
    RegisterObsidianWingChat();

} // namespace
