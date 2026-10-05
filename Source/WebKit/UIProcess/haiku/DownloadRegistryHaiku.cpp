/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "DownloadRegistryHaiku.h"

#include "APIDownloadClient.h"
#include "DownloadFilenameHaiku.h"
#include "DownloadProxy.h"
#include "WebKitContext.h"
#include <Message.h>
#include <WebCore/ResourceError.h>
#include <WebCore/ResourceResponse.h>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <wtf/FileSystem.h>
#include <wtf/MonotonicTime.h>
#include <wtf/RunLoop.h>
#include <wtf/TZoneMallocInlines.h>
#include <wtf/text/CString.h>

namespace WebKit {

// A separate client for every registered proxy makes late terminal callbacks
// harmless without keeping an ever-growing set of completed download IDs.
class DownloadClientHaiku final : public API::DownloadClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DownloadClientHaiku);
public:
    explicit DownloadClientHaiku(std::weak_ptr<DownloadRegistryHaiku> registry)
        : m_registry(std::move(registry)) { }

private:
    void decideDestinationWithSuggestedFilename(DownloadProxy& download, const WebCore::ResourceResponse& response, const String& filename, CompletionHandler<void(AllowOverwrite, String)>&& completion) override
    {
        if (!m_terminal) {
            if (auto registry = m_registry.lock())
                return registry->decideDestination(download, response, filename, WTF::move(completion));
        }
        completion(AllowOverwrite::No, { });
    }

    void didCreateDestination(DownloadProxy& download, const String& path) override
    {
        if (!m_terminal) {
            if (auto registry = m_registry.lock())
                registry->didCreateDestination(download, path);
        }
    }

    void didReceiveData(DownloadProxy& download, uint64_t bytes, uint64_t, uint64_t) override
    {
        if (!m_terminal) {
            if (auto registry = m_registry.lock())
                registry->didReceiveData(download, bytes);
        }
    }

    void didFinish(DownloadProxy& download) override
    {
        if (std::exchange(m_terminal, true))
            return;
        if (auto registry = m_registry.lock())
            registry->didFinish(download);
    }

    void didFail(DownloadProxy& download, const WebCore::ResourceError& error, API::Data*) override
    {
        if (std::exchange(m_terminal, true))
            return;
        if (auto registry = m_registry.lock())
            registry->didFail(download, error);
    }

    void processDidCrash(DownloadProxy& download) override
    {
        // A finish received after cancel was issued deliberately retains the
        // registry entry until the cancel reply. Invalidation must release that
        // entry even though a previous terminal callback set m_terminal.
        m_terminal = true;
        if (auto registry = m_registry.lock())
            registry->processDidCrash(download);
    }

    const std::weak_ptr<DownloadRegistryHaiku> m_registry;
    bool m_terminal { false };
};

// WebProcessPool installs this client when it constructs each DownloadProxy.
// DidStart precedes response/redirect/data IPC in PendingDownload. Registering
// here also covers downloads whose originating page has already gone away.
class DownloadBootstrapClientHaiku final : public API::DownloadClient {
    WTF_MAKE_TZONE_ALLOCATED_INLINE(DownloadBootstrapClientHaiku);
public:
    explicit DownloadBootstrapClientHaiku(std::weak_ptr<DownloadRegistryHaiku> registry)
        : m_registry(std::move(registry)) { }

private:
    void legacyDidStart(DownloadProxy& download) override
    {
        if (auto registry = m_registry.lock())
            registry->registerDownload(download);
        else
            download.cancel([](API::Data*) { });
    }

    void didFail(DownloadProxy& download, const WebCore::ResourceError& error, API::Data* data) override
    {
        if (auto registry = m_registry.lock()) {
            registry->registerDownload(download);
            download.client().didFail(download, error, data);
        }
    }

    void processDidCrash(DownloadProxy& download) override
    {
        if (auto registry = m_registry.lock()) {
            registry->registerDownload(download);
            download.client().processDidCrash(download);
        }
    }

    const std::weak_ptr<DownloadRegistryHaiku> m_registry;
};

struct DownloadRegistryHaiku::Entry {
    explicit Entry(DownloadProxy& proxy, const BMessenger& listener, const String& directory)
        : download(proxy), target(listener), directory(directory) { }

    Ref<DownloadProxy> download;
    const BMessenger target;
    const String directory;
    BMessenger view;
    String requestedFilename;
    String destination;
    String responseURL;
    String mimeType;
    uint64_t currentSize { 0 };
    int64_t expectedSize { -1 };
    MonotonicTime lastProgress;
    bool cancelling { false };
    bool cancelIssued { false };
    uint32_t cancelResult { B_WEBKIT_DOWNLOAD_CANCELLED };
    int32_t cancelError { B_CANCELED };
    String cancelDomain { "HaikuDownload"_s };
    String cancelDescription;
};

