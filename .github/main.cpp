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
#include <exception>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using UltronActionCallback = void (*)();
using UltronUITaskCallback = void (*)(void* user_data);

extern "C" bool Ultron_RegisterAction(
    const char* name,
    UltronActionCallback callback);

extern "C" bool Ultron_ShowDocument(
    const char* document_name,
    const char* rml_path);

extern "C" bool Ultron_GoBack();

extern "C" bool Ultron_IsDocumentLoaded(
    const char* document_name);

extern "C" bool Ultron_SetElementClass(
    const char* element_id,
    const char* class_name,
    bool enabled);

extern "C" bool Ultron_SetElementText(
    const char* element_id,
    const char* text);

extern "C" bool Ultron_SetElementProperty(
    const char* element_id,
    const char* property_name,
    const char* value);

extern "C" bool Ultron_SetElementAttribute(
    const char* element_id,
    const char* attribute_name,
    const char* value);

extern "C" bool Ultron_RemoveElementAttribute(
    const char* element_id,
    const char* attribute_name);

extern "C" bool Ultron_PostToUI(
    UltronUITaskCallback callback,
    void* user_data);

namespace
{
    // ---------------------------------------------------------
    // Application state
    // ---------------------------------------------------------

    std::atomic_bool g_running{true};
    std::atomic_bool g_paused{false};
    std::atomic_bool g_accept_ui_tasks{false};

    // These pointers are accessed only on the UI/main thread.
    Rml::Context* g_context = nullptr;
    Rml::Element* g_context_root = nullptr;
    Rml::ElementDocument* g_active_document = nullptr;

    std::unordered_map<std::string, Rml::ElementDocument*> g_documents;
    std::string g_active_document_name;
    std::vector<std::string> g_document_history;
    std::thread::id g_ui_thread_id{};

    // ---------------------------------------------------------
    // Native action registry
    //
    // Function-local statics avoid cross-translation-unit
    // static initialization order problems with action files.
    // ---------------------------------------------------------

    using ActionRegistry =
        std::unordered_map<std::string, UltronActionCallback>;

    ActionRegistry& GetActionRegistry()
    {
        static ActionRegistry registry;
        return registry;
    }

    std::mutex& GetActionRegistryMutex()
    {
        static std::mutex mutex;
        return mutex;
    }

    void DispatchAction(const Rml::String& action_name)
    {
        if (action_name.empty())
        {
            return;
        }

        UltronActionCallback callback = nullptr;

        {
            std::lock_guard lock(GetActionRegistryMutex());

            const auto found = GetActionRegistry().find(
                std::string(action_name.c_str()));

            if (found != GetActionRegistry().end())
            {
                callback = found->second;
            }
        }

        if (callback == nullptr)
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

        // Do not hold the registry mutex while calling user code.
        callback();
    }

    // ---------------------------------------------------------
    // UI-thread task queue
    //
    // Background work can post callbacks here. Callbacks execute
    // on the main/UI thread in the frame loop.
    //
    // user_data remains owned by the caller.
    // ---------------------------------------------------------

    struct PendingUITask
    {
        UltronUITaskCallback callback = nullptr;
        void* user_data = nullptr;
    };

    struct UITaskQueue
    {
        std::mutex mutex;
        std::queue<PendingUITask> tasks;
    };

    UITaskQueue& GetUITaskQueue()
    {
        static UITaskQueue queue;
        return queue;
    }

    bool IsUIThread()
    {
        return g_ui_thread_id != std::thread::id{} &&
               std::this_thread::get_id() == g_ui_thread_id;
    }

    bool RequireUIThread(const char* function_name)
    {
        if (IsUIThread())
        {
            return true;
        }

        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: %s must run on the UI thread. "
            "Use Ultron_PostToUI from background threads.",
            function_name != nullptr ? function_name : "GUI API");

