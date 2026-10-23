#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_filesystem.h>

#include <GLES3/gl3.h>

#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Event.h>
#include <RmlUi/Core/EventListener.h>
#include <RmlUi/Core/FileInterface.h>
#include <RmlUi/Debugger.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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

// Correct C++ function-pointer aliases.
using UltronActionCallback = void (*)();
using UltronUITaskCallback = void (*)(void* user_data);

// -------------------------------------------------------------
// Public native APIs
// -------------------------------------------------------------

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

// -------------------------------------------------------------
// Application state
// -------------------------------------------------------------

constexpr const char* kUiDirectory = "ui";

constexpr const char* kFallbackDocumentName =
    "__ultron_internal_black_fallback__";

std::atomic_bool g_running{true};
std::atomic_bool g_paused{false};
std::atomic_bool g_accept_ui_tasks{false};

// UI objects are accessed only from the main/UI thread.
Rml::Context* g_context = nullptr;
Rml::Element* g_context_root = nullptr;
Rml::ElementDocument* g_active_document = nullptr;

std::unordered_map<std::string, Rml::ElementDocument*> g_documents;
std::string g_active_document_name;
std::vector<std::string> g_document_history;
std::thread::id g_ui_thread_id{};

// -------------------------------------------------------------
// Native action registry
// -------------------------------------------------------------

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

    // Do not hold the registry mutex while running user code.
    callback();
}

// -------------------------------------------------------------
// UI-thread task queue
// -------------------------------------------------------------

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

    // Discard remaining tasks during shutdown.
    // Ownership of each user_data pointer remains with its caller.
    std::queue<PendingUITask> empty;
    queue.tasks.swap(empty);
}

// -------------------------------------------------------------
// SDL3-backed RmlUi file interface
// -------------------------------------------------------------

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
        SDL_IOStream* stream = SDL_IOFromFile(path.c_str(), "rb");

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

// -------------------------------------------------------------
// Shared click connector
// -------------------------------------------------------------

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

        // Support clicks on nested text, icons and child elements.
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

// -------------------------------------------------------------
// Automatic RML loader helpers
// -------------------------------------------------------------