std::shared_ptr<DownloadRegistryHaiku> DownloadRegistryHaiku::create()
{
    ASSERT(RunLoop::isMain());
    return std::shared_ptr<DownloadRegistryHaiku>(new DownloadRegistryHaiku);
}

DownloadRegistryHaiku::~DownloadRegistryHaiku()
{
    ASSERT(RunLoop::isMain());
    ASSERT(m_downloads.empty());
}

Ref<API::DownloadClient> DownloadRegistryHaiku::createClient()
{
    ASSERT(RunLoop::isMain());
    return adoptRef(*new DownloadBootstrapClientHaiku(weak_from_this()));
}

status_t DownloadRegistryHaiku::setListener(const BMessenger& listener)
{
    ASSERT(RunLoop::isMain());
    if (m_shuttingDown)
        return B_NO_INIT;
    if (!m_downloads.empty())
        return B_BUSY;
    if (!listener.IsValid())
        return B_BAD_VALUE;
    m_listener = listener;
    return B_OK;
}

status_t DownloadRegistryHaiku::setDirectory(const char* directory)
{
    ASSERT(RunLoop::isMain());
    if (m_shuttingDown)
        return B_NO_INIT;
    if (!m_downloads.empty())
        return B_BUSY;
    if (!directory || directory[0] != '/' || String::fromUTF8(directory).isNull())
        return B_BAD_VALUE;
    std::unique_ptr<char, decltype(&std::free)> canonical(realpath(directory, nullptr), &std::free);
    if (!canonical)
        return errno == ENOENT ? B_ENTRY_NOT_FOUND : errno == EACCES ? B_PERMISSION_DENIED : B_IO_ERROR;
    struct stat info;
    if (stat(canonical.get(), &info) || !S_ISDIR(info.st_mode))
        return B_NOT_A_DIRECTORY;
    if (access(canonical.get(), W_OK | X_OK))
        return B_PERMISSION_DENIED;
    auto path = String::fromUTF8(canonical.get());
    if (path.isNull())
        return B_BAD_VALUE;
    m_directory = WTF::move(path);
    return B_OK;
}

bool DownloadRegistryHaiku::hasPendingDownloads() const
{
    ASSERT(RunLoop::isMain());
    return !m_downloads.empty();
}

DownloadRegistryHaiku::Entry* DownloadRegistryHaiku::entry(uint64_t identifier)
{
    auto found = m_downloads.find(identifier);
    return found == m_downloads.end() ? nullptr : found->second.get();
}

void DownloadRegistryHaiku::setObserver(Function<void(const DownloadRegistryEventHaiku&)>&& observer)
{
    ASSERT(RunLoop::isMain());
    m_observer = WTF::move(observer);
}

void DownloadRegistryHaiku::registerDownload(DownloadProxy& download, const BMessenger& view, const String& requestedFilename)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    if (auto* existing = entry(identifier)) {
        if (view.IsValid())
            existing->view = view;
        if (!requestedFilename.isEmpty() && existing->destination.isEmpty())
            existing->requestedFilename = requestedFilename;
        return;
    }
    auto item = std::make_unique<Entry>(download, m_listener, m_directory);
    item->view = view;
    item->requestedFilename = requestedFilename;
    auto* value = item.get();
    m_downloads.emplace(identifier, std::move(item));
    download.setClient(adoptRef(*new DownloadClientHaiku(weak_from_this())));
    auto delivered = notify(*value, B_WEBKIT_DOWNLOAD_STARTED);
    if (m_shuttingDown)
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_CANCELLED, B_CANCELED, "Download context closed"_s);
    else if (delivered != B_OK)
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, delivered, "Download listener is unavailable"_s);
    else if (value->directory.isEmpty())
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, B_NO_INIT, "Download directory is not configured"_s);
}