        return false;
    }

    void ProcessPendingUITasks()
    {
        UITaskQueue& queue = GetUITaskQueue();
        std::queue<PendingUITask> pending;

        {
            std::lock_guard lock(queue.mutex);
            pending.swap(queue.tasks);
        }

        while (!pending.empty())
        {
            const PendingUITask task = pending.front();
            pending.pop();

            if (task.callback == nullptr)
            {
                continue;
            }

            try
            {
                task.callback(task.user_data);
            }
            catch (const std::exception& exception)
            {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: UI task threw an exception: %s",
                    exception.what());
            }
            catch (...)
            {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: UI task threw an unknown exception.");
            }
        }
    }

    void StopAcceptingUITasks()
    {
        g_accept_ui_tasks.store(false, std::memory_order_release);

        UITaskQueue& queue = GetUITaskQueue();
        std::lock_guard lock(queue.mutex);

        // Queued tasks remaining during shutdown are discarded.
        // The caller retains ownership of their user_data.
        std::queue<PendingUITask> empty;
        queue.tasks.swap(empty);
    }

    // ---------------------------------------------------------
    // RmlUi file interface backed by SDL3 IO streams.
    //
    // RmlUi 6.3 uses uintptr_t as its FileHandle type.
    // ---------------------------------------------------------

    class SDLFileInterface final : public Rml::FileInterface
    {
    private:
        static SDL_IOStream* ToStream(Rml::FileHandle file)
        {
            return reinterpret_cast<SDL_IOStream*>(file);
        }

    public:
        Rml::FileHandle Open(const Rml::String& path) override
        {
            SDL_IOStream* stream = SDL_IOFromFile(
                path.c_str(),
                "rb");

            if (stream == nullptr)
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: unable to open UI resource '%s'.",
                    path.c_str());

                return 0;
            }

            return reinterpret_cast<Rml::FileHandle>(stream);
        }

        void Close(Rml::FileHandle file) override
        {
            if (file != 0)
            {
                SDL_CloseIO(ToStream(file));
            }
        }

        size_t Read(
            void* buffer,
            size_t size,
            Rml::FileHandle file) override
        {
            if (file == 0 || buffer == nullptr || size == 0)
            {
                return 0;
            }

            return SDL_ReadIO(ToStream(file), buffer, size);
        }

        bool Seek(
            Rml::FileHandle file,
            long offset,
            int origin) override
        {
            if (file == 0)
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
                ToStream(file),
                static_cast<Sint64>(offset),
                whence) >= 0;
        }

        size_t Tell(Rml::FileHandle file) override
        {
            if (file == 0)
            {
                return 0;
            }

            const Sint64 position = SDL_TellIO(ToStream(file));

            return position >= 0
                ? static_cast<size_t>(position)
                : 0;
        }
    };

    // ---------------------------------------------------------
    // One click connector shared by every document.
    //
    // The listener is attached once to the context root in
    // capture phase. Newly loaded documents work automatically.
    // ---------------------------------------------------------

    class ClickActionListener final : public Rml::EventListener
    {
    public:
        explicit ClickActionListener(Rml::Element* context_root)
            : context_root_(context_root)
        {
        }

        void ProcessEvent(Rml::Event& event) override
        {
            if (context_root_ == nullptr)
            {
                return;
            }

            Rml::Element* element = event.GetTargetElement();

            // Walk upward to support clicks on nested text or icons.
            while (element != nullptr && element != context_root_)
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
        Rml::Element* context_root_ = nullptr;
    };

    // ---------------------------------------------------------
    // Multiple-document navigation
    //
    // Documents are loaded at most once by their logical name.
    // Switching documents hides the previous screen.
    // ---------------------------------------------------------

    Rml::ElementDocument* FindDocument(const char* document_name)
    {
        if (document_name == nullptr || document_name[0] == '\0')
        {
            return nullptr;
        }

        const auto found = g_documents.find(document_name);

        return found != g_documents.end()
            ? found->second
            : nullptr;
    }

    bool SwitchToLoadedDocument(
        const std::string& document_name,
        bool remember_current)
    {
        const auto found = g_documents.find(document_name);

        if (found == g_documents.end() || found->second == nullptr)
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: document '%s' is not loaded.",
                document_name.c_str());

            return false;
        }

        Rml::ElementDocument* next_document = found->second;

        if (next_document == g_active_document)
        {
            next_document->Show();
            g_active_document_name = document_name;
            return true;
        }

        if (remember_current &&
            g_active_document != nullptr &&
            !g_active_document_name.empty())
        {
            g_document_history.push_back(g_active_document_name);
        }

        if (g_active_document != nullptr)
        {
            g_active_document->Hide();
        }

        next_document->Show();

        g_active_document = next_document;
        g_active_document_name = document_name;

        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: active document is now '%s'.",
            g_active_document_name.c_str());

        return true;
    }

    bool ShowDocumentInternal(
        const char* document_name,
        const char* rml_path,
        bool remember_current)
    {
        if (!RequireUIThread("Ultron_ShowDocument") ||
            g_context == nullptr)
        {
            return false;
        }

        if (document_name == nullptr || document_name[0] == '\0')
        {
            return false;
        }

        const std::string name(document_name);

        if (g_documents.find(name) == g_documents.end())
        {
            if (rml_path == nullptr || rml_path[0] == '\0')
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: document '%s' is not loaded and "
                    "no RML path was supplied.",
                    document_name);

                return false;
            }

            Rml::ElementDocument* document =
                g_context->LoadDocument(rml_path);

            if (document == nullptr)
            {
                SDL_LogWarn(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron: failed to load document '%s' from '%s'.",
                    document_name,
                    rml_path);

                return false;
            }

            g_documents.emplace(name, document);
        }

        return SwitchToLoadedDocument(name, remember_current);
    }

    Rml::Element* FindActiveElement(const char* element_id)
    {
        if (g_active_document == nullptr ||
            element_id == nullptr ||
            element_id[0] == '\0')
        {
            return nullptr;
        }

        return g_active_document->GetElementById(element_id);
    }

    Rml::String EscapeRmlText(std::string_view text)
    {
        Rml::String escaped;
        escaped.reserve(text.size());

        for (const char character : text)
        {
            switch (character)
            {
                case '&':
                    escaped += "&amp;";
                    break;

                case '<':
                    escaped += "&lt;";
                    break;

                case '>':
                    escaped += "&gt;";
                    break;

                default:
                    escaped += character;
                    break;
            }
        }

        return escaped;
    }

    // ---------------------------------------------------------
    // Android / SDL lifecycle events
    // ---------------------------------------------------------

    bool HandleAppEvents(void* userdata, SDL_Event* event)
    {
        (void)userdata;

        if (event == nullptr)
        {
            return true;
        }

        switch (event->type)
        {
            case SDL_EVENT_WILL_ENTER_BACKGROUND:
            case SDL_EVENT_DID_ENTER_BACKGROUND:
            case SDL_EVENT_WILL_ENTER_FOREGROUND:
                g_paused.store(true, std::memory_order_relaxed);
                break;

            case SDL_EVENT_DID_ENTER_FOREGROUND:
                g_paused.store(false, std::memory_order_relaxed);
                break;

            case SDL_EVENT_LOW_MEMORY:
                SDL_Log("Ultron: Android low-memory notification received.");
                break;

            case SDL_EVENT_TERMINATING:
                g_running.store(false, std::memory_order_relaxed);
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
            message != nullptr ? message : "unknown error");

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

// -------------------------------------------------------------
// Native action registration API
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

    std::lock_guard lock(GetActionRegistryMutex());

    // Duplicate names are rejected rather than silently replaced.
    return GetActionRegistry().emplace(name, callback).second;
}

