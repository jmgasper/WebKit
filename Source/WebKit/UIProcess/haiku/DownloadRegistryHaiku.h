/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <Messenger.h>
#include <SupportDefs.h>
#include <memory>
#include <unordered_map>
#include <wtf/CompletionHandler.h>
#include <wtf/Function.h>
#include <wtf/Ref.h>
#include <wtf/text/WTFString.h>

namespace API { class DownloadClient; }
namespace WebCore { class ResourceError; class ResourceResponse; }

namespace WebKit {

class DownloadProxy;
enum class AllowOverwrite : bool;

// A value-only view of a registry notification, delivered synchronously on the
// WTF main RunLoop together with the listener message of the same kind. The
// observer must not call back into the registry synchronously.
struct DownloadRegistryEventHaiku {
    enum class Type : uint8_t { Started, Progress, Finished };
    Type type { Type::Started };
    uint64_t identifier { 0 };
    String url;
    String finalURL;
    String path;
    String mimeType;
    uint64_t currentSize { 0 };
    int64_t expectedSize { -1 };
    // B_WEBKIT_DOWNLOAD_SUCCEEDED/FAILED/CANCELLED; meaningful for Finished only.
    uint32_t result { 0 };
    int32_t error { 0 };
    String errorDescription;
};

// Every method and all engine references are confined to the application's
// WTF main RunLoop. Clients weakly reference this registry; cancellation replies
// retain it until shutdown has removed every registered download.
class DownloadRegistryHaiku final : public std::enable_shared_from_this<DownloadRegistryHaiku> {
public:
    static std::shared_ptr<DownloadRegistryHaiku> create();
    ~DownloadRegistryHaiku();
    Ref<API::DownloadClient> createClient();

    status_t setListener(const BMessenger&);
    status_t setDirectory(const char*);
    bool hasPendingDownloads() const;
    void cancel(uint64_t);
    void cancelAll();
    void shutdown();

    // requestedFilename, when not empty, replaces the engine's suggested name.
    // It is sanitized and uniquified like every other destination.
    void registerDownload(DownloadProxy&, const BMessenger& = { }, const String& requestedFilename = { });
    // One observer (the native extension downloads bridge). Null clears it.
    void setObserver(Function<void(const DownloadRegistryEventHaiku&)>&&);
    const String& directory() const { return m_directory; }
    bool isActive(uint64_t identifier) const { return m_downloads.contains(identifier); }
    void decideDestination(DownloadProxy&, const WebCore::ResourceResponse&, const String&, CompletionHandler<void(AllowOverwrite, String)>&&);
    void didCreateDestination(DownloadProxy&, const String&);
    void didReceiveData(DownloadProxy&, uint64_t);
    void didFinish(DownloadProxy&);
    void didFail(DownloadProxy&, const WebCore::ResourceError&);
    void processDidCrash(DownloadProxy&);

private:
    DownloadRegistryHaiku() = default;
    struct Entry;
    Entry* entry(uint64_t);
    void requestCancel(uint64_t, uint32_t result, int32_t error, const String& description);
    void finish(uint64_t, uint32_t result, const String& domain = { }, int32_t error = 0, const String& description = { });
    status_t notify(Entry&, uint32_t what, uint32_t result = 0, const String& domain = { }, int32_t error = 0, const String& description = { });
    String chooseDestination(const Entry&, const String&) const;

    BMessenger m_listener;
    Function<void(const DownloadRegistryEventHaiku&)> m_observer;
    String m_directory;
    bool m_shuttingDown { false };
    std::unordered_map<uint64_t, std::unique_ptr<Entry>> m_downloads;
};

} // namespace WebKit