status_t DownloadRegistryHaiku::notify(Entry& item, uint32_t what, uint32_t result, const String& domain, int32_t error, const String& description)
{
    if (m_observer) {
        DownloadRegistryEventHaiku event;
        event.type = what == B_WEBKIT_DOWNLOAD_STARTED ? DownloadRegistryEventHaiku::Type::Started
            : what == B_WEBKIT_DOWNLOAD_FINISHED ? DownloadRegistryEventHaiku::Type::Finished : DownloadRegistryEventHaiku::Type::Progress;
        event.identifier = item.download->downloadID().toUInt64();
        event.url = item.download->request().url().string();
        event.finalURL = item.responseURL.isEmpty() ? event.url : item.responseURL;
        event.path = item.destination;
        event.mimeType = item.mimeType;
        event.currentSize = item.currentSize;
        event.expectedSize = item.expectedSize;
        event.result = result;
        event.error = error;
        event.errorDescription = description;
        m_observer(event);
    }
    BMessage message(what);
    status_t status = B_OK;
    auto append = [&status](status_t result) {
        if (status == B_OK)
            status = result;
    };
    auto appendString = [&](const char* key, const String& value) {
        append(message.AddString(key, value.isEmpty() ? "" : value.utf8().legacyCStringPointer()));
    };
    append(message.AddUInt64("identifier", item.download->downloadID().toUInt64()));
    appendString("url", item.responseURL.isEmpty() ? item.download->request().url().string() : item.responseURL);
    appendString("path", item.destination);
    appendString("filename", FileSystem::pathFileName(item.destination));
    append(message.AddUInt64("current_size", item.currentSize));
    append(message.AddInt64("expected_size", item.expectedSize));
    if (item.view.IsValid())
        append(message.AddMessenger("view", item.view));
    if (what == B_WEBKIT_DOWNLOAD_FINISHED) {
        append(message.AddUInt32("result", result));
        appendString("error_domain", domain);
        append(message.AddInt32("error", error));
        appendString("error_description", description);
    }
    if (status != B_OK)
        return status;
    // The receiving looper must not synchronously call back into WebKit. All
    // payloads are values; the registry does not expose an engine pointer.
    return item.target.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
}

String DownloadRegistryHaiku::chooseDestination(const Entry& item, const String& suggestedFilename) const
{
    auto name = sanitizedDownloadFilename(BString(suggestedFilename.utf8().legacyCStringPointer()));
    // This bound also avoids a UI-main-thread loop over an unbounded directory.
    for (uint32_t attempt = 0; attempt < 10000; ++attempt) {
        auto leaf = attempt ? downloadFilenameWithCounter(name, attempt - 1) : name;
        auto destination = FileSystem::pathByAppendingComponent(item.directory, String::fromUTF8(leaf.String()));
        bool reserved = false;
        for (const auto& [identifier, other] : m_downloads) {
            if (other.get() != &item && other->destination == destination) {
                reserved = true;
                break;
            }
        }
        if (reserved)
            continue;
        struct stat info;
        if (!lstat(destination.utf8().legacyCStringPointer(), &info))
            continue;
        if (errno == ENOENT)
            return destination;
        return { };
    }
    return { };
}

void DownloadRegistryHaiku::decideDestination(DownloadProxy& download, const WebCore::ResourceResponse& response, const String& filename, CompletionHandler<void(AllowOverwrite, String)>&& completion)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    auto* item = entry(identifier);
    if (!item || item->cancelling || m_shuttingDown) {
        completion(AllowOverwrite::No, { });
        return;
    }
    item->responseURL = response.url().string();
    item->mimeType = response.mimeType();
    item->expectedSize = response.expectedContentLength() < 0 ? -1 : response.expectedContentLength();
    if (item->destination.isEmpty())
        item->destination = chooseDestination(*item, item->requestedFilename.isEmpty() ? filename : item->requestedFilename);
    if (item->destination.isEmpty()) {
        completion(AllowOverwrite::No, { });
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, B_IO_ERROR, "Could not choose an unused download destination"_s);
        return;
    }
    // The NetworkProcess performs the actual O_EXCL open; a competing external
    // creation fails safely. Never pre-create this path or permit overwrite.
    completion(AllowOverwrite::No, item->destination);
}

void DownloadRegistryHaiku::didCreateDestination(DownloadProxy& download, const String& path)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    auto* item = entry(identifier);
    if (!item || item->cancelling)
        return;
    item->destination = path;
    auto result = notify(*item, B_WEBKIT_DOWNLOAD_PROGRESS);
    if (result != B_OK)
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, result, "Download listener is unavailable"_s);
}

void DownloadRegistryHaiku::didReceiveData(DownloadProxy& download, uint64_t bytes)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    auto* item = entry(identifier);
    if (!item || item->cancelling)
        return;
    if (bytes > std::numeric_limits<uint64_t>::max() - item->currentSize) {
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, B_BAD_DATA, "Download byte count overflow"_s);
        return;
    }
    item->currentSize += bytes;
    auto now = MonotonicTime::now();
    if (item->lastProgress && now - item->lastProgress < 100_ms)
        return;
    item->lastProgress = now;
    auto result = notify(*item, B_WEBKIT_DOWNLOAD_PROGRESS);
    if (result != B_OK)
        requestCancel(identifier, B_WEBKIT_DOWNLOAD_FAILED, result, "Download listener is unavailable"_s);
}