// -------------------------------------------------------------
// Multiple-screen API
// -------------------------------------------------------------

extern "C" bool Ultron_ShowDocument(
    const char* document_name,
    const char* rml_path)
{
    return ShowDocumentInternal(document_name, rml_path, true);
}

extern "C" bool Ultron_GoBack()
{
    if (!RequireUIThread("Ultron_GoBack") || g_context == nullptr)
    {
        return false;
    }

    while (!g_document_history.empty())
    {
        std::string previous_name =
            std::move(g_document_history.back());

        g_document_history.pop_back();

        if (previous_name != g_active_document_name &&
            SwitchToLoadedDocument(previous_name, false))
        {
            return true;
        }
    }

    return false;
}

extern "C" bool Ultron_IsDocumentLoaded(const char* document_name)
{
    if (!RequireUIThread("Ultron_IsDocumentLoaded"))
    {
        return false;
    }

    return FindDocument(document_name) != nullptr;
}

// -------------------------------------------------------------
// GUI manipulation API
//
// These functions operate on elements of the active document.
// Call them from actions on the UI thread. For background tasks,
// use Ultron_PostToUI and invoke them from its callback.
// -------------------------------------------------------------

extern "C" bool Ultron_SetElementClass(
    const char* element_id,
    const char* class_name,
    bool enabled)
{
    if (!RequireUIThread("Ultron_SetElementClass") ||
        class_name == nullptr ||
        class_name[0] == '\0')
    {
        return false;
    }

    Rml::Element* element = FindActiveElement(element_id);

    if (element == nullptr)
    {
        return false;
    }

    element->SetClass(class_name, enabled);
    return true;
}

