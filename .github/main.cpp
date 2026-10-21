#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <GLES3/gl3.h>

#include <atomic>
#include <cstdio>
#include <utility>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/FileInterface.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"
#include "action.h"

namespace
{
    constexpr const char* kAppName = "Ultron";
    constexpr const char* kInitialGui = "ui/main.rml";

    std::atomic_bool g_running{true};
    std::atomic_bool g_paused{false};

    // --------------------------------------------------------
    // SDL file access for RML, RCSS and font files.
    // SDL3 can read files packaged in Android app assets.
    // --------------------------------------------------------

    class SDLFileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(const Rml::String& path) override
        {
            return SDL_IOFromFile(path.c_str(), "rb");
        }

        void Close(Rml::FileHandle file) override
        {
            if (file != nullptr)
            {
                SDL_CloseIO(
                    static_cast<SDL_IOStream*>(file));
            }
        }

        size_t Read(
            void* buffer,
            size_t size,
            Rml::FileHandle file) override
        {
            if (file == nullptr || buffer == nullptr)
            {
                return 0;
            }

            return SDL_ReadIO(
                static_cast<SDL_IOStream*>(file),
                buffer,
                size);
        }

        bool Seek(
            Rml::FileHandle file,
            long offset,
            int origin) override
        {
            if (file == nullptr)
            {
                return false;
            }

            SDL_IOWhence whence;

            switch (origin)
            {
                case SEEK_SET:
                    whence = SDL_IO_SEEK_SET;
                    break;

                case SEEK_CUR:
                    whence = SDL_IO_SEEK_CUR;
                    break;

                case SEEK_END:
                    whence = SDL_IO_SEEK_END;
                    break;

                default:
                    return false;
            }

            return SDL_SeekIO(
                static_cast<SDL_IOStream*>(file),
                static_cast<Sint64>(offset),
                whence) >= 0;
        }

        size_t Tell(Rml::FileHandle file) override
        {
            if (file == nullptr)
            {
                return 0;
            }

            const Sint64 position = SDL_TellIO(
                static_cast<SDL_IOStream*>(file));

            if (position < 0)
            {
                return 0;
            }

            return static_cast<size_t>(position);
        }
    };

    // --------------------------------------------------------
    // Android lifecycle events.
    // --------------------------------------------------------

    bool HandleAppEvents(
        void* userdata,
        SDL_Event* event)
    {
        (void)userdata;

        switch (event->type)
        {
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
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
                    "Ultron: Android low-memory notification.");
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

    // --------------------------------------------------------
    // Keep the empty fallback entirely black if the initial
    // external GUI file has not been packaged yet.
    // --------------------------------------------------------

    Rml::ElementDocument* LoadBlackFallback(
        Rml::Context* context)
    {
        const Rml::String fallback = R"rml(
            <rml>
              <head>
                <style>
                  body {
                    margin: 0px;
                    width: 100%;
                    height: 100%;
                    background-color: #000000;
                  }
                </style>
              </head>
              <body></body>
            </rml>
        )rml";

        return context->LoadDocumentFromMemory(
            fallback,
            "[ultron-black-fallback.rml]");
    }

    bool IsValidGuiPath(const Rml::String& path)
    {
        // GUI documents must come from the packaged UI folder.
        // Example: ui/settings.rml
        return path.rfind("ui/", 0) == 0
            && path.size() >= 7
            && path.compare(
                   path.size() - 4,
                   4,
                   ".rml") == 0;
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

        if (error != nullptr && error[0] != '\0')
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "SDL error: %s",
                error);
        }

        if (gl_context != nullptr)
        {
            SDL_GL_DestroyContext(gl_context);
        }

        if (window != nullptr)
        {
            SDL_DestroyWindow(window);
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

    SDL_Log("Ultron native engine starting.");

    // --------------------------------------------------------
    // SDL initialization.
    // --------------------------------------------------------

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

    // --------------------------------------------------------
    // OpenGL ES 3.2.
    // --------------------------------------------------------

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

    // --------------------------------------------------------
    // Create the native window.
    // --------------------------------------------------------

    SDL_PropertiesID properties = SDL_CreateProperties();

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
        kAppName);

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
        SDL_CreateWindowWithProperties(properties);

    SDL_DestroyProperties(properties);

    if (window == nullptr)
    {
        return Fail(
            nullptr,
            nullptr,
            3,
            "SDL window creation failed");
    }

    SDL_GLContext gl_context =
        SDL_GL_CreateContext(window);

    if (gl_context == nullptr)
    {
        return Fail(
            window,
            nullptr,
            4,
            "SDL_GL_CreateContext failed");
    }

    if (!SDL_GL_MakeCurrent(window, gl_context))
    {
        return Fail(
            window,
            gl_context,
            5,
            "SDL_GL_MakeCurrent failed");
    }

    SDL_GL_SetSwapInterval(1);

    glDisable(GL_SCISSOR_TEST);

    glClearColor(
        0.0f,
        0.0f,
        0.0f,
        1.0f);

    glClear(GL_COLOR_BUFFER_BIT);

    // --------------------------------------------------------
    // RmlUi renderer.
    // --------------------------------------------------------

    if (!RmlGL3::Initialize())
    {
        return Fail(
            window,
            gl_context,
            6,
            "RmlGL3::Initialize failed");
    }

    {
        SystemInterface_SDL system_interface(window);
        RenderInterface_GL3 render_interface;
        SDLFileInterface file_interface;

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

        render_interface.SetViewport(width, height);

        Rml::SetSystemInterface(&system_interface);
        Rml::SetRenderInterface(&render_interface);

        // Must be installed before Rml::Initialise().
        Rml::SetFileInterface(&file_interface);

        if (!Rml::Initialise())
        {
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                7,
                "Rml::Initialise failed");
        }

        Rml::Context* context = Rml::CreateContext(
            "main",
            Rml::Vector2i(width, height));

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

        const float window_scale =
            SDL_GetWindowDisplayScale(window);

        if (window_scale > 0.0f)
        {
            context->SetDensityIndependentPixelRatio(
                window_scale);
        }

        // Android provides the Roboto system font.
        // If it cannot be loaded, the engine still continues.
