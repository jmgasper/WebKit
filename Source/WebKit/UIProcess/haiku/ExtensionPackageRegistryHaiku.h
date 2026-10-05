/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)
#include "WebExtension.h"
#include "WebExtensionContext.h"
#include "haiku/WebExtensionPackageSnapshotHaiku.h"
#include <Message.h>
#include <Messenger.h>
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/WorkQueue.h>

struct BWebKitExtensionLoadOptions;
struct BWebKitExtensionInstallation;

namespace WebKit {

// Main RunLoop owns requests and prepared WebKit objects. Only filesystem
// preparation runs on the serial worker; callbacks retain no native window.
class ExtensionPackageRegistryHaiku final : public std::enable_shared_from_this<ExtensionPackageRegistryHaiku> {
public:
    static std::shared_ptr<ExtensionPackageRegistryHaiku> create(Ref<WebExtensionController>&&, bool privateBrowsing);
    ~ExtensionPackageRegistryHaiku();
    void prepare(String&& path, const BMessenger&, uint64_t identifier);
    void cancel(uint64_t identifier);
    void discard(const String& token, const BMessenger&, uint64_t identifier);
    void load(const String& token, const BWebKitExtensionLoadOptions&, const BWebKitExtensionInstallation&, const BMessenger&, uint64_t identifier);
    void unload(const String& extensionIdentifier, const BMessenger&, uint64_t identifier);
    RefPtr<API::PageConfiguration> viewConfiguration(const String& extensionIdentifier) const;
    status_t setActionListener(const BMessenger&);
    void actions(const BMessenger& window, const BMessenger& reply, uint64_t identifier);
    void activateAction(const String& extensionIdentifier, const BMessenger& window, uint64_t loadIdentifier,
        uint64_t pageIdentifier, const BMessenger& reply, uint64_t identifier);
    // Load the action's popup hidden ahead of a click; no reply, no user gesture.
    void preloadAction(const String& extensionIdentifier, const BMessenger& window, uint64_t loadIdentifier, uint64_t pageIdentifier);
    // Private pages of contexts that share this registry's extensions.
    void setPrivateAccess(const String& extensionIdentifier, bool allowed);
    void shutdown();
    bool hasPendingPreparations() const { return m_inFlight; }

private:
    ExtensionPackageRegistryHaiku(Ref<WebExtensionController>&&, bool privateBrowsing);
    void complete(uint64_t, uint64_t operation, std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku>&&);
    static status_t reply(const BMessenger&, uint32 what, uint64_t, status_t, BMessage* = nullptr);
    void actionsDidChange();
    Ref<WebExtensionController> m_controller;
    const bool m_privateBrowsing;
    Ref<WorkQueue> m_queue;
    struct Request { BMessenger target; uint64_t operation; };
    HashMap<uint64_t, Request> m_requests;
    HashMap<String, Ref<WebExtension>> m_prepared;
    HashMap<String, Ref<WebExtensionContext>> m_loaded;
    BMessenger m_actionListener;
    unsigned m_inFlight { 0 };
    uint64_t m_nextOperation { 0 };
    bool m_shutdown { false };
};

} // namespace WebKit
#endif