extern "C" bool Ultron_SetElementText(
    const char* element_id,
    const char* text)
{
    if (!RequireUIThread("Ultron_SetElementText") || text == nullptr)
    {
        return false;
    }

    Rml::Element* element = FindActiveElement(element_id);

    if (element == nullptr)
    {
        return false;
    }

    // SetInnerRML accepts markup, so escape ordinary text first.
    element->SetInnerRML(EscapeRmlText(text));
    return true;
}

extern "C" bool Ultron_SetElementProperty(
    const char* element_id,
    const char* property_name,
    const char* value)
{
    if (!RequireUIThread("Ultron_SetElementProperty") ||
        property_name == nullptr ||
        property_name[0] == '\0' ||
        value == nullptr)
    {
        return false;
    }

    Rml::Element* element = FindActiveElement(element_id);

    return element != nullptr &&
           element->SetProperty(property_name, value);
}

extern "C" bool Ultron_SetElementAttribute(
    const char* element_id,
    const char* attribute_name,
    const char* value)
{
    if (!RequireUIThread("Ultron_SetElementAttribute") ||
        attribute_name == nullptr ||
        attribute_name[0] == '\0' ||
        value == nullptr)
    {
        return false;
    }

    Rml::Element* element = FindActiveElement(element_id);

    if (element == nullptr)
    {
        return false;
    }

    element->SetAttribute(attribute_name, Rml::String(value));
    return true;
}

extern "C" bool Ultron_RemoveElementAttribute(
    const char* element_id,
    const char* attribute_name)
{
    if (!RequireUIThread("Ultron_RemoveElementAttribute") ||
        attribute_name == nullptr ||
        attribute_name[0] == '\0')
    {
        return false;
    }

    Rml::Element* element = FindActiveElement(element_id);

    if (element == nullptr)
    {
        return false;
    }

    element->RemoveAttribute(attribute_name);
    return true;
}

// -------------------------------------------------------------
// Post native work back to the UI thread
// -------------------------------------------------------------

extern "C" bool Ultron_PostToUI(
    UltronUITaskCallback callback,
    void* user_data)
{
    if (callback == nullptr ||
        !g_accept_ui_tasks.load(std::memory_order_acquire))
    {
        return false;
    }

    UITaskQueue& queue = GetUITaskQueue();
    std::lock_guard lock(queue.mutex);

    // Recheck under the queue lock so shutdown cannot accept
    // a task after the queue has been stopped.
    if (!g_accept_ui_tasks.load(std::memory_order_relaxed))
    {
        return false;
    }

    queue.tasks.push(PendingUITask{callback, user_data});
    return true;
}