void DownloadRegistryHaiku::finish(uint64_t identifier, uint32_t result, const String& domain, int32_t error, const String& description)
{
    ASSERT(RunLoop::isMain());
    // Remove first: reentrancy and late finish/failure/cancel completions cannot
    // emit another terminal notification or release the same entry twice.
    auto found = m_downloads.find(identifier);
    if (found == m_downloads.end())
        return;
    auto item = std::move(found->second);
    m_downloads.erase(found);
    notify(*item, B_WEBKIT_DOWNLOAD_FINISHED, result, domain, error, description);
}

void DownloadRegistryHaiku::didFinish(DownloadProxy& download)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    auto* item = entry(identifier);
    if (!item)
        return;
    if (!item->cancelIssued) {
        // The engine completed before the queued cancellation ran. Its completed
        // file and success win; the queued request will find no entry.
        finish(identifier, B_WEBKIT_DOWNLOAD_SUCCEEDED);
        return;
    }
    // Completion can race an already-issued cancellation. Preserve the observed
    // success, but retain the entry until the cancellation reply removes the
    // proxy from DownloadProxyMap. HasPendingDownloads stays true meanwhile.
    item->cancelResult = B_WEBKIT_DOWNLOAD_SUCCEEDED;
    item->cancelDomain = { };
    item->cancelError = B_OK;
    item->cancelDescription = { };
}

void DownloadRegistryHaiku::didFail(DownloadProxy& download, const WebCore::ResourceError& error)
{
    ASSERT(RunLoop::isMain());
    auto identifier = download.downloadID().toUInt64();
    auto* item = entry(identifier);
    if (!item)
        return;
    if (!item->cancelIssued) {
        finish(identifier, B_WEBKIT_DOWNLOAD_FAILED, error.domain(), error.errorCode(), error.localizedDescription());
        return;
    }
    item->cancelResult = B_WEBKIT_DOWNLOAD_FAILED;
    item->cancelDomain = error.domain();
    item->cancelError = error.errorCode();
    item->cancelDescription = error.localizedDescription();
}

void DownloadRegistryHaiku::processDidCrash(DownloadProxy& download)
{
    ASSERT(RunLoop::isMain());
    // Release our proxy reference during invalidation. Holding it until a later
    // cancellation reply could let DownloadProxy try to remove it from the
    // already-cleared DownloadProxyMap. A crash is a failure, even after a user
    // requested cancellation; it does not prove that a partial file was removed.
    finish(download.downloadID().toUInt64(), B_WEBKIT_DOWNLOAD_FAILED, "HaikuDownload"_s, B_ERROR, "Download network process exited"_s);
}

void DownloadRegistryHaiku::requestCancel(uint64_t identifier, uint32_t result, int32_t error, const String& description)
{
    ASSERT(RunLoop::isMain());
    auto* item = entry(identifier);
    if (!item || item->cancelling)
        return;
    item->cancelling = true;
    item->cancelResult = result;
    item->cancelError = error;
    item->cancelDescription = description;
    // Defer the map mutation out of download callbacks, in particular the
    // DownloadProxyMap::invalidate iteration used during process shutdown.
    RunLoop::mainSingleton().dispatch([registry = shared_from_this(), identifier] {
        auto* item = registry->entry(identifier);
        if (!item)
            return;
        item->cancelIssued = true;
        item->download->cancel([registry, identifier](API::Data*) {
            auto* item = registry->entry(identifier);
            if (!item)
                return;
            registry->finish(identifier, item->cancelResult, item->cancelDomain, item->cancelError, item->cancelDescription);
        });
    });
}

void DownloadRegistryHaiku::cancel(uint64_t identifier)
{
    requestCancel(identifier, B_WEBKIT_DOWNLOAD_CANCELLED, B_CANCELED, "Download cancelled"_s);
}

void DownloadRegistryHaiku::cancelAll()
{
    ASSERT(RunLoop::isMain());
    for (const auto& [identifier, item] : m_downloads)
        cancel(identifier);
}

void DownloadRegistryHaiku::shutdown()
{
    ASSERT(RunLoop::isMain());
    m_shuttingDown = true;
    cancelAll();
}

} // namespace WebKit
