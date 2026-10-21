#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <GLES3/gl3.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/FileInterface.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

#include <atomic>
#include <cstdio>
#include <string>
#include <unordered_map>

// Public registration interface for separate native action files.
// A future action file can register a callback by name.
using UltronActionCallback = void (*)();

extern "C" bool Ultron_RegisterAction(
    const char* name,
    UltronActionCallback callback);

namespace
{
    std::atomic_bool g_running{true};
    std::atomic_bool g_paused{false};

    // ---------------------------------------------------------
    // Native action registry
    //
    // The engine knows action names, not feature implementations.
    // Gallery, Camera, Message, etc. belong in separate .cpp files.
    // ---------------------------------------------------------

    using ActionRegistry =
        std::unordered_map<std::string, UltronActionCallback>;

    ActionRegistry& GetActionRegistry()
    {
        // Function-local static avoids cross-file static
        // initialization ordering problems during registration.
        static ActionRegistry registry;
        return registry;
    }

    void DispatchAction(const Rml::String& action_name)
    {
        if (action_name.empty())
        {
            return;
        }

        const auto& registry = GetActionRegistry();

        const auto found = registry.find(
            std::string(action_name.c_str()));

        if (found == registry.end())
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: no native action registered for '%s'.",
                action_name.c_str());

            return;
        }

        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: dispatching action '%s'.",
            action_name.c_str());

        // The registered native callback runs on the UI/event thread.
        found->second();
    }

    // ---------------------------------------------------------
    // SDL-backed file interface
    //
    // Allows RmlUi to read RML and RCSS using SDL's file API.
    // SDL_IOFromFile can access packaged Android assets.
    // ---------------------------------------------------------

    class SDLFileInterface final : public Rml::FileInterface
    {
    public:
        Rml::FileHandle Open(
            const Rml::String& path) override
        {
            SDL_IOStream* stream = SDL_IOFromFile(
                path.c_str(),
                "rb");

            if (stream == nullptr)
            {
                SDL_LogDebug(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: unable to open UI resource '%s'.",
                    path.c_str());
            }

            return stream;
        }

        void Close(
            Rml::FileHandle file) override
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
            if (file == nullptr ||
                buffer == nullptr ||
                size == 0)
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

        size_t Tell(
            Rml::FileHandle file) override
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

    // ---------------------------------------------------------
    // Common click connector
    //
    // One listener handles clicks across the entire document.
    // Buttons identify their requested action with data-action.
    //
    // Example:
    // <button data-action="gallery">Open Gallery</button>
    // ---------------------------------------------------------

    class ClickActionListener final : public Rml::EventListener
    {
    public:
        explicit ClickActionListener(
            Rml::ElementDocument* document)
            : document_(document)
        {
        }

        void ProcessEvent(
            Rml::Event& event) override
        {
            if (document_ == nullptr)
            {
                return;
            }

            Rml::Element* element =
                event.GetTargetElement();

            // Walk upward so clicks on nested icons or text
            // still find the action assigned to their parent.
            while (element != nullptr &&
                   element != document_)
            {
                if (element->HasAttribute("data-action"))
                {
                    const Rml::String action_name =
                        element->GetAttribute<Rml::String>(
                            "data-action",
                            Rml::String{});

                    if (!action_name.empty())
                    {
                        DispatchAction(action_name);
                        return;
                    }
                }

                element = element->GetParentNode();
            }
        }

    private:
        Rml::ElementDocument* document_ = nullptr;
    };

    // ---------------------------------------------------------
    // Android / SDL application lifecycle
    // ---------------------------------------------------------

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
                    "Ultron: Android low-memory notification received.");
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

// -------------------------------------------------------------
// Action registration API
//
// Separate action translation units can register named callbacks.
// Returns false for an invalid name, null callback, or duplicate.
// -------------------------------------------------------------

extern "C" bool Ultron_RegisterAction(
    const char* name,
    UltronActionCallback callback)
{
    if (name == nullptr ||
        name[0] == '\0' ||
        callback == nullptr)
    {
        return false;
    }

    const auto result = GetActionRegistry().emplace(
        name,
        callback);

    return result.second;
}

// -------------------------------------------------------------
// Main
// -------------------------------------------------------------

