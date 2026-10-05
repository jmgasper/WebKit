/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "OpenPanelHaiku.h"

#include "APIArray.h"
#include "APIOpenPanelParameters.h"
#include "APIString.h"
#include "WebOpenPanelResultListenerProxy.h"
#include <Application.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Handler.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <memory>
// struct stat_beos, which BRefFilter::Filter() receives.
#include <compat/sys/stat.h>
#include <wtf/RunLoop.h>

namespace WebKit {

namespace {

Vector<String> lowercaseStringsHaiku(API::Array& array)
{
    Vector<String> strings;
    for (auto& element : array.elements()) {
        if (auto* string = dynamicDowncast<API::String>(element.get()); string && !string->string().isEmpty())
            strings.append(string->string().convertToASCIILowercase());
    }
    return strings;
}

// Shows folders, and files matching the accept list: extensions (".csv") or
// MIME types, exact or by major type ("image/*"). No list shows everything.
class AcceptFilterHaiku final : public BRefFilter {
public:
    AcceptFilterHaiku(Vector<String>&& mimeTypes, Vector<String>&& extensions)
        : m_mimeTypes(WTF::move(mimeTypes)), m_extensions(WTF::move(extensions)) { }

    bool Filter(const entry_ref* ref, BNode*, struct stat_beos* stat, const char* mimeType) override
    {
        if (!stat || S_ISDIR(stat->st_mode) || S_ISLNK(stat->st_mode))
            return true;
        if (m_mimeTypes.isEmpty() && m_extensions.isEmpty())
            return true;
        String name = String::fromUTF8(ref->name).convertToASCIILowercase();
        for (auto& extension : m_extensions) {
            if (name.endsWith(extension.startsWith('.') ? extension : makeString('.', extension)))
                return true;
        }
        String type = String::fromUTF8(mimeType ? mimeType : "").convertToASCIILowercase();
        for (auto& accepted : m_mimeTypes) {
            if (accepted.endsWith("/*"_s) ? type.startsWith(accepted.left(accepted.length() - 1)) : type == accepted)
                return true;
        }
        return false;
    }

private:
    const Vector<String> m_mimeTypes;
    const Vector<String> m_extensions;
};

// Receives the panel's answer on the application looper, which is the WebKit
// main thread, and deletes itself (and the panel) once the listener has it.
class OpenPanelHandlerHaiku final : public BHandler {
public:
    OpenPanelHandlerHaiku(Ref<WebOpenPanelResultListenerProxy>&& listener, std::unique_ptr<BRefFilter>&& filter)
        : BHandler("WebKit open panel")
        , m_listener(WTF::move(listener))
        , m_filter(std::move(filter))
    {
    }

    bool show(bool directories, bool multiple)
    {
        BMessenger target(this);
        if (!target.IsValid())
            return false;
        m_panel = std::make_unique<BFilePanel>(B_OPEN_PANEL, &target, nullptr,
            directories ? B_DIRECTORY_NODE : B_FILE_NODE, multiple, nullptr, m_filter.get(), true, true);
        m_panel->Show();
        return true;
    }

    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
        case B_REFS_RECEIVED: {
            Vector<String> paths;
            entry_ref ref;
            for (int32 index = 0; message->FindRef("refs", index, &ref) == B_OK; ++index) {
                BPath path(&ref);
                if (path.InitCheck() == B_OK)
                    paths.append(String::fromUTF8(path.Path()));
            }
            finish(WTF::move(paths));
            break;
        }
        case B_CANCEL:
            // Also sent when a panel that already answered is hidden.
            finish({ });
            break;
        default:
            BHandler::MessageReceived(message);
        }
    }

private:
    void finish(Vector<String>&& paths)
    {
        if (!m_listener)
            return;
        RefPtr listener = std::exchange(m_listener, nullptr);
        WTF::RunLoop::mainSingleton().dispatch([listener = WTF::move(listener), paths = WTF::move(paths), handler = this] {
            if (paths.isEmpty())
                listener->cancel();
            else
                listener->chooseFiles(paths);
            // The panel's window is its own looper; closing it from here is safe.
            if (be_app && be_app->LockLooper()) {
                be_app->RemoveHandler(handler);
                be_app->UnlockLooper();
            }
            delete handler;
        });
    }

    RefPtr<WebOpenPanelResultListenerProxy> m_listener;
    std::unique_ptr<BRefFilter> m_filter;
    std::unique_ptr<BFilePanel> m_panel;
};

} // namespace

bool runOpenPanelHaiku(API::OpenPanelParameters* parameters, WebOpenPanelResultListenerProxy* listener)
{
    if (!parameters || !listener || !be_app)
        return false;
    auto filter = std::make_unique<AcceptFilterHaiku>(
        lowercaseStringsHaiku(parameters->acceptMIMETypes().get()),
        lowercaseStringsHaiku(parameters->acceptFileExtensions().get()));
    auto* handler = new OpenPanelHandlerHaiku(Ref { *listener }, std::move(filter));
    if (!be_app->LockLooper()) {
        delete handler;
        return false;
    }
    be_app->AddHandler(handler);
    be_app->UnlockLooper();
    if (!handler->show(parameters->allowDirectories(), parameters->allowMultipleFiles())) {
        if (be_app->LockLooper()) {
            be_app->RemoveHandler(handler);
            be_app->UnlockLooper();
        }
        delete handler;
        return false;
    }
    return true;
}

} // namespace WebKit
