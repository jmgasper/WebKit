/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionWindow.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserTabRegistryHaiku.h"
#include "WebExtensionContext.h"
#include "WebExtensionController.h"
#include "WebExtensionTab.h"
#include "WebExtensionWindowParameters.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {

WTF_MAKE_TZONE_ALLOCATED_IMPL(WebExtensionWindow);

WebExtensionWindow::WebExtensionWindow(const WebExtensionContext& context, uint64_t identifier)
    : m_extensionContext(context)
    , m_nativeWindowIdentifier(identifier)
{
}

WebExtensionContext* WebExtensionWindow::extensionContext() const { return m_extensionContext.get(); }
bool WebExtensionWindow::operator==(const WebExtensionWindow& other) const { return this == &other; }

bool WebExtensionWindow::isValid() const
{
    RefPtr context = m_extensionContext.get();
    return context && context->isValidWindow(*this);
}

bool WebExtensionWindow::isOpen() const { return m_isOpen && isValid(); }

bool WebExtensionWindow::extensionHasAccess() const
{
    return isValid() && (!isPrivate() || m_extensionContext->hasAccessToPrivateData());
}

bool WebExtensionWindow::isPrivate() const
{
    RefPtr context = m_extensionContext.get();
    RefPtr controller = context ? context->extensionController() : nullptr;
    if (!controller)
        return false;
    if (!controller->configuration().defaultWebsiteDataStore().isPersistent())
        return true;
    // The extensions of a normal profile also serve its private windows
    // (WebViewContextHaiku::createPrivate): a window is as private as its pages.
    auto window = controller->nativeBrowserTabs()->window(m_nativeWindowIdentifier);
    if (!window || window->pages.isEmpty())
        return false;
    RefPtr page = context->nativePage(window->pages.first());
    return page && page->websiteDataStore().sessionID().isEphemeral();
}

WebExtensionWindow::TabVector WebExtensionWindow::tabs(SkipValidation) const
{
    TabVector result;
    RefPtr context = m_extensionContext.get();
    if (!context || !isValid())
        return result;
    RefPtr controller = context->extensionController();
    auto window = controller->nativeBrowserTabs()->window(m_nativeWindowIdentifier);
    if (!window)
        return result;
    for (auto page : window->pages) {
        if (RefPtr tab = context->getOrCreateNativeTab(page))
            result.append(tab.releaseNonNull());
    }
    return result;
}

RefPtr<WebExtensionTab> WebExtensionWindow::activeTab(SkipValidation) const
{
    RefPtr context = m_extensionContext.get();
    if (!context || !isValid())
        return nullptr;
    RefPtr controller = context->extensionController();
    auto window = controller->nativeBrowserTabs()->window(m_nativeWindowIdentifier);
    return window && window->activePage ? context->getOrCreateNativeTab(*window->activePage) : nullptr;
}

bool WebExtensionWindow::isFocused() const
{
    RefPtr context = m_extensionContext.get();
    if (!context || !isValid())
        return false;
    RefPtr controller = context->extensionController();
    return controller->nativeBrowserTabs()->focusedWindow() == m_nativeWindowIdentifier;
}

bool WebExtensionWindow::isFrontmost() const
{
    RefPtr context = m_extensionContext.get();
    if (!context || !isValid())
        return false;
    RefPtr controller = context->extensionController();
    auto windows = controller->nativeBrowserTabs()->windows();
    return !windows.isEmpty() && windows[0].identifier == m_nativeWindowIdentifier;
}

WebExtensionWindowParameters WebExtensionWindow::minimalParameters() const
{
    WebExtensionWindowParameters result;
    result.identifier = identifier();
    result.type = Type::Normal;
    return result;
}

WebExtensionWindowParameters WebExtensionWindow::parameters(PopulateTabs populateTabs) const
{
    auto result = minimalParameters();
    if (!extensionHasAccess())
        return result;
    result.focused = isFocused();
    result.privateBrowsing = isPrivate();
    if (populateTabs == PopulateTabs::Yes) {
        Vector<WebExtensionTabParameters> parameters;
        for (auto& tab : tabs()) {
            if (tab->isOpen() && tab->extensionHasAccess())
                parameters.append(tab->parameters());
        }
        result.tabs = WTF::move(parameters);
    }
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
