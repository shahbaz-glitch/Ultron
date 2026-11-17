#include "ultron_api.h"

#include <RmlUi/Core.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/Elements/ElementFormControlTextArea.h>
#include <RmlUi/Core/ObserverPtr.h>

#include <algorithm>
#include <cstdio>
#include <string>

namespace
{
    // ------------------------------------------------------------
    // Obsidian Wing Chat element IDs
    // ------------------------------------------------------------

    constexpr const char* kMessageInputId = "message-input";
    constexpr const char* kInputBarId     = "input-bar";
    constexpr const char* kChatAreaId     = "chat-area";
    constexpr const char* kWingHitId      = "wing-hit";

    constexpr const char* kMultilineClass = "multiline";


    // ------------------------------------------------------------
    // Original RCSS dimensions
    //
    // Portrait: vw
    // Landscape: vh
    //
    // wing-hit provides the reference dimension of 14.22 units.
    // ------------------------------------------------------------

    constexpr float kReferenceSize = 14.22f;

    constexpr float kBaseInputHeight = 7.35f;

    constexpr float kInputPaddingTop = 0.74f;
    constexpr float kInputPaddingBottom = 0.49f;

    constexpr float kMaximumInputHeight = 28.92f;

    constexpr float kChatGap = 11.52f;


    // ------------------------------------------------------------
    // Safe element reference
    // ------------------------------------------------------------

    Rml::ObserverPtr<Rml::Element> g_messageInput;

    bool g_layoutTaskQueued = false;


    // ------------------------------------------------------------
    // Forward declarations
    // ------------------------------------------------------------

    void ApplyInputLayout(void* userData);

    void QueueInputLayout();


    // ------------------------------------------------------------
    // Convert pixel value to RCSS-compatible string
    // ------------------------------------------------------------

    std::string ToPixels(float value)
    {
        char buffer[64];

        std::snprintf(
            buffer,
            sizeof(buffer),
            "%.3fpx",
            static_cast<double>(std::max(value, 0.0f))
        );

        return buffer;
    }


    // ------------------------------------------------------------
    // Find the chat elements and cache the textarea safely
    // ------------------------------------------------------------

    void BindChatElements(Rml::Element* root)
    {
        if (!root)
            return;

        Rml::Element* input =
            root->GetElementById(kMessageInputId);

        Rml::Element* bar =
            root->GetElementById(kInputBarId);

        Rml::Element* chat =
            root->GetElementById(kChatAreaId);

        Rml::Element* wing =
            root->GetElementById(kWingHitId);

        if (!input || !bar || !chat || !wing)
            return;

        g_messageInput = input->GetObserverPtr();

        QueueInputLayout();
    }


    // ------------------------------------------------------------
    // Queue layout update on the UI thread
    //
    // This lets the current RmlUi event finish its default action
    // before the new textarea value is measured.
    // ------------------------------------------------------------

    void QueueInputLayout()
    {
        if (!g_messageInput || g_layoutTaskQueued)
            return;

        g_layoutTaskQueued = true;

        Ultron_PostToUI(
            &ApplyInputLayout,
            nullptr
        );
    }


    // ------------------------------------------------------------
    // Dynamic Input Height
    // First-Line Shift
    // Responsive Multiline
    // Dynamic Chat Position
    // ------------------------------------------------------------