int main(
    int argc,
    char** argv)
{
    (void)argc;
    (void)argv;

    constexpr int initial_width = 1280;
    constexpr int initial_height = 720;

    constexpr int error_sdl_init = 1;
    constexpr int error_properties = 2;
    constexpr int error_window = 3;
    constexpr int error_gl_context = 4;
    constexpr int error_gl_current = 5;
    constexpr int error_renderer = 6;
    constexpr int error_rml_init = 7;
    constexpr int error_rml_context = 8;
    constexpr int error_rml_document = 9;

    g_running.store(
        true,
        std::memory_order_relaxed);

    g_paused.store(
        false,
        std::memory_order_relaxed);

    SDL_SetLogPriority(
        SDL_LOG_CATEGORY_APPLICATION,
        SDL_LOG_PRIORITY_INFO);

    SDL_Log(
        "Ultron starting...");

    // ---------------------------------------------------------
    // SDL initialization
    // ---------------------------------------------------------

    if (!SDL_Init(
            SDL_INIT_VIDEO |
            SDL_INIT_EVENTS))
    {
        return Fail(
            nullptr,
            nullptr,
            error_sdl_init,
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

    // ---------------------------------------------------------
    // OpenGL ES 3.2
    // ---------------------------------------------------------

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

    // ---------------------------------------------------------
    // Window
    // ---------------------------------------------------------

    SDL_PropertiesID properties =
        SDL_CreateProperties();

    if (properties == 0)
    {
        return Fail(
            nullptr,
            nullptr,
            error_properties,
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
            error_window,
            "SDL_CreateWindowWithProperties failed");
    }

    // ---------------------------------------------------------
    // OpenGL context
    // ---------------------------------------------------------

    SDL_GLContext gl_context =
        SDL_GL_CreateContext(
            window);

    if (gl_context == nullptr)
    {
        return Fail(
            window,
            nullptr,
            error_gl_context,
            "SDL_GL_CreateContext failed");
    }

    if (!SDL_GL_MakeCurrent(
            window,
            gl_context))
    {
        return Fail(
            window,
            gl_context,
            error_gl_current,
            "SDL_GL_MakeCurrent failed");
    }

    SDL_GL_SetSwapInterval(1);

    // Keep the base frame black.
    glDisable(GL_SCISSOR_TEST);

    glClearColor(
        0.0f,
        0.0f,
        0.0f,
        1.0f);

    glClear(
        GL_COLOR_BUFFER_BIT);

    SDL_GL_SwapWindow(window);

    SDL_Log(
        "Ultron: OpenGL ES context created.");

    // ---------------------------------------------------------
    // RmlUi renderer
    // ---------------------------------------------------------

    if (!RmlGL3::Initialize())
    {
        return Fail(
            window,
            gl_context,
            error_renderer,
            "RmlGL3::Initialize failed");
    }

    {
        SystemInterface_SDL system_interface(
            window);

        RenderInterface_GL3 render_interface;

        SDLFileInterface file_interface;

        // -----------------------------------------------------
        // Drawable size
        // -----------------------------------------------------

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

        // -----------------------------------------------------
        // RmlUi interfaces
        // -----------------------------------------------------

        Rml::SetSystemInterface(
            &system_interface);

        Rml::SetRenderInterface(
            &render_interface);

        Rml::SetFileInterface(
            &file_interface);

        if (!Rml::Initialise())
        {
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                error_rml_init,
                "Rml::Initialise failed");
        }

        // -----------------------------------------------------
        // RmlUi context
        // -----------------------------------------------------

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
                error_rml_context,
                "Rml::CreateContext failed");
        }

        const float window_scale =
            SDL_GetWindowDisplayScale(
                window);

        if (window_scale > 0.0f)
        {
            context->SetDensityIndependentPixelRatio(
                window_scale);
        }

        // -----------------------------------------------------
        // External GUI
        //
        // Primary document:
        // ui/home.rml
        //
        // Its RCSS file can be linked inside the RML document.
        // If the external document is not packaged yet, keep
        // the app usable with the original pure-black fallback.
        // -----------------------------------------------------

        const Rml::String fallback_ui = R"rml(
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
            context->LoadDocument(
                "ui/home.rml");

        if (document == nullptr)
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: ui/home.rml could not be loaded; "
                "using the black fallback document.");

            document =
                context->LoadDocumentFromMemory(
                    fallback_ui,
                    "[embedded-ultron-fallback.rml]");
        }

        if (document == nullptr)
        {
            Rml::RemoveContext("main");
            Rml::Shutdown();
            RmlGL3::Shutdown();

            return Fail(
                window,
                gl_context,
                error_rml_document,
                "Unable to load the external or fallback RML document");
        }

        // -----------------------------------------------------
        // Common click connection
        //
        // Attach once to the document in the capture phase.
        // All controls can request an action using data-action.
        // -----------------------------------------------------

        ClickActionListener click_listener(
            document);

        document->AddEventListener(
            "click",
            &click_listener,
            true);

        document->Show();

        SDL_Log(
            "Ultron: GUI document initialized.");

        SDL_Log(
            "Ultron: common click connector initialized.");

        // -----------------------------------------------------
        // Main loop
        // -----------------------------------------------------

        while (g_running.load(
            std::memory_order_relaxed))
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

            // Responsive drawable size.
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

            // Clear the frame to pure black before rendering UI.
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

        // -----------------------------------------------------
        // RmlUi shutdown
        //
        // The listener remains alive until its document/context
        // has been removed and RmlUi has shut down.
        // -----------------------------------------------------

        Rml::RemoveContext(
            "main");

        Rml::Shutdown();
    }

    // ---------------------------------------------------------
    // GL + SDL shutdown
    // ---------------------------------------------------------

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
