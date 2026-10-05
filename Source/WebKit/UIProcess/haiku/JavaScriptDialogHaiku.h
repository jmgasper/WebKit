/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "JavaScriptDialogRequestsHaiku.h"
#include <Button.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <Messenger.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <atomic>
#include <deque>
#include <memory>

namespace WebKit {

enum class JavaScriptDialogKindHaiku : int32 { Alert, Confirm, Prompt, BeforeUnload };
enum : uint32 {
    javaScriptDialogShowHaiku = 'jssh', javaScriptDialogClosedHaiku = 'jscl',
    javaScriptDialogDismissHaiku = 'jsdi', javaScriptDialogsCancelHaiku = 'jsca',
    javaScriptDialogAcceptHaiku = 'jsok', javaScriptDialogCancelHaiku = 'jsno'
};

// BTextControl's internal text view passes inserted text through BString,
// which cannot preserve embedded NULs. Use the native BTextView byte buffer
// directly, with the same single-line editing behavior.
class JavaScriptPromptInputHaiku final : public BTextView {
public:
    JavaScriptPromptInputHaiku() : BTextView("dialog-input")
    {
        SetWordWrap(false);
        SetDoesUndo(true);
        SetExplicitMinSize(BSize(490, 26));
        SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, 26));
    }

    void KeyDown(const char* bytes, int32 length) override
    {
        if (length == 1 && bytes[0] == B_ENTER && Window()) {
            Window()->PostMessage(javaScriptDialogAcceptHaiku);
            return;
        }
        BTextView::KeyDown(bytes, length);
    }

protected:
    void InsertText(const char* text, int32 length, int32 offset, const text_run_array* runs) override
    {
        if (length <= 0)
            return;
        std::string filtered(text, static_cast<size_t>(length));
        for (auto& byte : filtered) {
            if (byte == '\n' || byte == '\r')
                byte = ' ';
        }
        BTextView::InsertText(filtered.data(), length, offset, runs);
    }
};

class JavaScriptDialogActivityHaiku {
public:
    static uint32_t activeCount() { return s_active.load(); }
protected:
    JavaScriptDialogActivityHaiku() { ++s_active; }
    ~JavaScriptDialogActivityHaiku() { --s_active; }
private:
    static inline std::atomic<uint32_t> s_active { 0 };
};

// This base order keeps activity nonzero through the BWindow/BLooper
// destructors, which still use the native application and app_server.
class JavaScriptDialogHaiku final : private JavaScriptDialogActivityHaiku, public BWindow {
public:
    static uint32_t activeCount() { return JavaScriptDialogActivityHaiku::activeCount(); }
    JavaScriptDialogHaiku(uint64_t identifier, JavaScriptDialogKindHaiku kind,
        const std::string& origin, const std::string& topOrigin, const std::string& message, const std::string& defaultValue,
        std::shared_ptr<JavaScriptDialogRequestsHaiku> requests, BMessenger target)
        : BWindow(BRect(0, 0, 559, 359), "Web page dialog", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
            B_ASYNCHRONOUS_CONTROLS | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
        , m_identifier(identifier), m_requests(std::move(requests)), m_target(target)
    {
        auto* originView = textView("dialog-origin", origin, 76);
        auto* body = textView("dialog-message", kind == JavaScriptDialogKindHaiku::BeforeUnload
            ? "This page may contain unsaved changes. Leave this page?" : message, 140);
        auto* scroll = new BScrollView("dialog-message-scroll", body, 0, false, true, B_NO_BORDER);
        BLayoutBuilder::Group<> layout(this, B_VERTICAL, 10);
        layout.SetInsets(16).Add(new BStringView("dialog-site-label", "Website:"))
            .Add(originView);
        if (!topOrigin.empty()) {
            layout.Add(new BStringView("dialog-top-label", "Embedded in:"))
                .Add(textView("dialog-top-origin", topOrigin, 76));
        }
        layout.Add(scroll);
        if (kind == JavaScriptDialogKindHaiku::Prompt) {
            m_input = new JavaScriptPromptInputHaiku();
            m_input->SetText(defaultValue.data(), static_cast<int32>(defaultValue.size()));
            layout.Add(new BScrollView("dialog-input-border", m_input, 0, false, false, B_FANCY_BORDER));
        }
        auto* buttons = new BGroupView(B_HORIZONTAL, 8);
        BLayoutBuilder::Group<> buttonLayout(buttons);
        buttonLayout.AddGlue();
        BButton* cancel = nullptr;
        if (kind != JavaScriptDialogKindHaiku::Alert) {
            cancel = new BButton("dialog-cancel", kind == JavaScriptDialogKindHaiku::BeforeUnload ? "Stay" : "Cancel",
                new BMessage(javaScriptDialogCancelHaiku));
            buttonLayout.Add(cancel);
        }
        auto* accept = new BButton("dialog-accept", kind == JavaScriptDialogKindHaiku::BeforeUnload ? "Leave" : "OK",
            new BMessage(javaScriptDialogAcceptHaiku));
        buttonLayout.Add(accept);
        layout.Add(buttons);
        SetDefaultButton(kind == JavaScriptDialogKindHaiku::BeforeUnload ? cancel : accept);
        ResizeToPreferred();
        if (m_input) {
            m_input->MakeFocus();
            m_input->SelectAll();
        }
    }

    ~JavaScriptDialogHaiku() override { finish(false); }

    bool QuitRequested() override
    {
        finish(false);
        return true;
    }

    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
        case javaScriptDialogAcceptHaiku:
            finish(true);
            PostMessage(B_QUIT_REQUESTED);
            return;
        case javaScriptDialogCancelHaiku:
        case javaScriptDialogDismissHaiku:
            finish(false);
            PostMessage(B_QUIT_REQUESTED);
            return;
        default:
            BWindow::MessageReceived(message);
        }
    }

