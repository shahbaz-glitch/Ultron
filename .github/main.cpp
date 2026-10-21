#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <GLES3/gl3.h>

#include <atomic>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/ElementDocument.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

namespace
{
    std::atomic_bool g_running{true};
    std::atomic_bool g_paused{false};

    bool HandleAppEvents(
        void* userdata,
        SDL_Event* event)
    {
        (void)userdata;

        switch (event->type)
        {
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
                g_paused.store(
                    true,
                    std::memory_order_relaxed);
                break;

            case SDL_EVENT_DID_ENTER_BACKGROUND:
                g_paused.store(
                    true,
                    std::memory_order_relaxed);
                break;

            case SDL_EVENT_WILL_ENTER_FOREGROUND:
                g_paused.store(
                    true,
                    std::memory_order_relaxed);
                break;

            case SDL_EVENT_DID_ENTER_FOREGROUND:
                g_paused.store(
                    false,
                    std::memory_order_relaxed);
                break;

            case SDL_EVENT_LOW_MEMORY:
                SDL_Log(
                    "Android low-memory notification received.");
                break;

            case SDL_EVENT_TERMINATING:
                g_running.store(
                    false,
                    std::memory_order_relaxed);
                break;

            default:
                break;
        }

        return true;
    }

    int Fail(
        SDL_Window* window,
        SDL_GLContext gl_context,
        int code,
        const char* message)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron startup failure: %s",
            message);

        const char* error = SDL_GetError();

        if (error != nullptr &&
            error[0] != '\0')
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "SDL error: %s",
                error);
        }

        if (gl_context != nullptr)
        {
            SDL_GL_DestroyContext(
                gl_context);
        }

        if (window != nullptr)
        {
            SDL_DestroyWindow(
                window);
        }

        SDL_Quit();

        return code;
    }
}

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    constexpr int initial_width = 1280;
    constexpr int initial_height = 720;

    SDL_SetLogPriority(
        SDL_LOG_CATEGORY_APPLICATION,
        SDL_LOG_PRIORITY_INFO);

    SDL_Log(
        "Ultron starting...");

    if (!SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_EVENTS))
    {
        return Fail(
            nullptr,
            nullptr,
            1,
            "SDL_Init failed");
    }

    SDL_SetEventFilter(
        HandleAppEvents,
        nullptr);

    SDL_SetHint(
        SDL_HINT_IME_IMPLEMENTED_UI,
        "composition");

    SDL_SetHint(
        SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH,
        "1");

    SDL_SetHint(
        SDL_HINT_TOUCH_MOUSE_EVENTS,
        "0");

    // OpenGL ES 3.2
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_FLAGS,
        0);

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_ES);

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MAJOR_VERSION,
        3);

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_MINOR_VERSION,
        2);

    SDL_GL_SetAttribute(
        SDL_GL_DOUBLEBUFFER,
        1);

    // Window
    SDL_PropertiesID properties =
        SDL_CreateProperties();

    if (properties == 0)
    {
        return Fail(
            nullptr,
            nullptr,
            2,
            "SDL_CreateProperties failed");
    }

    SDL_SetStringProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_TITLE_STRING,
        "Ultron");

    SDL_SetNumberProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_X_NUMBER,
        SDL_WINDOWPOS_CENTERED);

    SDL_SetNumberProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_Y_NUMBER,
        SDL_WINDOWPOS_CENTERED);

    SDL_SetNumberProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER,
        initial_width);

    SDL_SetNumberProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER,
        initial_height);

    SDL_SetBooleanProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_OPENGL_BOOLEAN,
        true);

    SDL_SetBooleanProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN,
        true);

    SDL_SetBooleanProperty(
        properties,
        SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN,
        true);

    SDL_Window* window =
        SDL_CreateWindowWithProperties(
            properties);

    SDL_DestroyProperties(
        properties);

    if (window == nullptr)
    {
        return Fail(
            nullptr,
            nullptr,
            3,
            "SDL_CreateWindowWithProperties failed");
    }

    // OpenGL context
    SDL_GLContext gl_context =
        SDL_GL_CreateContext(
            window);

    if (gl_context == nullptr)
    {
        return Fail(
            window,
            nullptr,
            4,
            "SDL_GL_CreateContext failed");
    }

    if (!SDL_GL_MakeCurrent(
            window,
            gl_context))
    {
        return Fail(
            window,
            gl_context,
            5,
            "SDL_GL_MakeCurrent failed");
    }

    SDL_GL_SetSwapInterval(1);

    // Initial pure-black frame
    glDisable(GL_SCISSOR_TEST);

    glClearColor(
        0.0f,
        0.0f,
        0.0f,
        1.0f);

    glClear(
        GL_COLOR_BUFFER_BIT);

    SDL_Log(
        "OpenGL context created.");

    // Initialize RmlUi GL3 renderer
    if (!RmlGL3::Initialize())
    {
        return Fail(
            window,
            gl_context,
            6,
            "RmlGL3::Initialize failed");
    }

    {
        SystemInterface_SDL system_interface(
            window);

        RenderInterface_GL3 render_interface;

        // Drawable size
        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSizeInPixels(
                window,
                &width,
                &height))
        {
            width = initial_width;
            height = initial_height;
        }

        if (width < 1)
        {
            width = initial_width;
        }

        if (height < 1)
        {
            height = initial_height;
        }

        render_interface.SetViewport(
            width,
            height);

        // RmlUi interfaces
        Rml::SetSystemInterface(
            &system_interface);

        Rml::SetRenderInterface(
            &render_interface);

        if (!Rml::Initialise())
        {
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                7,
                "Rml::Initialise failed");
        }

        // Context
        Rml::Context* context =
            Rml::CreateContext(
                "main",
                Rml::Vector2i(
                    width,
                    height));

        if (context == nullptr)
        {
            Rml::Shutdown();
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                8,
                "Rml::CreateContext failed");
        }

        // Display density
        const float window_scale =
            SDL_GetWindowDisplayScale(
                window);

        if (window_scale > 0.0f)
        {
            context->SetDensityIndependentPixelRatio(
                window_scale);
        }

        // Embedded black document; external HUD will
        // replace this in the next stage.
        const Rml::String ui = R"rml(
        <rml>
          <head>
            <style>
              body
              {
                margin: 0px;
                width: 100%;
                height: 100%;
                background-color: #000000;
                color: #000000;
              }
            </style>
          </head>
          <body></body>
        </rml>
        )rml";

        Rml::ElementDocument* document =
            context->LoadDocumentFromMemory(
                ui,
                "[embedded-ultron-ui.rml]");

        if (document == nullptr)
        {
            Rml::RemoveContext(
                "main");

            Rml::Shutdown();
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                9,
                "LoadDocumentFromMemory failed");
        }

        document->Show();

        SDL_Log(
            "Ultron black UI initialized.");

        // Main loop
        while (
            g_running.load(
                std::memory_order_relaxed))
        {
            SDL_Event event;

            while (SDL_PollEvent(&event))
            {
                if (event.type ==
                    SDL_EVENT_QUIT)
                {
                    g_running.store(
                        false,
                        std::memory_order_relaxed);

                    continue;
                }

                RmlSDL::InputEventHandler(
                    context,
                    window,
                    event);
            }

            if (!g_running.load(
                    std::memory_order_relaxed))
            {
                break;
            }

            if (g_paused.load(
                    std::memory_order_relaxed))
            {
                SDL_Delay(16);
                continue;
            }

            // Responsive drawable size
            int new_width = 0;
            int new_height = 0;

            if (SDL_GetWindowSizeInPixels(
                    window,
                    &new_width,
                    &new_height))
            {
                if (new_width > 0 &&
                    new_height > 0 &&
                    (new_width != width ||
                     new_height != height))
                {
                    width = new_width;
                    height = new_height;

                    context->SetDimensions(
                        Rml::Vector2i(
                            width,
                            height));

                    render_interface.SetViewport(
                        width,
                        height);
                }
            }

            // Pure-black frame
            glDisable(GL_SCISSOR_TEST);

            glClearColor(
                0.0f,
                0.0f,
                0.0f,
                1.0f);

            glClear(
                GL_COLOR_BUFFER_BIT);

            context->Update();

            render_interface.BeginFrame();

            context->Render();

            render_interface.EndFrame();

            SDL_GL_SwapWindow(
                window);
        }

        // RmlUi shutdown
        Rml::RemoveContext(
            "main");

        Rml::Shutdown();
    }

    // GL + SDL shutdown
    RmlGL3::Shutdown();

    SDL_GL_DestroyContext(
        gl_context);

    SDL_DestroyWindow(
        window);

    SDL_Quit();

    SDL_Log(
        "Ultron shutdown cleanly.");

    return 0;
}