// -------------------------------------------------------------
// Main application
// -------------------------------------------------------------

int main(int argc, char** argv)
{
    (void)argc;
    (void)argv;

    constexpr int initial_width = 1280;
    constexpr int initial_height = 720;

    g_ui_thread_id = std::this_thread::get_id();
    g_running.store(true, std::memory_order_relaxed);
    g_paused.store(false, std::memory_order_relaxed);
    g_accept_ui_tasks.store(false, std::memory_order_relaxed);

    SDL_SetLogPriority(
        SDL_LOG_CATEGORY_APPLICATION,
        SDL_LOG_PRIORITY_INFO);

    SDL_Log("Ultron starting...");

    // ---------------------------------------------------------
    // SDL initialization
    // ---------------------------------------------------------

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        return Fail(nullptr, nullptr, 1, "SDL_Init failed");
    }

    SDL_SetEventFilter(HandleAppEvents, nullptr);

    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // ---------------------------------------------------------
    // OpenGL ES 3.2
    // ---------------------------------------------------------

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_ES);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // ---------------------------------------------------------
    // Resizable high-density window
    // ---------------------------------------------------------

    SDL_PropertiesID properties = SDL_CreateProperties();

    if (properties == 0)
    {
        return Fail(nullptr, nullptr, 2, "SDL_CreateProperties failed");
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

    SDL_Window* window = SDL_CreateWindowWithProperties(properties);
    SDL_DestroyProperties(properties);

    if (window == nullptr)
    {
        return Fail(
            nullptr, nullptr, 3,
            "SDL_CreateWindowWithProperties failed");
    }

    // ---------------------------------------------------------
    // OpenGL context
    // ---------------------------------------------------------

    SDL_GLContext gl_context = SDL_GL_CreateContext(window);

    if (gl_context == nullptr)
    {
        return Fail(
            window, nullptr, 4,
            "SDL_GL_CreateContext failed");
    }

    if (!SDL_GL_MakeCurrent(window, gl_context))
    {
        return Fail(
            window, gl_context, 5,
            "SDL_GL_MakeCurrent failed");
    }

    SDL_GL_SetSwapInterval(1);

    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);

    SDL_Log("Ultron: OpenGL ES context created.");

    // ---------------------------------------------------------
    // RmlUi renderer and interfaces
    // ---------------------------------------------------------

    if (!RmlGL3::Initialize())
    {
        return Fail(
            window, gl_context, 6,
            "RmlGL3::Initialize failed");
    }

    int exit_code = 0;

    {
        SystemInterface_SDL system_interface(window);
        RenderInterface_GL3 render_interface;
        SDLFileInterface file_interface;

        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSizeInPixels(window, &width, &height))
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
        Rml::SetFileInterface(&file_interface);

        if (!Rml::Initialise())
        {
            RmlGL3::Shutdown();

            return Fail(
                window, gl_context, 7,
                "Rml::Initialise failed");
        }

        g_context = Rml::CreateContext(
            "main",
            Rml::Vector2i(width, height));

        if (g_context == nullptr)
        {
            Rml::Shutdown();
            RmlGL3::Shutdown();

            return Fail(
                window, gl_context, 8,
                "Rml::CreateContext failed");
        }

        g_context_root = g_context->GetRootElement();

        float window_scale = SDL_GetWindowDisplayScale(window);

        if (window_scale > 0.0f)
        {
            g_context->SetDensityIndependentPixelRatio(window_scale);
        }

        // -----------------------------------------------------
        // Attach one common click listener to the context root.
        // All documents in this context share this listener.
        // -----------------------------------------------------

        ClickActionListener click_listener(g_context_root);

        if (g_context_root != nullptr)
        {
            g_context_root->AddEventListener(
                "click",
                &click_listener,
                true);
        }

        // -----------------------------------------------------
        // Initial screen with embedded black fallback.
        //
        // Expected runtime paths:
        // ui/home.rml
        // ui/home.rcss
        // -----------------------------------------------------

        const Rml::String fallback_ui = R"rml(
