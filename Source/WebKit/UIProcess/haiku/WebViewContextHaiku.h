/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include <Messenger.h>
#include <SupportDefs.h>
#include <memory>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Ref.h>
#include <wtf/text/StringHash.h>
#include <wtf/text/WTFString.h>

namespace API { class PageConfiguration; }
namespace WebCore { class ResourceRequest; }

namespace WebKit {

class BrowserCommandBrokerHaiku;
class BrowserTabRegistryHaiku;
class DownloadRegistryHaiku;
class ExtensionPermissionPromptHaiku;
class ExtensionPackageRegistryHaiku;
class SitePermissionsHaiku;
class WebExtensionController;
class WebPageProxy;
class WebProcessPool;
class WebsiteDataStore;

// References to WebKit objects are created, inspected and released on the
// application thread. The shared owner itself may travel across native loopers.
class WebViewContextHaiku final {
public:
    static std::shared_ptr<WebViewContextHaiku> create(const char*, bool, status_t&);
    static std::shared_ptr<WebViewContextHaiku> defaultContext(status_t&);
    // A private context whose pages are controlled by the extensions of
    // extensionOwner, as one extension runtime spans normal and private
    // windows in other browsers. Only extensions allowed in private browsing
    // see or act on its pages. It keeps extensionOwner alive.
    static std::shared_ptr<WebViewContextHaiku> createPrivate(std::shared_ptr<WebViewContextHaiku> extensionOwner);
    ~WebViewContextHaiku();
    void configure(API::PageConfiguration&) const;
    const std::shared_ptr<DownloadRegistryHaiku>& downloads() const { return m_downloads; }
    const std::shared_ptr<BrowserTabRegistryHaiku>& browserTabs() const { return m_browserTabs; }
    WebsiteDataStore& dataStore() const;
    std::shared_ptr<ExtensionPermissionPromptHaiku> permissionPrompts() const;
    // Null when this build has no extension support.
    std::shared_ptr<BrowserCommandBrokerHaiku> browserCommands() const;
    std::shared_ptr<ExtensionPackageRegistryHaiku> extensionPackages() const;
    void download(WebPageProxy&, const WebCore::ResourceRequest&, const BMessenger&);
    // Notifications and location: requests, the host's decisions, providers.
    const std::shared_ptr<SitePermissionsHaiku>& sitePermissions() const { return m_sitePermissions; }
    // Server certificates the user trusts for a host although they failed
    // verification, by SHA-256 of the DER leaf certificate (lower-case hex).
    // Application thread only.
    void allowServerCertificate(const String& host, const String& sha256);
    void forgetServerCertificates(const String& host);
    bool allowsServerCertificate(const String& host, const String& sha256) const;

private:
    explicit WebViewContextHaiku(Ref<WebsiteDataStore>&&, String extensionStorageDirectory);
    WebViewContextHaiku(Ref<WebsiteDataStore>&&, std::shared_ptr<WebViewContextHaiku> extensionOwner);
    static std::shared_ptr<WebViewContextHaiku> adopt(WebViewContextHaiku*);
    static std::shared_ptr<WebViewContextHaiku> adopt(Ref<WebsiteDataStore>&&, String extensionStorageDirectory = { });

    const Ref<WebsiteDataStore> m_dataStore;
    const Ref<WebProcessPool> m_processPool;
    const std::shared_ptr<DownloadRegistryHaiku> m_downloads;
#if ENABLE(WK_WEB_EXTENSIONS)
    const Ref<WebExtensionController> m_extensionController;
    const std::shared_ptr<ExtensionPackageRegistryHaiku> m_extensionPackages;
#endif
    const std::shared_ptr<BrowserTabRegistryHaiku> m_browserTabs;
    // Set when the extensions above are another context's.
    const std::shared_ptr<WebViewContextHaiku> m_extensionOwner;
    HashMap<String, HashSet<String>> m_allowedServerCertificates;
    const std::shared_ptr<SitePermissionsHaiku> m_sitePermissions;
};

} // namespace WebKit