bool StartsWith(
    const std::string& value,
    const std::string& prefix)
{
    return value.size() >= prefix.size() &&
           value.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWithRml(const std::string& path)
{
    if (path.size() < 4)
    {
        return false;
    }

    const size_t offset = path.size() - 4;

    return std::tolower(
               static_cast<unsigned char>(path[offset])) == '.' &&
           std::tolower(
               static_cast<unsigned char>(path[offset + 1])) == 'r' &&
           std::tolower(
               static_cast<unsigned char>(path[offset + 2])) == 'm' &&
           std::tolower(
               static_cast<unsigned char>(path[offset + 3])) == 'l';
}

std::string NormalizeSlashes(std::string path)
{
    for (char& character : path)
    {
        if (character == '\\')
        {
            character = '/';
        }
    }

    return path;
}

// Convert a result returned by SDL_GlobDirectory into a path
// that SDL/RmlUi can open.
std::string GetRmlLoadPath(const char* discovered_path)
{
    if (discovered_path == nullptr || discovered_path[0] == '\0')
    {
        return {};
    }

    std::string path = NormalizeSlashes(discovered_path);

    while (StartsWith(path, "./"))
    {
        path.erase(0, 2);
    }

    // Keep full paths and SDL asset paths as supplied.
    if (StartsWith(path, "assets://") ||
        (!path.empty() && path[0] == '/'))
    {
        return path;
    }

    // Some directory enumerators include the directory in the result.
    if (StartsWith(path, "ui/"))
    {
        return path;
    }

    return std::string(kUiDirectory) + "/" + path;
}

// Generate a navigation name from the RML path.
//
// Examples:
//   ui/home.rml           -> home
//   ui/settings.rml       -> settings
//   ui/agent/profile.rml  -> agent/profile
std::string GetDocumentNameFromPath(const std::string& load_path)
{
    std::string path = NormalizeSlashes(load_path);

    if (StartsWith(path, "assets://"))
    {
        path.erase(0, 9);
    }

    while (StartsWith(path, "./"))
    {
        path.erase(0, 2);
    }

    if (StartsWith(path, "ui/"))
    {
        path.erase(0, 3);
    }
    else
    {
        // Support an absolute path whose final directory is "ui".
        const size_t marker = path.find("/ui/");

        if (marker != std::string::npos)
        {
            path.erase(0, marker + 4);
        }
    }

    if (EndsWithRml(path))
    {
        path.resize(path.size() - 4);
    }

    return path;
}

struct DiscoveredDocument
{
    std::string name;
    std::string path;
};

// Discover and load every RML file found inside ui/.
//
// The resulting document names are derived from the file paths.
// No screen name is hardcoded as the default.
size_t AutoLoadDocumentsFromUI(std::string& first_document_name)
{
    first_document_name.clear();

    int file_count = 0;

    char** discovered_files = SDL_GlobDirectory(
        kUiDirectory,
        "*.rml",
        SDL_GLOB_CASEINSENSITIVE,
        &file_count);

    if (discovered_files == nullptr)
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: unable to scan '%s' for RML files: %s",
            kUiDirectory,
            SDL_GetError());

        SDL_ClearError();
        return 0;
    }

    std::vector<DiscoveredDocument> candidates;
    candidates.reserve(
        file_count > 0 ? static_cast<size_t>(file_count) : 0);

    for (int index = 0; index < file_count; ++index)
    {
        if (discovered_files[index] == nullptr)
        {
            continue;
        }

        std::string path =
            GetRmlLoadPath(discovered_files[index]);

        if (path.empty() || !EndsWithRml(path))
        {
            continue;
        }

        std::string name = GetDocumentNameFromPath(path);

        if (name.empty())
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: skipped RML file with an empty document name.");

            continue;
        }

        // Keep the fallback name reserved for the internal document.
        if (name == kFallbackDocumentName)
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: skipped reserved document name '%s'.",
                name.c_str());

            continue;
        }

        candidates.push_back(
            DiscoveredDocument{std::move(name), std::move(path)});
    }

    // Make startup deterministic across runs and devices.
    std::sort(
        candidates.begin(),
        candidates.end(),
        [](const DiscoveredDocument& left,
           const DiscoveredDocument& right)
        {
            return left.path < right.path;
        });

    size_t loaded_count = 0;

    for (const DiscoveredDocument& candidate : candidates)
    {
        if (g_documents.find(candidate.name) != g_documents.end())
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: duplicate screen name '%s'; skipped '%s'.",
                candidate.name.c_str(),
                candidate.path.c_str());

            continue;
        }

        Rml::ElementDocument* document =
            g_context->LoadDocument(candidate.path.c_str());

        if (document == nullptr)
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: failed to load screen '%s' from '%s'.",
                candidate.name.c_str(),
                candidate.path.c_str());

            continue;
        }

        // Loaded screens are registered but not all shown at once.
        document->Hide();

        g_documents.emplace(candidate.name, document);

        if (first_document_name.empty())
        {
            first_document_name = candidate.name;
        }

        ++loaded_count;

        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: registered screen '%s' from '%s'.",
            candidate.name.c_str(),
            candidate.path.c_str());
    }

    SDL_free(discovered_files);

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: automatic loader registered %zu screen(s).",
        loaded_count);

    return loaded_count;
}

// -------------------------------------------------------------
// Multiple-document navigation
// -------------------------------------------------------------

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
    if (!RequireUIThread("Ultron_ShowDocument") || g_context == nullptr)
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
        // Runtime loading remains available for screens not discovered
        // during startup. The automatic loader itself does not need it.
        if (rml_path == nullptr || rml_path[0] == '\0')
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: screen '%s' is not loaded and no RML path was supplied.",
                document_name);

            return false;
        }

        Rml::ElementDocument* document =
            g_context->LoadDocument(rml_path);

        if (document == nullptr)
        {
            SDL_LogWarn(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron: failed to load screen '%s' from '%s'.",
                document_name,
                rml_path);

            return false;
        }

        document->Hide();
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
            case '&': escaped += "&amp;"; break;
            case '<': escaped += "&lt;"; break;
            case '>': escaped += "&gt;"; break;
            default:  escaped += character; break;
        }
    }

    return escaped;
}

// -------------------------------------------------------------
// Embedded black fallback
// -------------------------------------------------------------

