
#include "ultron_api.h"

#include <SDL3/SDL.h>

namespace
{
    // Generous touch area around the visible button.
    constexpr float BUTTON_LEFT   = 0.10f;
    constexpr float BUTTON_RIGHT  = 0.90f;
    constexpr float BUTTON_TOP    = 0.65f;
    constexpr float BUTTON_BOTTOM = 0.98f;

    constexpr Uint64 DOUBLE_TAP_MS = 700;

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


    void HandleButtonTap(float x, float y)
    {
        if (!IsButtonArea(x, y))
            return;

        const Uint64 now = SDL_GetTicks();

        // Circle is visible: one tap hides it.
        if (g_circle_visible)
        {
            if (Ultron_SetElementProperty(
                    "red-circle", "display", "none"))
            {
                g_circle_visible = false;

                Ultron_SetElementText(
                    "toggle-button", "DOUBLE TAP");
            }

            g_last_tap = 0;
            return;
        }

        // Second tap within 700 ms: show the circle.
        if (g_last_tap != 0 &&
            now - g_last_tap <= DOUBLE_TAP_MS)
        {
            g_last_tap = 0;

            if (Ultron_SetElementProperty(
                    "red-circle", "display", "block"))
            {
                g_circle_visible = true;

                Ultron_SetElementText(
                    "toggle-button", "HIDE CIRCLE");
            }

            return;
        }

        // First tap: wait for the second tap.
        g_last_tap = now;

        Ultron_SetElementText(
            "toggle-button", "TAP AGAIN");
    }


    bool OnFingerDown(
        const SDL_Event* event,
        void* user_data)
    {
        (void)user_data;

        if (!event ||
            event->type != SDL_EVENT_FINGER_DOWN)
        {
            return false;
        }

        HandleButtonTap(
            event->tfinger.x,
            event->tfinger.y);

        return false;
    }


    bool OnMouseButtonDown(
        const SDL_Event* event,
        void* user_data)
    {
        (void)user_data;

        if (!event ||
            event->type != SDL_EVENT_MOUSE_BUTTON_DOWN ||
            event->button.button != SDL_BUTTON_LEFT)
        {
            return false;
        }

        SDL_Window* window =
            SDL_GetWindowFromID(event->button.windowID);

        if (!window)
            return false;

        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSize(window, &width, &height) ||
            width <= 0 || height <= 0)
        {
            return false;
        }

        HandleButtonTap(
            event->button.x / static_cast<float>(width),
            event->button.y / static_cast<float>(height));

        return false;
    }


    [[maybe_unused]] const bool registered = []
    {
        const bool touch = Ultron_RegisterEventHandler(
            "home-circle",
            SDL_EVENT_FINGER_DOWN,
            &OnFingerDown);

        const bool mouse = Ultron_RegisterEventHandler(
            "home-circle",
            SDL_EVENT_MOUSE_BUTTON_DOWN,
            &OnMouseButtonDown);

        return touch && mouse;
    }();
}
