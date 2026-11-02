
#include "ultron_api.h"

#include <SDL3/SDL.h>

namespace
{
    constexpr float BUTTON_LEFT   = 0.20f;
    constexpr float BUTTON_RIGHT  = 0.80f;
    constexpr float BUTTON_TOP    = 0.82f;
    constexpr float BUTTON_BOTTOM = 0.91f;

    constexpr Uint64 DOUBLE_TAP_MS = 450;

    bool g_circle_visible = false;
    Uint64 g_last_tap = 0;


    bool IsButtonArea(float x, float y) noexcept
    {
        return
            x >= BUTTON_LEFT &&
            x <= BUTTON_RIGHT &&
            y >= BUTTON_TOP &&
            y <= BUTTON_BOTTOM;
    }


    bool SetCircleVisible(bool visible)
    {
        const bool updated = Ultron_SetElementProperty(
            "red-circle",
            "display",
            visible ? "block" : "none"
        );

        if (updated)
        {
            g_circle_visible = visible;
        }
        else
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: unable to update red-circle."
            );
        }

        return updated;
    }


    void HandleButtonTap(float x, float y)
    {
        if (!IsButtonArea(x, y))
        {
            return;
        }

        const Uint64 now = SDL_GetTicks();

        // A single tap hides an already visible circle.
        if (g_circle_visible)
        {
            SetCircleVisible(false);
            g_last_tap = 0;
            return;
        }

        // A second tap within the time limit shows the circle.
        if (g_last_tap != 0 &&
            now - g_last_tap <= DOUBLE_TAP_MS)
        {
            g_last_tap = 0;
            SetCircleVisible(true);
            return;
        }

        // Remember the first tap.
        g_last_tap = now;
    }


    bool OnFingerDown(
        const SDL_Event* event,
        void* user_data)
    {
        (void)user_data;

        if (event == nullptr ||
            event->type != SDL_EVENT_FINGER_DOWN)
        {
            return false;
        }

        HandleButtonTap(
            event->tfinger.x,
            event->tfinger.y
        );

        // Allow normal RmlUi touch processing too.
        return false;
    }


    bool OnMouseButtonDown(
        const SDL_Event* event,
        void* user_data)
    {
        (void)user_data;

        if (event == nullptr ||
            event->type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event->button.button != SDL_BUTTON_LEFT)
        {
            return false;
        }

        SDL_Window* window =
            SDL_GetWindowFromID(event->button.windowID);

        if (window == nullptr)
        {
            return false;
        }

        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSize(window, &width, &height) ||
            width <= 0 || height <= 0)
        {
            return false;
        }

        HandleButtonTap(
            event->button.x / static_cast<float>(width),
            event->button.y / static_cast<float>(height)
        );

        return false;
    }


    // Register handlers before the Ultron UI starts.
    [[maybe_unused]] const bool registered = []
    {
        const bool touch_registered =
            Ultron_RegisterEventHandler(
                "home-circle",
                SDL_EVENT_FINGER_DOWN,
                &OnFingerDown
            );

        const bool mouse_registered =
            Ultron_RegisterEventHandler(
                "home-circle",
                SDL_EVENT_MOUSE_BUTTON_DOWN,
                &OnMouseButtonDown
            );

        return touch_registered && mouse_registered;
    }();

}