bool ShowBlackFallback()
{
    if (g_context == nullptr)
    {
        return false;
    }

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

    Rml::ElementDocument* fallback_document =
        g_context->LoadDocumentFromMemory(
            fallback_ui,
            "[embedded-ultron-black-fallback.rml]");

    if (fallback_document == nullptr)
    {
        SDL_LogError(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: unable to create embedded black fallback.");

        return false;
    }

    fallback_document->Hide();

    g_documents.emplace(
        kFallbackDocumentName,
        fallback_document);

    SDL_LogWarn(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: no valid UI screen found; showing embedded black fallback.");

    return SwitchToLoadedDocument(
        kFallbackDocumentName,
        false);
}

// -------------------------------------------------------------
// Android / SDL lifecycle events
// -------------------------------------------------------------

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
            SDL_Log(
                "Ultron: Android low-memory notification received.");
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

// -------------------------------------------------------------
// Window and OpenGL context creation with MSAA fallback
// -------------------------------------------------------------

bool CreateWindowAndContext(
    SDL_PropertiesID properties,
    int requested_samples,
    SDL_Window*& window,
    SDL_GLContext& gl_context)
{
    const int buffer_count = requested_samples > 0 ? 1 : 0;

    if (!SDL_GL_SetAttribute(
            SDL_GL_MULTISAMPLEBUFFERS,
            buffer_count) ||
        !SDL_GL_SetAttribute(
            SDL_GL_MULTISAMPLESAMPLES,
            requested_samples))
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: unable to set MSAA attributes (%d samples): %s",
            requested_samples,
            SDL_GetError());
    }

    window = SDL_CreateWindowWithProperties(properties);

    if (window == nullptr)
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: window creation failed with %d MSAA samples: %s",
            requested_samples,
            SDL_GetError());

        return false;
    }

    gl_context = SDL_GL_CreateContext(window);

    if (gl_context == nullptr)
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: GL context creation failed with %d MSAA samples: %s",
            requested_samples,
            SDL_GetError());

        SDL_DestroyWindow(window);
        window = nullptr;

        return false;
    }

    if (!SDL_GL_MakeCurrent(window, gl_context))
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: unable to make GL context current with %d MSAA samples: %s",
            requested_samples,
            SDL_GetError());

        SDL_GL_DestroyContext(gl_context);
        SDL_DestroyWindow(window);

        gl_context = nullptr;
        window = nullptr;

        return false;
    }

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: created GL window with requested MSAA samples=%d.",
        requested_samples);

    return true;
}

} // namespace

// -------------------------------------------------------------
// Native action registration API
// -------------------------------------------------------------

extern "C" bool Ultron_RegisterAction(
    const char* name,
    UltronActionCallback callback)
{
    if (name == nullptr || name[0] == '\0' || callback == nullptr)
    {
        return false;
    }

    std::lock_guard lock(GetActionRegistryMutex());

    return GetActionRegistry().emplace(name, callback).second;
}

// -------------------------------------------------------------
// Multiple-screen API
// -------------------------------------------------------------

