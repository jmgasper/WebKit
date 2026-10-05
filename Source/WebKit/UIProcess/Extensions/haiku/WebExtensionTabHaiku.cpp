/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionTab.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserTabRegistryHaiku.h"
#include "PageLoadState.h"
#include "WebExtensionContext.h"
#include "WebExtensionPermission.h"
#include "WebExtensionController.h"
#include "WebExtensionCookieStoreIdentifier.h"
#include "WebExtensionTabParameters.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebExtensionTab);

WebExtensionTab::WebExtensionTab(const WebExtensionContext& context, WebPageProxy& page)
    : m_extensionContext(context)
    , m_nativePage(page)
{
}

WebExtensionContext* WebExtensionTab::extensionContext() const { return m_extensionContext.get(); }

RefPtr<WebPageProxy> WebExtensionTab::nativePage() const
{
    RefPtr page = m_nativePage.get();
    return page && !page->isClosed() ? page : nullptr;
}

bool WebExtensionTab::isValid() const
{
    RefPtr context = m_extensionContext.get();
    return context && context->isValidTab(*this);
}

bool WebExtensionTab::operator==(const WebExtensionTab& other) const { return this == &other; }
bool WebExtensionTab::isOpen() const { return m_isOpen && isValid(); }

bool WebExtensionTab::extensionHasAccess() const
{
    return isValid() && (!isPrivate() || m_extensionContext->hasAccessToPrivateData());
}

bool WebExtensionTab::extensionHasPermission() const
{
    RefPtr context = m_extensionContext.get();
    return context && extensionHasAccess() && context->hasPermission(url(), const_cast<WebExtensionTab*>(this));
}

URL WebExtensionTab::nativeCommittedOrigin() const
{
    RefPtr page = nativePage();
    if (!page || !isValid())
        return { };
    auto& origin = page->pageLoadState().origin();
    return origin.isOpaque() ? URL { } : origin.toURL();
}

bool WebExtensionTab::shouldGrantPermissionsOnUserGesture() const
{
    RefPtr context = m_extensionContext.get();
    if (!context || !isOpen() || !extensionHasAccess())
        return false;
    auto origin = nativeCommittedOrigin();
    return origin.isValid() && (origin.protocolIsInHTTPFamily() || (origin.protocolIsFile() && context->hasAccessToFileURLs()));
}

bool WebExtensionTab::grantNativeTemporaryPermission()
{
    if (!shouldGrantPermissionsOnUserGesture())
        return false;
    Ref context = *m_extensionContext;
    if (!m_nativeActiveTabGrant.grant(nativeCommittedOrigin(), context->hasAccessToFileURLs()))
        return false;
    m_temporaryPermissionMatchPattern = m_nativeActiveTabGrant.pattern();
    return true;
}

bool WebExtensionTab::hasNativeTemporaryPermission(const URL& requestedURL) const
{
    RefPtr context = m_extensionContext.get();
    if (!context || !isOpen() || !extensionHasAccess() || !context->hasPermission(WebExtensionPermission::activeTab()))
        return false;
    if (!m_nativeActiveTabGrant.matchesURL(nativeCommittedOrigin(), requestedURL, context->hasAccessToFileURLs()))
        return false;
    // Read only the persistent policy here: passing this tab would recurse.
    // A current denial always overrides a previous user invocation.
    auto state = context->permissionState(requestedURL, nullptr, WebExtensionContext::PermissionStateOptions::SkipRequestedPermissions);
    return state != WebExtensionContext::PermissionState::DeniedExplicitly && state != WebExtensionContext::PermissionState::DeniedImplicitly;
}

bool WebExtensionTab::extensionHasTemporaryPermission() const
{
    return hasNativeTemporaryPermission(nativeCommittedOrigin());
}

bool WebExtensionTab::clearNativeUserGesture()
{
    bool changed = m_activeUserGesture || m_nativeActiveTabGrant.isGranted();
    m_activeUserGesture = false;
    m_nativeActiveTabGrant.clear();
    m_temporaryPermissionMatchPattern = nullptr;
    return changed;
}

void WebExtensionTab::nativeDidCommitNavigation()
{
    m_activeUserGesture = false;
    if (m_nativeActiveTabGrant.revokeForNavigation(nativeCommittedOrigin()))
        m_temporaryPermissionMatchPattern = nullptr;
}

