/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebViewContextHaiku.h"
#include "BrowserCommandBrokerHaiku.h"
#include "BrowserTabRegistryHaiku.h"

#include "APIDownloadClient.h"
#include "APIPageConfiguration.h"
#include "DownloadProxy.h"
#include "DownloadRegistryHaiku.h"
#include "ExtensionPermissionPromptHaiku.h"
#include "ExtensionPackageRegistryHaiku.h"
#include "SitePermissionsHaiku.h"
#include "APIProcessPoolConfiguration.h"
#include "StoragePathsHaiku.h"
#include "WebsiteDataStore.h"
#include "WebsiteDataStoreConfiguration.h"
#if ENABLE(WK_WEB_EXTENSIONS)
#include "WebExtensionController.h"
#include "WebExtensionControllerConfiguration.h"
#include "haiku/WebExtensionDownloadsHaiku.h"
#include "haiku/WebExtensionStagingDirectoryHaiku.h"
#endif
#include "WebProcessPool.h"
#include <pal/SessionID.h>
#include <cstdlib>
#include <wtf/FileSystem.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>

namespace WebKit {

#if ENABLE(WK_WEB_EXTENSIONS)
static Ref<WebExtensionController> createNativeExtensionController(WebsiteDataStore& store, const String& storageDirectory)
{
    auto configuration = store.isPersistent() ? WebExtensionControllerConfiguration::createDefault() : WebExtensionControllerConfiguration::createNonPersistent();
    if (store.isPersistent()) {
        ASSERT(!storageDirectory.isEmpty());
        configuration->setStorageDirectory(storageDirectory);
    }
    configuration->setDefaultWebsiteDataStore(&store);
    return WebExtensionController::create(WTF::move(configuration));
}
#endif

WebViewContextHaiku::WebViewContextHaiku(Ref<WebsiteDataStore>&& dataStore, [[maybe_unused]] String extensionStorageDirectory)
    : m_dataStore(WTF::move(dataStore))
    , m_processPool(WebProcessPool::create(API::ProcessPoolConfiguration::create()))
    , m_downloads(DownloadRegistryHaiku::create())
#if ENABLE(WK_WEB_EXTENSIONS)
    , m_extensionController(createNativeExtensionController(m_dataStore.get(), extensionStorageDirectory))
    , m_extensionPackages(ExtensionPackageRegistryHaiku::create(m_extensionController.copyRef(), !m_dataStore->isPersistent()))
    , m_browserTabs(m_extensionController->nativeBrowserTabs())
#else
    , m_browserTabs(std::make_shared<BrowserTabRegistryHaiku>())
#endif
    , m_sitePermissions(SitePermissionsHaiku::create(m_processPool.get(), !m_dataStore->isPersistent()))
{
    ASSERT(RunLoop::isMain());
    m_processPool->setLegacyDownloadClient(m_downloads->createClient());
#if ENABLE(WK_WEB_EXTENSIONS)
    // Extensions with the downloads permission observe and start this
    // context's downloads; a private context keeps its own separate history.
    m_extensionController->nativeDownloads().attach(m_downloads, !m_dataStore->isPersistent());
#endif
}

WebViewContextHaiku::WebViewContextHaiku(Ref<WebsiteDataStore>&& dataStore, std::shared_ptr<WebViewContextHaiku> extensionOwner)
    : m_dataStore(WTF::move(dataStore))
    , m_processPool(WebProcessPool::create(API::ProcessPoolConfiguration::create()))
    , m_downloads(DownloadRegistryHaiku::create())
#if ENABLE(WK_WEB_EXTENSIONS)
    , m_extensionController(extensionOwner->m_extensionController.copyRef())
    , m_extensionPackages(extensionOwner->m_extensionPackages)
#endif
    , m_browserTabs(extensionOwner->m_browserTabs)
    , m_extensionOwner(WTF::move(extensionOwner))
    , m_sitePermissions(SitePermissionsHaiku::create(m_processPool.get(), true))
{
    ASSERT(RunLoop::isMain());
    // Downloads of private pages stay out of extensions' downloads API.
    m_processPool->setLegacyDownloadClient(m_downloads->createClient());
}

WebViewContextHaiku::~WebViewContextHaiku()
{
    ASSERT(RunLoop::isMain());
    m_sitePermissions->shutdown();
    if (m_extensionOwner) {
        // The extensions, their prompts and commands are the owner's.
        m_downloads->shutdown();
        return;
    }
#if ENABLE(WK_WEB_EXTENSIONS)
    m_extensionController->nativeDownloads().shutdown();
#endif
    m_downloads->shutdown();
#if ENABLE(WK_WEB_EXTENSIONS)
    m_extensionPackages->shutdown();
#endif
    if (auto prompts = permissionPrompts())
        prompts->shutdown();
    if (auto commands = browserCommands())
        commands->shutdown();
}

void WebViewContextHaiku::allowServerCertificate(const String& host, const String& sha256)
{
    ASSERT(RunLoop::isMain());
    if (host.isEmpty() || sha256.isEmpty())
        return;
    m_allowedServerCertificates.ensure(host.convertToASCIILowercase(), [] {
        return HashSet<String> { };
    }).iterator->value.add(sha256.convertToASCIILowercase());
}

void WebViewContextHaiku::forgetServerCertificates(const String& host)
{
    ASSERT(RunLoop::isMain());
    if (host.isEmpty())
        m_allowedServerCertificates.clear();
    else
        m_allowedServerCertificates.remove(host.convertToASCIILowercase());
}

bool WebViewContextHaiku::allowsServerCertificate(const String& host, const String& sha256) const
{
    ASSERT(RunLoop::isMain());
    auto iterator = m_allowedServerCertificates.find(host.convertToASCIILowercase());
    return iterator != m_allowedServerCertificates.end()
        && iterator->value.contains(sha256.convertToASCIILowercase());
}

WebsiteDataStore& WebViewContextHaiku::dataStore() const
{
    return m_dataStore.get();
}

std::shared_ptr<BrowserCommandBrokerHaiku> WebViewContextHaiku::browserCommands() const
{
    ASSERT(RunLoop::isMain());
#if ENABLE(WK_WEB_EXTENSIONS)
    return m_extensionController->nativeBrowserCommands();
#else
    return nullptr;
#endif
}

std::shared_ptr<ExtensionPermissionPromptHaiku> WebViewContextHaiku::permissionPrompts() const
{
    ASSERT(RunLoop::isMain());
#if ENABLE(WK_WEB_EXTENSIONS)
    return m_extensionController->nativePermissionPrompts();
#else
    return nullptr;
#endif
}

std::shared_ptr<ExtensionPackageRegistryHaiku> WebViewContextHaiku::extensionPackages() const
{
    ASSERT(RunLoop::isMain());
#if ENABLE(WK_WEB_EXTENSIONS)
    return m_extensionPackages;
#else
    return nullptr;
#endif
}

std::shared_ptr<WebViewContextHaiku> WebViewContextHaiku::adopt(WebViewContextHaiku* created)
{
    return { created, [](WebViewContextHaiku* context) {
        if (RunLoop::isMain())
            delete context;
        else
            RunLoop::mainSingleton().dispatch([context] { delete context; });
    } };
}

std::shared_ptr<WebViewContextHaiku> WebViewContextHaiku::adopt(Ref<WebsiteDataStore>&& dataStore, String extensionStorageDirectory)
{
    return adopt(new WebViewContextHaiku(WTF::move(dataStore), WTF::move(extensionStorageDirectory)));
}

std::shared_ptr<WebViewContextHaiku> WebViewContextHaiku::createPrivate(std::shared_ptr<WebViewContextHaiku> extensionOwner)
{
    ASSERT(RunLoop::isMain());
    if (!extensionOwner || !extensionOwner->m_dataStore->isPersistent())
        return nullptr;
    return adopt(new WebViewContextHaiku(WebsiteDataStore::createNonPersistent(), WTF::move(extensionOwner)));
}

std::shared_ptr<WebViewContextHaiku> WebViewContextHaiku::create(const char* profilePath, bool privateBrowsing, status_t& status)
{
    ASSERT(RunLoop::isMain());
    status = B_OK;
    if (privateBrowsing)
        return adopt(WebsiteDataStore::createNonPersistent());

    String requestedPath = String::fromUTF8(profilePath);
    if (profilePath && (requestedPath.isEmpty() || !requestedPath.startsWith('/'))) {
        status = B_BAD_VALUE;
        return nullptr;
    }
    auto profile = storageDirectoryPathHaiku(false, requestedPath, emptyString(), true);
    if (profile.isEmpty()) {
        status = B_IO_ERROR;
        return nullptr;
    }
    auto cache = storageDirectoryPathHaiku(true, profile, "Cache"_s, true);
    auto data = storageDirectoryPathHaiku(false, profile, "Data"_s, true);
    if (cache.isEmpty() || data.isEmpty()) {
        status = B_IO_ERROR;
        return nullptr;
    }

    // Use WebKit's complete directory initialization so new storage backends
    // inherit these same profile roots. Curl's cookie path is not initialized
    // by the common non-Cocoa implementation, so supply it explicitly.
    auto configuration = WebsiteDataStoreConfiguration::createWithBaseDirectories(cache, data);
    // Compiled scripts kept between visits (CachedScriptSourceProviderHaiku.cpp):
    // web processes inherit this, the first persistent profile's.
    setenv("WEBKIT_HAIKU_BYTECODE_CACHE", FileSystem::pathByAppendingComponent(cache, "JavaScript"_s).utf8().legacyCStringPointer(), 0);
    configuration->setCookieStorageFile(FileSystem::pathByAppendingComponent(data, "Cookies.db"_s));
    configuration->setSearchFieldHistoryDirectory(storageDirectoryPathHaiku(false, data, "SearchFieldHistory"_s, true));
    configuration->setEnhancedSecurityDirectory(storageDirectoryPathHaiku(false, data, "EnhancedSecurity"_s, true));
    if (auto type = FileSystem::fileTypeFollowingSymlinks(configuration->cookieStorageFile()); type && *type != FileSystem::FileType::Regular) {
        status = B_BAD_VALUE;
        return nullptr;
    }

    const auto& paths = configuration->directories();
    // Directory creation failure must not silently turn one backend into an
    // in-memory store or cause it to fall back to another profile's defaults.
    for (const auto* path : {
        &paths.alternativeServicesDirectory, &paths.cacheStorageDirectory,
        &paths.cookieStorageFile, &paths.deviceIdHashSaltsStorageDirectory,
#if ENABLE(ENCRYPTED_MEDIA)
        &paths.mediaKeysHashSaltsStorageDirectory,
#endif
        &paths.generalStorageDirectory, &paths.hstsStorageDirectory,
        &paths.indexedDBDatabaseDirectory, &paths.isolatedSitesDirectory,
        &paths.localStorageDirectory, &paths.mediaCacheDirectory,
        &paths.mediaKeysStorageDirectory, &paths.networkCacheDirectory,
        &paths.resourceLoadStatisticsDirectory, &paths.searchFieldHistoryDirectory,
        &paths.serviceWorkerRegistrationDirectory, &paths.webSQLDatabaseDirectory,
#if ENABLE(CONTENT_EXTENSIONS)
        &paths.resourceMonitorThrottlerDirectory,
#endif
        &paths.enhancedSecurityDirectory,
    }) {
        if (path->isEmpty()) {
            status = B_IO_ERROR;
            return nullptr;
        }
    }
    // JavaScriptConfiguration is a Cocoa-only configuration input. Its common
    // Haiku default stays empty instead of enabling an unrelated configuration.
    String extensionStorageDirectory;
#if ENABLE(WK_WEB_EXTENSIONS)
    extensionStorageDirectory = storageDirectoryPathHaiku(false, profile, "WebExtensions"_s, true);
    if (extensionStorageDirectory.isEmpty()) {
        status = B_IO_ERROR;
        return nullptr;
    }
    // Stage extension packages beside the profile they belong to. The system
    // temporary directory can be on a different, much smaller volume, where a
    // failed copy would silently leave every extension unloaded.
    setWebExtensionStagingDirectoryHaiku(profile);
#endif
    return adopt(WebsiteDataStore::create(WTF::move(configuration), PAL::SessionID::generatePersistentSessionID()), WTF::move(extensionStorageDirectory));
}

std::shared_ptr<WebViewContextHaiku> WebViewContextHaiku::defaultContext(status_t& status)
{
    ASSERT(RunLoop::isMain());
    static NeverDestroyed<std::weak_ptr<WebViewContextHaiku>> weakContext;
    auto context = weakContext.get().lock();
    if (context) {
        status = B_OK;
        return context;
    }
    context = create(nullptr, false, status);
    weakContext.get() = context;
    return context;
}

void WebViewContextHaiku::configure(API::PageConfiguration& configuration) const
{
    ASSERT(RunLoop::isMain());
    configuration.setProcessPool(m_processPool.copyRef());
    configuration.setWebsiteDataStore(m_dataStore.copyRef());
#if ENABLE(WK_WEB_EXTENSIONS)
    configuration.setWebExtensionController(m_extensionController.copyRef());
#endif
}

void WebViewContextHaiku::download(WebPageProxy& page, const WebCore::ResourceRequest& request, const BMessenger& view)
{
    ASSERT(RunLoop::isMain());
    Ref download = m_processPool->download(m_dataStore.get(), &page, request, { });
    // Register before a DidStart/error reply so the explicit operation has the
    // same native client and lifetime rules as a navigation-triggered download.
    m_downloads->registerDownload(download.get(), view);
}

} // namespace WebKit