extern "C" bool Ultron_ShowDocument(
    const char* document_name,
    const char* rml_path)
{
    return ShowDocumentInternal(
        document_name,
        rml_path,
        true);
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

extern "C" bool Ultron_IsDocumentLoaded(
    const char* document_name)
{
    if (!RequireUIThread("Ultron_IsDocumentLoaded"))
    {
        return false;
    }

    return FindDocument(document_name) != nullptr;
}

// -------------------------------------------------------------
// GUI manipulation API
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

    // SetInnerRML interprets markup, so escape ordinary text.
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

    element->SetAttribute(
        attribute_name,
        Rml::String(value));

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

    // Recheck under lock to avoid accepting tasks during shutdown.
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

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS))
    {
        return Fail(
            nullptr,
            nullptr,
            1,
            "SDL_Init failed");
    }

    SDL_SetEventFilter(HandleAppEvents, nullptr);
    SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");

    // ---------------------------------------------------------
    // OpenGL ES 3.2 request
    // ---------------------------------------------------------

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);

    SDL_GL_SetAttribute(
        SDL_GL_CONTEXT_PROFILE_MASK,
        SDL_GL_CONTEXT_PROFILE_ES);

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    // ---------------------------------------------------------
    // Window properties
    // ---------------------------------------------------------

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

    // Try 4x MSAA, then 2x, then no window-framebuffer MSAA.
    SDL_Window* window = nullptr;
    SDL_GLContext gl_context = nullptr;

    constexpr int msaa_attempts[] = {4, 2, 0};
    bool gl_ready = false;

    for (const int samples : msaa_attempts)
    {
        if (CreateWindowAndContext(
                properties,
                samples,
                window,
                gl_context))
        {
            gl_ready = true;
            break;
        }

        SDL_ClearError();
    }

    SDL_DestroyProperties(properties);

    if (!gl_ready)
    {
        return Fail(
            nullptr,
            nullptr,
            3,
            "Unable to create an SDL window and OpenGL ES context, even without MSAA");
    }

    if (!SDL_GL_SetSwapInterval(1))
    {
        SDL_LogWarn(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: VSync could not be enabled: %s",
            SDL_GetError());

        SDL_ClearError();
    }

    int actual_msaa_buffers = 0;
    int actual_msaa_samples = 0;

    const bool have_msaa_info =
        SDL_GL_GetAttribute(
            SDL_GL_MULTISAMPLEBUFFERS,
            &actual_msaa_buffers) &&
        SDL_GL_GetAttribute(
            SDL_GL_MULTISAMPLESAMPLES,
            &actual_msaa_samples);

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: window framebuffer MSAA: %s",
        have_msaa_info
            ? (actual_msaa_samples > 0 ? "enabled" : "not available")
            : "query unavailable");

    if (have_msaa_info)
    {
        SDL_LogInfo(
            SDL_LOG_CATEGORY_APPLICATION,
            "Ultron: window framebuffer reports %d buffers / %d samples.",
            actual_msaa_buffers,
            actual_msaa_samples);
    }

    const GLubyte* gl_version = glGetString(GL_VERSION);
    const GLubyte* gl_renderer = glGetString(GL_RENDERER);

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: OpenGL version: %s",
        gl_version != nullptr
            ? reinterpret_cast<const char*>(gl_version)
            : "unknown");

    SDL_LogInfo(
        SDL_LOG_CATEGORY_APPLICATION,
        "Ultron: GPU renderer: %s",
        gl_renderer != nullptr
            ? reinterpret_cast<const char*>(gl_renderer)
            : "unknown");

    glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    SDL_GL_SwapWindow(window);

    SDL_Log("Ultron: OpenGL ES context created.");

    // ---------------------------------------------------------
    // RmlUi renderer initialization
    // ---------------------------------------------------------

    if (!RmlGL3::Initialize())
    {
        return Fail(
            window,
            gl_context,
            6,
            "RmlGL3::Initialize failed");
    }

    int exit_code = 0;

    {
        SystemInterface_SDL system_interface(window);
        RenderInterface_GL3 render_interface;
        SDLFileInterface file_interface;

        int width = 0;
        int height = 0;

        if (!SDL_GetWindowSizeInPixels(window, &width, &height) ||
            width < 1 ||
            height < 1)
        {
            width = initial_width;
            height = initial_height;
        }

        render_interface.SetViewport(width, height);

        Rml::SetSystemInterface(&system_interface);
        Rml::SetRenderInterface(&render_interface);
        Rml::SetFileInterface(&file_interface);

        const bool rml_initialized = Rml::Initialise();

        if (!rml_initialized)
        {
            SDL_LogError(
                SDL_LOG_CATEGORY_APPLICATION,
                "Ultron startup failure: Rml::Initialise failed.");

            exit_code = 7;
        }
        else
        {
            g_context = Rml::CreateContext(
                "main",
                Rml::Vector2i(width, height));

            if (g_context == nullptr)
            {
                SDL_LogError(
                    SDL_LOG_CATEGORY_APPLICATION,
                    "Ultron startup failure: Rml::CreateContext failed.");

                Rml::Shutdown();
                exit_code = 8;
            }
            else
            {
                g_context_root = g_context->GetRootElement();

                // -------------------------------------------------
                // Visual Debugger initialization
                // -------------------------------------------------
                //
                // Requires the RmlUi debugger target to be linked
                // in CMakeLists.txt.
                //
                // The debugger starts hidden. Press F8 on a keyboard
                // to show or hide it.
                //
                // If initialization fails, the regular UI continues.
                // -------------------------------------------------

                const bool debugger_initialized =
                    Rml::Debugger::Initialise(g_context);

                if (debugger_initialized)
                {
                    SDL_LogInfo(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "Ultron: Visual Debugger initialized. "
                        "Press F8 to toggle visibility.");
                }
                else
                {
                    SDL_LogWarn(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "Ultron: Visual Debugger initialization failed; "
                        "continuing without the debugger.");
                }

                float window_scale =
                    SDL_GetWindowDisplayScale(window);

                if (window_scale > 0.0f)
                {
                    g_context->SetDensityIndependentPixelRatio(
                        window_scale);
                }

                // A single click listener services all screens.
                ClickActionListener click_listener(g_context_root);

                if (g_context_root != nullptr)
                {
                    g_context_root->AddEventListener(
                        "click",
                        &click_listener,
                        true);
                }

                // -------------------------------------------------
                // Automatic loader startup
                // -------------------------------------------------
                //
                // There is no mandatory home.rml.
                // Every valid .rml found inside ui/ is registered.
                // The first valid screen after sorting its path is
                // opened, regardless of its filename.
                //
                // If no valid RML document can be loaded, the
                // embedded black fallback is opened instead.
                // -------------------------------------------------

                std::string startup_document_name;

                AutoLoadDocumentsFromUI(startup_document_name);

                bool startup_ready = false;

                if (!startup_document_name.empty())
                {
                    startup_ready = SwitchToLoadedDocument(
                        startup_document_name,
                        false);
                }

                if (!startup_ready)
                {
                    startup_ready = ShowBlackFallback();
                }

                if (!startup_ready)
                {
                    SDL_LogError(
                        SDL_LOG_CATEGORY_APPLICATION,
                        "Ultron startup failure: unable to show any screen or the black fallback.");

                    exit_code = 9;
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

                    // Rml::Shutdown() automatically shuts down the
                    // debugger plugin if it was initialized.
                    Rml::RemoveContext("main");
                    Rml::Shutdown();
                }
                else
                {
                    g_accept_ui_tasks.store(
                        true,
                        std::memory_order_release);

                    SDL_Log(
                        "Ultron: automatic RML loader initialized.");

                    SDL_Log(
                        "Ultron: shared click connector initialized.");

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

                            // Toggle the Visual Debugger with F8.
                            // Consume this key event so the regular UI
                            // does not also process it.
                            if (debugger_initialized &&
                                event.type == SDL_EVENT_KEY_DOWN &&
                                event.key.key == SDLK_F8 &&
                                !event.key.repeat)
                            {
                                const bool make_visible =
                                    !Rml::Debugger::IsVisible();

                                Rml::Debugger::SetVisible(make_visible);

                                SDL_LogInfo(
                                    SDL_LOG_CATEGORY_APPLICATION,
                                    "Ultron: Visual Debugger %s.",
                                    make_visible ? "shown" : "hidden");

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

                        // Keep the pixel viewport and RmlUi dimensions
                        // synchronized after a window-size change.
                        int new_width = 0;
                        int new_height = 0;

                        if (SDL_GetWindowSizeInPixels(
                                window,
                                &new_width,
                                &new_height) &&
                            new_width > 0 &&
                            new_height > 0 &&
                            (new_width != width ||
                             new_height != height))
                        {
                            width = new_width;
                            height = new_height;

                            g_context->SetDimensions(
                                Rml::Vector2i(width, height));

                            render_interface.SetViewport(
                                width,
                                height);
                        }

                        const float new_scale =
                            SDL_GetWindowDisplayScale(window);

                        if (new_scale > 0.0f &&
                            new_scale != window_scale)
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

                    StopAcceptingUITasks();

                    g_documents.clear();
                    g_document_history.clear();

                    g_active_document = nullptr;
                    g_active_document_name.clear();

                    g_context_root = nullptr;
                    g_context = nullptr;

                    // The click listener is still alive at this point.
                    // Rml::Shutdown() automatically releases the
                    // Visual Debugger plugin.
                    Rml::RemoveContext("main");
                    Rml::Shutdown();
                }
            }
        }
    }

    // Interfaces and context are shut down before the GL context.
    RmlGL3::Shutdown();

    SDL_GL_DestroyContext(gl_context);
    SDL_DestroyWindow(window);

    SDL_Quit();

    g_ui_thread_id = std::thread::id{};

    SDL_Log("Ultron shutdown cleanly.");

    return exit_code;
}