<rml>
  <head>
    <style>
      body {
        margin: 0px;
        width: 100%;
        height: 100%;
        background-color: #000000;
        color: #ffffff;
      }
    </style>
  </head>
  <body></body>
</rml>
)rml";

        if (!Ultron_ShowDocument("home", "ui/home.rml"))
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: ui/home.rml unavailable; "
                "trying embedded fallback.");

            Rml::ElementDocument* fallback_document =
                g_context->LoadDocumentFromMemory(
                    fallback_ui,
                    "[embedded-ultron-fallback.rml]");

            if (fallback_document != nullptr)
            {
                g_documents.emplace("home", fallback_document);

                if (!SwitchToLoadedDocument("home", false))
                {
                    exit_code = 9;
                }
            }
            else
            {
                exit_code = 9;
            }
        }

        if (exit_code != 0)
        {
            StopAcceptingUITasks();

            g_documents.clear();
            g_document_history.clear();
            g_active_document = nullptr;
            g_active_document_name.clear();
            g_context_root = nullptr;
            g_context = nullptr;

            Rml::RemoveContext("main");
            Rml::Shutdown();
        }
        else
        {
            g_accept_ui_tasks.store(true, std::memory_order_release);

            SDL_Log("Ultron: GUI document initialized.");
            SDL_Log("Ultron: shared click connector initialized.");

            // -------------------------------------------------
            // Main frame loop
            // -------------------------------------------------

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
                        g_context,
                        window,
                        event);
                }

                if (!g_running.load(std::memory_order_relaxed))
                {
                    break;
                }

                ProcessPendingUITasks();

                if (g_paused.load(std::memory_order_relaxed))
                {
                    SDL_Delay(16);
                    continue;
                }

                // Keep the viewport synchronized with drawable size.
                int new_width = 0;
                int new_height = 0;

                if (SDL_GetWindowSizeInPixels(
                        window,
                        &new_width,
                        &new_height) &&
                    new_width > 0 &&
                    new_height > 0)
                {
                    if (new_width != width || new_height != height)
                    {
                        width = new_width;
                        height = new_height;

                        g_context->SetDimensions(
                            Rml::Vector2i(width, height));

                        render_interface.SetViewport(width, height);
                    }
                }

                const float new_scale = SDL_GetWindowDisplayScale(window);

                if (new_scale > 0.0f && new_scale != window_scale)
                {
                    window_scale = new_scale;

                    g_context->SetDensityIndependentPixelRatio(
                        window_scale);
                }

                glDisable(GL_SCISSOR_TEST);
                glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
                glClear(GL_COLOR_BUFFER_BIT);

                g_context->Update();

                render_interface.BeginFrame();
                g_context->Render();
                render_interface.EndFrame();

                SDL_GL_SwapWindow(window);
            }

            // Shut down UI tasks before removing documents/context.
            StopAcceptingUITasks();

            g_documents.clear();
            g_document_history.clear();
            g_active_document = nullptr;
            g_active_document_name.clear();
            g_context_root = nullptr;
            g_context = nullptr;

            // click_listener remains alive while the context shuts down.
            Rml::RemoveContext("main");
            Rml::Shutdown();
        }
    }

    // ---------------------------------------------------------
    // OpenGL + SDL shutdown
    // ---------------------------------------------------------

    RmlGL3::Shutdown();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);
    SDL_Quit();

    g_ui_thread_id = std::thread::id{};

    SDL_Log("Ultron shutdown cleanly.");

    return exit_code;
}