    void ApplyInputLayout(void* userData)
    {
        g_layoutTaskQueued = false;

        if (!g_messageInput)
            return;

        Rml::Element* inputElement =
            g_messageInput.operator->();

        if (!inputElement)
            return;

        Rml::ElementDocument* document =
            inputElement->GetOwnerDocument();

        if (!document)
            return;

        auto* textArea =
            static_cast<Rml::ElementFormControlTextArea*>(
                inputElement
            );

        Rml::Element* inputBar =
            document->GetElementById(kInputBarId);

        Rml::Element* chatArea =
            document->GetElementById(kChatAreaId);

        Rml::Element* wingHit =
            document->GetElementById(kWingHitId);

        if (!inputBar || !chatArea || !wingHit)
            return;


        // --------------------------------------------------------
        // 1. Get the original responsive reference size
        // --------------------------------------------------------

        const float controlHeight =
            wingHit->GetOffsetHeight();

        if (controlHeight <= 0.0f)
            return;


        // --------------------------------------------------------
        // 2. Convert original RCSS dimensions into pixels
        // --------------------------------------------------------

        const float unit =
            controlHeight / kReferenceSize;

        const float baseTextHeight =
            unit * kBaseInputHeight;

        const float verticalPadding =
            unit * (
                kInputPaddingTop +
                kInputPaddingBottom
            );

        const float maximumTextHeight =
            unit * kMaximumInputHeight;

        const float chatGap =
            unit * kChatGap;


        // --------------------------------------------------------
        // 3. Detect first-line wrapping and manual newlines
        // --------------------------------------------------------

        const Rml::String value =
            textArea->GetValue();

        const float scrollHeight =
            textArea->GetScrollHeight();

        const float clientHeight =
            textArea->GetClientHeight();

        const bool hasText =
            !value.empty();

        const bool hasManualNewLine =
            value.find_first_of("\r\n") != Rml::String::npos;

        const bool hasWrappedLine =
            scrollHeight > clientHeight + 1.0f;

        const bool wasMultiline =
            inputBar->IsClassSet(kMultilineClass);


        // Once multiline mode starts, keep its wider textarea
        // until the input becomes empty. This prevents class
        // switching back and forth during the first-line shift.

        const bool multiline =
            hasText &&
            (
                wasMultiline ||
                hasWrappedLine ||
                hasManualNewLine
            );


        // --------------------------------------------------------
        // 4. First-Line Shift
        // --------------------------------------------------------

        inputBar->SetClass(
            kMultilineClass,
            multiline
        );


        // --------------------------------------------------------
        // 5. Calculate the natural content height
        //
        // GetScrollHeight includes textarea padding.
        // Remove padding to obtain the content height.
        // --------------------------------------------------------

        const float updatedScrollHeight =
            textArea->GetScrollHeight();

        const float naturalTextHeight =
            std::max(
                baseTextHeight,
                updatedScrollHeight - verticalPadding
            );


        // --------------------------------------------------------
        // 6. Responsive Multiline
        //
        // Grow line-by-line.
        // Stop growing at the existing 28.92vw/vh maximum.
        // --------------------------------------------------------

        const float targetTextHeight =
            multiline
                ? std::clamp(
                    naturalTextHeight,
                    baseTextHeight,
                    maximumTextHeight
                  )
                : baseTextHeight;


        // --------------------------------------------------------
        // 7. Dynamic Input Height
        //
        // Keep the original input bar height as the baseline.
        // Grow only by the textarea's additional content height.
        // --------------------------------------------------------

        const float additionalTextHeight =
            targetTextHeight - baseTextHeight;

        const float targetBarHeight =
            controlHeight + additionalTextHeight;


        // --------------------------------------------------------
        // 8. Apply textarea height
        //
        // The original RCSS max-height remains unchanged.
        // Once the maximum is reached, overflow-y:auto handles
        // additional text inside the textarea.
        // --------------------------------------------------------

        textArea->SetProperty(
            "height",
            ToPixels(targetTextHeight)
        );


        // --------------------------------------------------------
        // 9. Apply input bar height
        //
        // Its bottom position remains controlled by the original
        // RCSS, so additional height grows upward.
        // --------------------------------------------------------

        inputBar->SetProperty(
            "height",
            ToPixels(targetBarHeight)
        );


        // --------------------------------------------------------
        // 10. Dynamic Chat Position
        //
        // Preserve the original 11.52 / 14.22 spacing ratio.
        // --------------------------------------------------------

        chatArea->SetProperty(
            "bottom",
            ToPixels(targetBarHeight + chatGap)
        );
    }


    // ------------------------------------------------------------
    // Input events
    //
    // Registered on the context root by Ultron's event bridge.
    // Only events originating from #message-input are processed.
    // ------------------------------------------------------------

    bool OnInputEvent(
        Rml::Event* event,
        void* userData
    )
    {
        if (!event)
            return false;

        Rml::Element* target =
            event->GetTargetElement();

        if (!target)
            return false;

        Rml::ElementDocument* document =
            target->GetOwnerDocument();

        if (!document)
            return false;

        Rml::Element* messageInput =
            document->GetElementById(kMessageInputId);

        if (!messageInput || target != messageInput)
            return false;

        BindChatElements(document);

        return false;
    }


    // ------------------------------------------------------------
    // Document events
    //
    // Used when the document becomes visible or its viewport
    // dimensions change.
    // ------------------------------------------------------------

    bool OnDocumentEvent(
        Rml::Event* event,
        void* userData
    )
    {
        if (!event)
            return false;

        Rml::Element* target =
            event->GetTargetElement();

        if (!target)
            return false;

        BindChatElements(target);

        return false;
    }


    // ------------------------------------------------------------
    // Register all behaviour handlers
    // ------------------------------------------------------------

    struct ChatBehaviourRegistration
    {
        ChatBehaviourRegistration()
        {
            constexpr int priority = 100;

            // Textarea focus and editing events.

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-focus",
                "focus",
                &OnInputEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-blur",
                "blur",
                &OnInputEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-textinput",
                "textinput",
                &OnInputEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-keydown",
                "keydown",
                &OnInputEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-keyup",
                "keyup",
                &OnInputEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-change",
                "change",
                &OnInputEvent,
                nullptr,
                priority
            );


            // Document lifecycle and responsive layout events.

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-show",
                "show",
                &OnDocumentEvent,
                nullptr,
                priority
            );

            Ultron_RegisterRmlEventHandler(
                "obsidian-chat-resize",
                "resize",
                &OnDocumentEvent,
                nullptr,
                priority
            );
        }
    };


    // Static registration: no manual call from main.cpp.

    ChatBehaviourRegistration g_chatBehaviourRegistration;

} // namespace