#if defined(__ANDROID__)
        if (!Rml::LoadFontFace(
                "/system/fonts/Roboto-Regular.ttf"))
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "System Roboto font could not be loaded.");
        }
#endif

        // ----------------------------------------------------
        // Shared GUI loader action.
        //
        // An RML button can request another packaged document:
        // <button onclick="load_gui"
        //         data-path="ui/settings.rml">
        //     Settings
        // </button>
        //
        // The actual document change is processed after
        // event/update handling, not inside the click callback.
        // ----------------------------------------------------

        Rml::String pending_gui_path;
        Rml::String current_gui_path;
        Rml::ElementDocument* current_document = nullptr;

        const bool loader_registered =
            Ultron::Actions::Register(
                "load_gui",
                [&pending_gui_path](
                    Rml::Element& element,
                    Rml::Event& event)
                {
                    (void)event;

                    const Rml::String path =
                        element.GetAttribute<Rml::String>(
                            "data-path",
                            Rml::String{});

                    if (!IsValidGuiPath(path))
                    {
                        SDL_LogWarn(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "load_gui rejected an invalid UI path.");

                        return;
                    }

                    pending_gui_path = path;
                });

        if (!loader_registered ||
            !Ultron::Actions::Initialize())
        {
            Rml::RemoveContext("main");
            Ultron::Actions::Shutdown();
            Rml::Shutdown();
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                9,
                "Action system initialization failed");
        }

        // ----------------------------------------------------
        // Load the starting GUI from an external RML file.
        // If the UI asset isn't packaged yet, preserve the
        // working black base rather than crashing.
        // ----------------------------------------------------

        current_document = context->LoadDocument(kInitialGui);

        if (current_document != nullptr)
        {
            current_gui_path = kInitialGui;
            current_document->Show();

            SDL_Log(
                "Initial GUI loaded: %s",
                kInitialGui);
        }
        else
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "ui/main.rml not found; using black fallback.");

            current_document = LoadBlackFallback(context);

            if (current_document == nullptr)
            {
                Rml::RemoveContext("main");
                Ultron::Actions::Shutdown();
                Rml::Shutdown();
                RmlGL3::Shutdown();

                return Fail(
                    window,
                    gl_context,
                    10,
                    "Could not create the black fallback document");
            }

            current_document->Show();
        }

        // ----------------------------------------------------
        // Main native event and rendering loop.
        // ----------------------------------------------------

        while (g_running.load(std::memory_order_relaxed))
        {
            SDL_Event event;

            while (SDL_PollEvent(&event))
            {
                if (event.type == SDL_EVENT_QUIT)
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

            if (!g_running.load(std::memory_order_relaxed))
            {
                break;
            }

            if (g_paused.load(std::memory_order_relaxed))
            {
                SDL_Delay(16);
                continue;
            }

            // Keep the RmlUi viewport synchronized with the
            // physical drawable size.
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
                        Rml::Vector2i(width, height));

                    render_interface.SetViewport(
                        width,
                        height);
                }
            }

            // Update RmlUi and process pending GUI changes.
            context->Update();

            if (!pending_gui_path.empty())
            {
                Rml::String requested_path =
                    std::move(pending_gui_path);

                pending_gui_path.clear();

                if (requested_path != current_gui_path)
                {
                    Rml::ElementDocument* next_document =
                        context->LoadDocument(requested_path);

                    if (next_document != nullptr)
                    {
                        if (current_document != nullptr)
                        {
                            current_document->Hide();
                            current_document->Close();
                        }

                        current_document = next_document;
                        current_gui_path = requested_path;
                        current_document->Show();

                        SDL_Log(
                            "GUI loaded: %s",
                            requested_path.c_str());
                    }
                    else
                    {
                        SDL_LogError(
                            SDL_LOG_CATEGORY_APPLICATION,
                            "Could not load GUI: %s",
                            requested_path.c_str());
                    }
                }
            }

            // Permanent black background. GUI renders above it.
            glDisable(GL_SCISSOR_TEST);

            glClearColor(
                0.0f,
                0.0f,
                0.0f,
                1.0f);

            glClear(GL_COLOR_BUFFER_BIT);

            render_interface.BeginFrame();
            context->Render();
            render_interface.EndFrame();

            SDL_GL_SwapWindow(window);
        }

        // Remove documents/listeners before shutting down
        // the shared action registry and RmlUi itself.
        Rml::RemoveContext("main");

        Ultron::Actions::Shutdown();

        Rml::Shutdown();
    }

    RmlGL3::Shutdown();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    SDL_Log("Ultron shutdown cleanly.");

    return 0;
}