private:
    static BTextView* textView(const char* name, const std::string& text, float height)
    {
        auto* view = new BTextView(name);
        view->SetText(text.data(), static_cast<int32>(text.size()));
        view->MakeEditable(false);
        view->MakeSelectable(true);
        view->SetWordWrap(true);
        view->SetViewUIColor(B_PANEL_BACKGROUND_COLOR);
        view->SetExplicitMinSize(BSize(490, height));
        view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, height));
        return view;
    }

    void finish(bool accepted)
    {
        if (m_finished)
            return;
        m_finished = true;
        std::string value;
        if (accepted && m_input)
            value.assign(m_input->Text(), m_input->TextLength());
        m_requests->complete(m_identifier, accepted, std::move(value));
        BMessage closed(javaScriptDialogClosedHaiku);
        closed.AddUInt64("identifier", m_identifier);
        m_target.SendMessage(&closed, static_cast<BHandler*>(nullptr), 0);
    }

    const uint64_t m_identifier;
    const std::shared_ptr<JavaScriptDialogRequestsHaiku> m_requests;
    const BMessenger m_target;
    JavaScriptPromptInputHaiku* m_input { nullptr };
    bool m_finished { false };
};

// Owned and called only by the native BWebKitView's window looper. Dialogs have
// their own native loopers; this queue keeps one sheet visible per web view.
class JavaScriptDialogHostHaiku {
public:
    explicit JavaScriptDialogHostHaiku(std::shared_ptr<JavaScriptDialogRequestsHaiku> requests)
        : m_requests(std::move(requests)) { }
    ~JavaScriptDialogHostHaiku() { cancelAll(); }

    void show(BMessage message, BWindow* parent, BMessenger target, bool visible)
    {
        uint64 identifier;
        if (message.FindUInt64("identifier", &identifier) != B_OK || !m_requests->contains(identifier))
            return;
        m_pending.push_back(std::move(message));
        pump(parent, target, visible);
    }

    void pump(BWindow* parent, BMessenger target, bool visible)
    {
        if (!parent || !visible || m_active.IsValid())
            return;
        while (!m_pending.empty()) {
            BMessage message = std::move(m_pending.front());
            m_pending.pop_front();
            uint64 identifier;
            int32 kind;
            if (message.FindUInt64("identifier", &identifier) != B_OK || !m_requests->contains(identifier))
                continue;
            if (message.FindInt32("kind", &kind) != B_OK || kind < 0
                || kind > static_cast<int32>(JavaScriptDialogKindHaiku::BeforeUnload)) {
                m_requests->complete(identifier, false);
                continue;
            }
            auto origin = text(message, "origin");
            if (origin.empty()) {
                m_requests->complete(identifier, false);
                continue;
            }
            auto* dialog = new JavaScriptDialogHaiku(identifier, static_cast<JavaScriptDialogKindHaiku>(kind),
                origin, text(message, "topOrigin"), text(message, "message"),
                text(message, "defaultValue"), m_requests, target);
            if (dialog->AddToSubset(parent) != B_OK) {
                dialog->Quit();
                continue;
            }
            dialog->CenterIn(parent->Frame());
            m_identifier = identifier;
            m_active = BMessenger(dialog);
            dialog->Show();
            return;
        }
    }

    void didClose(uint64 identifier, BWindow* parent, BMessenger target, bool visible)
    {
        if (identifier != m_identifier)
            return;
        m_active = BMessenger();
        m_identifier = 0;
        pump(parent, target, visible);
    }

    void cancelAll()
    {
        for (const auto& message : m_pending) {
            uint64 identifier;
            if (message.FindUInt64("identifier", &identifier) == B_OK)
                m_requests->complete(identifier, false);
        }
        m_pending.clear();
        m_requests->complete(m_identifier, false);
        BMessage dismiss(javaScriptDialogDismissHaiku);
        m_active.SendMessage(&dismiss, static_cast<BHandler*>(nullptr), 0);
        m_active = BMessenger();
        m_identifier = 0;
    }

private:
    static std::string text(const BMessage& message, const char* name)
    {
        const void* bytes;
        ssize_t length;
        if (message.FindData(name, B_RAW_TYPE, &bytes, &length) == B_OK && length > 0
            && !static_cast<const char*>(bytes)[length - 1])
            return { static_cast<const char*>(bytes), static_cast<size_t>(length - 1) };
        const char* value = message.FindString(name);
        return value ? value : "";
    }

    const std::shared_ptr<JavaScriptDialogRequestsHaiku> m_requests;
    std::deque<BMessage> m_pending;
    BMessenger m_active;
    uint64_t m_identifier { 0 };
};

} // namespace WebKit