bool WebExtensionTab::isPrivate() const
{
    RefPtr page = nativePage();
    return page && page->websiteDataStore().sessionID().isEphemeral();
}

RefPtr<WebExtensionWindow> WebExtensionTab::window() const
{
    RefPtr context = m_extensionContext.get();
    RefPtr page = nativePage();
    if (!context || !page || !isValid())
        return nullptr;
    RefPtr controller = context->extensionController();
    auto state = controller->nativeBrowserTabs()->tab(page->identifier());
    return state ? context->getOrCreateNativeWindow(state->windowIdentifier) : nullptr;
}

size_t WebExtensionTab::index() const
{
    RefPtr context = m_extensionContext.get();
    RefPtr page = nativePage();
    if (!context || !page || !isValid())
        return notFound;
    RefPtr controller = context->extensionController();
    auto state = controller->nativeBrowserTabs()->tab(page->identifier());
    return state ? state->index : notFound;
}

bool WebExtensionTab::isActive() const
{
    RefPtr context = m_extensionContext.get();
    RefPtr page = nativePage();
    if (!context || !page || !isValid())
        return false;
    RefPtr controller = context->extensionController();
    auto state = controller->nativeBrowserTabs()->tab(page->identifier());
    return state && state->active;
}

bool WebExtensionTab::isSelected() const { return isActive(); }

String WebExtensionTab::title() const
{
    RefPtr page = nativePage();
    return page && isValid() ? page->pageLoadState().title() : nullString();
}

URL WebExtensionTab::url() const
{
    RefPtr page = nativePage();
    return page && isValid() ? page->pageLoadState().activeURL() : URL { };
}

URL WebExtensionTab::pendingURL() const
{
    RefPtr page = nativePage();
    return page && isValid() ? page->pageLoadState().provisionalURL() : URL { };
}

bool WebExtensionTab::isLoadingComplete() const
{
    RefPtr page = nativePage();
    return page && isValid() && !page->pageLoadState().isLoading();
}

bool WebExtensionTab::isDiscarded() const
{
    // The native browser owns each open tab's page for its entire lifetime.
    // Hiding a tab does not discard it; there is no unload-and-restore path yet.
    return false;
}

WebExtensionTabParameters WebExtensionTab::parameters() const
{
    WebExtensionTabParameters result;
    result.identifier = identifier();
    if (!extensionHasAccess())
        return result;
    bool hasPermission = extensionHasPermission() || m_extensionContext->hasPermission(WebExtensionPermission::tabs(), const_cast<WebExtensionTab*>(this));
    result.url = hasPermission ? url() : URL { };
    result.title = hasPermission ? title() : nullString();
    RefPtr window = this->window();
    result.windowIdentifier = window ? window->identifier() : WebExtensionWindowConstants::NoneIdentifier;
    result.index = index();
    result.active = isActive();
    result.selected = isSelected();
    result.loading = !isLoadingComplete();
    result.discarded = isDiscarded();
    result.privateBrowsing = isPrivate();
    if (RefPtr page = nativePage()) {
        result.audible = page->isPlayingAudio();
        result.muted = page->isAudioMuted();
        // Cookie managers (Cookie AutoDelete) decide which cookie stores are in use
        // from tab.cookieStoreId, else from Chrome's "0"/"1". Neither matched
        // Summit's storeIds, so cookies of open tabs were treated as unused.
        result.cookieStoreIdentifier = webExtensionCookieStoreIdentifier(page->websiteDataStore().sessionID());
    }
    result.pinned = false; // The native browser has no pinned-tab state yet.
    return result;
}

WebExtensionTabParameters WebExtensionTab::changedParameters(OptionSet<ChangedProperties> properties) const
{
    WebExtensionTabParameters result;
    if (!extensionHasAccess())
        return result;
    if (properties.isEmpty())
        properties = changedProperties();
    bool hasPermission = extensionHasPermission() || m_extensionContext->hasPermission(WebExtensionPermission::tabs(), const_cast<WebExtensionTab*>(this));
    if (properties.contains(ChangedProperties::URL))
        result.url = hasPermission ? url() : URL { };
    if (properties.contains(ChangedProperties::Title))
        result.title = hasPermission ? title() : nullString();
    if (properties.contains(ChangedProperties::Loading))
        result.loading = !isLoadingComplete();
    if (properties.contains(ChangedProperties::Muted))
        result.muted = isMuted();
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
