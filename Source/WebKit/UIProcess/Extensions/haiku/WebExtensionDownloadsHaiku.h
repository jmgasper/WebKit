/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionEventListenerType.h"
#include <expected>
#include <memory>
#include <optional>
#include <wtf/HashMap.h>
#include <wtf/JSONValues.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/Vector.h>
#include <wtf/WallTime.h>
#include <wtf/WeakPtr.h>
#include <wtf/text/WTFString.h>

namespace WebCore { class ResourceRequest; }

namespace WebKit {

class DownloadRegistryHaiku;
class WebExtensionContext;
class WebExtensionController;
class WebPageProxy;
struct DownloadRegistryEventHaiku;

// Bridges the context-owned native download registry to the downloads
// extension API. It observes every download of its browser context (as other
// browsers expose them to extensions with the "downloads" permission) and keeps
// a history for the lifetime of the controller; the history is not persisted.
// Confined to the WTF main RunLoop.
class WebExtensionDownloadsHaiku final {
    WTF_MAKE_TZONE_ALLOCATED(WebExtensionDownloadsHaiku);
public:
    explicit WebExtensionDownloadsHaiku(WebExtensionController&);
    ~WebExtensionDownloadsHaiku();

    void attach(const std::shared_ptr<DownloadRegistryHaiku>&, bool incognito);
    void shutdown();

    std::expected<uint64_t, String> start(WebExtensionContext&, WebPageProxy&, WebCore::ResourceRequest&&, const String& filename);
    std::expected<Ref<JSON::Array>, String> search(const JSON::Object& query) const;
    std::expected<Vector<uint64_t>, String> erase(const JSON::Object& query);
    std::expected<void, String> cancel(uint64_t identifier);
    std::expected<String, String> pathForShow(uint64_t identifier) const;
    String directory() const;

    // Registry notifications, delivered on the WTF main RunLoop.
    void observe(const DownloadRegistryEventHaiku&);

private:
    struct Item {
        uint64_t identifier { 0 };
        uint64_t order { 0 };
        String url;
        String finalURL;
        String filename;
        String mimeType;
        WallTime startTime;
        std::optional<WallTime> endTime;
        String state;
        String error;
        uint64_t bytesReceived { 0 };
        int64_t totalBytes { -1 };
        int64_t fileSize { -1 };
        bool exists { false };
        String byExtensionIdentifier;
        String byExtensionName;
    };
    struct Initiator {
        String identifier;
        String name;
    };

    void dispatch(WebExtensionEventListenerType, const String& argumentJSON);
    Ref<JSON::Object> toJSON(const Item&) const;
    std::expected<Vector<const Item*>, String> query(const JSON::Object&) const;

    WeakPtr<WebExtensionController> m_controller;
    std::weak_ptr<DownloadRegistryHaiku> m_registry;
    bool m_incognito { false };
    uint64_t m_nextOrder { 0 };
    HashMap<uint64_t, Item> m_items;
    HashMap<uint64_t, Initiator> m_initiators;
};

} // namespace WebKit

#endif
