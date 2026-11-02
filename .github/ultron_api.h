#pragma once

// Public C++20 API shared by main.cpp and independently authored feature modules.
// This is a C++ header (not a C-compatible header) because callbacks expose SDL3
// and RmlUi event types.

#include <SDL3/SDL.h>
#include <RmlUi/Core/Event.h>

#include <cstdint>

using UltronActionCallback = void (*)();
using UltronUITaskCallback = void (*)(void* user_data);

// Return true from an SDL callback to consume the event and prevent the normal
// RmlUi SDL input handler from receiving it. Return false to observe/pass it on.
using UltronEventCallback = bool (*)(const SDL_Event* event, void* user_data);

// Return true from an RmlUi callback to stop propagation/default handling.
// The Rml::Event pointer is valid only for the duration of the callback.
using UltronRmlEventCallback = bool (*)(Rml::Event* event, void* user_data);

// Register for every SDL event type. Otherwise register for a specific SDL type.
inline constexpr Uint32 ULTRON_EVENT_ANY = 0xFFFFFFFFu;

extern "C"
{
    bool Ultron_RegisterAction(
        const char* name,
        UltronActionCallback callback);

    bool Ultron_UnregisterAction(
        const char* name);

    // Register raw SDL3 events. The same module name may register multiple types.
    // Higher priority handlers run first. A true callback result consumes the event.
    bool Ultron_RegisterEventHandler(
        const char* name,
        Uint32 event_type,
        UltronEventCallback callback,
        void* user_data = nullptr,
        int priority = 0);

    // Register RmlUi element/document events such as click, dblclick, drag,
    // keydown, focus, transitionend, or an application-defined RmlUi event.
    // Event names are written without the RML 'on' prefix.
    bool Ultron_RegisterRmlEventHandler(
        const char* name,
        const char* event_name,
        UltronRmlEventCallback callback,
        void* user_data = nullptr,
        int priority = 0);

    // Removes all SDL and RmlUi event handlers registered under this module name.
    bool Ultron_UnregisterEventHandlers(
        const char* name);

    // Allocate one SDL user-event type. Call after SDL has initialized.
    // Returns 0 on failure.
    Uint32 Ultron_RegisterCustomEventType();

    bool Ultron_ShowDocument(
        const char* document_name,
        const char* rml_path);

    bool Ultron_GoBack();

    bool Ultron_IsDocumentLoaded(
        const char* document_name);

    bool Ultron_SetElementClass(
        const char* element_id,
        const char* class_name,
        bool enabled);

    bool Ultron_SetElementText(
        const char* element_id,
        const char* text);

    bool Ultron_SetElementProperty(
        const char* element_id,
        const char* property_name,
        const char* value);

    bool Ultron_SetElementAttribute(
        const char* element_id,
        const char* attribute_name,
        const char* value);

    bool Ultron_RemoveElementAttribute(
        const char* element_id,
        const char* attribute_name);

    // Safe to call from a worker thread. user_data must remain valid until
    // the callback has executed (or the application shuts down).
    bool Ultron_PostToUI(
        UltronUITaskCallback callback,
        void* user_data);
}
