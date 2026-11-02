#include "ultron_api.h"

#include <cmath>

namespace
{

// =============================================================
// Gesture configuration
// =============================================================

constexpr float kEdgeStartRatio = 0.10f;
constexpr float kDrawerWidthRatio = 0.84f;

constexpr float kSwipeThreshold = 0.035f;
constexpr float kVerticalCancelThreshold = 0.045f;

// =============================================================
// Drawer state
// =============================================================

bool g_drawer_open = false;

// =============================================================
// Active touch gesture
// =============================================================

struct SwipeGesture
{
    bool active = false;
    bool drawer_was_open = false;
    bool committed = false;

    SDL_TouchID touch_id{};
    SDL_FingerID finger_id{};

    float start_x = 0.0f;
    float start_y = 0.0f;
};

SwipeGesture g_gesture;

// =============================================================
// Drawer visibility
// =============================================================

void SetDrawerOpen(bool open) noexcept
{
    if (g_drawer_open == open)
    {
        return;
    }

    g_drawer_open = open;

    (void)Ultron_SetElementClass(
        "drawer",
        "open",
        open);
}

// =============================================================
// Gesture reset
// =============================================================

void ResetGesture() noexcept
{
    g_gesture = {};
}

// =============================================================
// Gesture recognition
// =============================================================

void UpdateGesture(
    float x,
    float y) noexcept
{
    if (!g_gesture.active ||
        g_gesture.committed)
    {
        return;
    }

    const float delta_x =
        x - g_gesture.start_x;

    const float delta_y =
        y - g_gesture.start_y;

    const float abs_x = std::fabs(delta_x);
    const float abs_y = std::fabs(delta_y);

    // Cancel predominantly vertical movement.

    if (abs_y > kVerticalCancelThreshold &&
        abs_y > abs_x * 1.2f)
    {
        g_gesture.committed = true;
        return;
    }

    // Wait for sufficient horizontal movement.

    if (abs_x < kSwipeThreshold)
    {
        return;
    }

    // Closed: swipe from left to right.

    if (!g_gesture.drawer_was_open &&
        delta_x > 0.0f)
    {
        SetDrawerOpen(true);
    }

    // Open: swipe from right to left.

    else if (g_gesture.drawer_was_open &&
             delta_x < 0.0f)
    {
        SetDrawerOpen(false);
    }

    g_gesture.committed = true;
}

// =============================================================
// SDL3 event handler
// =============================================================

bool HandleDrawerGesture(
    const SDL_Event* event,
    void* user_data) noexcept
{
    (void)user_data;

    if (event == nullptr)
    {
        return false;
    }

    switch (event->type)
    {

        // -----------------------------------------------------
        // Finger down
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_DOWN:
        {
            if (g_gesture.active)
            {
                break;
            }

            const float x = event->tfinger.x;

            const bool valid_start =
                g_drawer_open
                    ? x <= kDrawerWidthRatio
                    : x <= kEdgeStartRatio;

            if (!valid_start)
            {
                break;
            }

            g_gesture.active = true;

            g_gesture.drawer_was_open =
                g_drawer_open;

            g_gesture.touch_id =
                event->tfinger.touchID;

            g_gesture.finger_id =
                event->tfinger.fingerID;

            g_gesture.start_x = x;
            g_gesture.start_y = event->tfinger.y;

            break;
        }

        // -----------------------------------------------------
        // Finger motion
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_MOTION:
        {
            if (!g_gesture.active)
            {
                break;
            }

            if (event->tfinger.touchID !=
                    g_gesture.touch_id ||
                event->tfinger.fingerID !=
                    g_gesture.finger_id)
            {
                break;
            }

            UpdateGesture(
                event->tfinger.x,
                event->tfinger.y);

            break;
        }

        // -----------------------------------------------------
        // Finger up
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_UP:
        {
            if (g_gesture.active &&
                event->tfinger.touchID ==
                    g_gesture.touch_id &&
                event->tfinger.fingerID ==
                    g_gesture.finger_id)
            {
                // Final position check in case the last motion
                // event was not delivered separately.

                UpdateGesture(
                    event->tfinger.x,
                    event->tfinger.y);

                ResetGesture();
            }

            break;
        }

        // -----------------------------------------------------
        // Finger cancelled
        // -----------------------------------------------------

        case SDL_EVENT_FINGER_CANCELED:
        {
            if (g_gesture.active &&
                event->tfinger.touchID ==
                    g_gesture.touch_id &&
                event->tfinger.fingerID ==
                    g_gesture.finger_id)
            {
                ResetGesture();
            }

            break;
        }

        // -----------------------------------------------------
        // Application lifecycle
        // -----------------------------------------------------

        case SDL_EVENT_WILL_ENTER_BACKGROUND:
        case SDL_EVENT_DID_ENTER_BACKGROUND:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
        case SDL_EVENT_TERMINATING:
        {
            ResetGesture();
            break;
        }

        default:
        {
            break;
        }
    }

    // Preserve normal SDL and RmlUi event handling.

    return false;
}

// =============================================================
// Automatic registration
// =============================================================

[[maybe_unused]]
const bool registered =
    Ultron_RegisterEventHandler(
        "ultron-edge-drawer",
        ULTRON_EVENT_ANY,
        &HandleDrawerGesture,
        nullptr,
        100);

} // namespace
