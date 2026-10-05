/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserTabRegistryHaiku.h"
#include "WebExtensionController.h"
#include "WebPageProxy.h"

namespace WebKit {

RefPtr<WebPageProxy> WebExtensionContext::nativePage(WebPageProxyIdentifier identifier) const
{
    RefPtr controller = extensionController();
    if (!isLoaded() || !controller)
        return nullptr;
    auto registry = controller->nativeBrowserTabs();
    auto tab = registry->tab(identifier);
    if (!tab)
        return nullptr;
    auto window = registry->window(tab->windowIdentifier);
    if (!window || !window->messenger.IsValid())
        return nullptr;
    for (auto& page : controller->allPages()) {
        if (page.identifier() == identifier && !page.isClosed() && page.webExtensionController() == controller.get())
            return page;
    }
    return nullptr;
}

bool WebExtensionContext::isValidTab(const WebExtensionTab& tab)
{
    if (!isLoaded() || tab.extensionContext() != this || m_tabMap.get(tab.identifier()) != &tab)
        return false;
    RefPtr page = tab.nativePage();
    return page && nativePage(page->identifier()) == page;
}

bool WebExtensionContext::isValidWindow(const WebExtensionWindow& window)
{
    if (!isLoaded() || window.extensionContext() != this || m_windowMap.get(window.identifier()) != &window)
        return false;
    RefPtr controller = extensionController();
    if (!controller)
        return false;
    auto nativeWindow = controller->nativeBrowserTabs()->window(window.nativeWindowIdentifier());
    return nativeWindow && nativeWindow->messenger.IsValid();
}

void WebExtensionContext::pruneNativeHostObjects() const
{
    // Validity reads these maps to check identity. Collect first so validation
    // never observes a hash table partway through removing its buckets.
    Vector<WebExtensionTabIdentifier> staleTabs;
    for (auto& entry : m_tabMap) {
        if (!entry.value->isValid())
            staleTabs.append(entry.key);
    }
    for (auto identifier : staleTabs) {
        if (RefPtr tab = m_tabMap.get(identifier))
            tab->clearNativeUserGesture();
        m_tabMap.remove(identifier);
    }
    Vector<WebExtensionWindowIdentifier> staleWindows;
    for (auto& entry : m_windowMap) {
        if (!entry.value->isValid())
            staleWindows.append(entry.key);
    }
    for (auto identifier : staleWindows)
        m_windowMap.remove(identifier);
    m_windowOrderVector.removeAllMatching([&](auto identifier) { return !m_windowMap.contains(identifier); });
    if (m_focusedWindowIdentifier && !m_windowMap.contains(*m_focusedWindowIdentifier))
        m_focusedWindowIdentifier = std::nullopt;
}

RefPtr<WebExtensionWindow> WebExtensionContext::getOrCreateNativeWindow(uint64_t nativeIdentifier) const
{
    pruneNativeHostObjects();
    RefPtr controller = extensionController();
    if (!isLoaded() || !controller)
        return nullptr;
    auto nativeWindow = controller->nativeBrowserTabs()->window(nativeIdentifier);
    if (!nativeWindow || !nativeWindow->messenger.IsValid())
        return nullptr;
    for (auto& entry : m_windowMap) {
        if (entry.value->nativeWindowIdentifier() == nativeIdentifier)
            return entry.value.copyRef();
    }
    Ref window = WebExtensionWindow::create(*this, nativeIdentifier);
    m_windowMap.set(window->identifier(), window.copyRef());
    window->didOpen();
    return window;
}

RefPtr<WebExtensionTab> WebExtensionContext::getOrCreateNativeTab(WebPageProxyIdentifier identifier) const
{
    pruneNativeHostObjects();
    RefPtr page = nativePage(identifier);
    if (!page)
        return nullptr;
    for (auto& entry : m_tabMap) {
        if (entry.value->nativePage() == page)
            return entry.value.copyRef();
    }
    Ref tab = WebExtensionTab::create(*this, *page);
    m_tabMap.set(tab->identifier(), tab.copyRef());
    tab->didOpen();
    return tab;
}

RefPtr<WebExtensionTab> WebExtensionContext::getTab(WebExtensionTabIdentifier identifier, IgnoreExtensionAccess ignoreExtensionAccess) const
{
    if (!isValid(identifier))
        return nullptr;
    pruneNativeHostObjects();
    RefPtr tab = m_tabMap.get(identifier);
    if (tab && ignoreExtensionAccess == IgnoreExtensionAccess::No && !tab->extensionHasAccess())
        return nullptr;
    return tab;
}

RefPtr<WebExtensionTab> WebExtensionContext::getTab(WebPageProxyIdentifier pageIdentifier, std::optional<WebExtensionTabIdentifier> identifier, IncludeExtensionViews includeExtensionViews, IgnoreExtensionAccess ignoreExtensionAccess) const
{
    return identifier ? getTab(*identifier, ignoreExtensionAccess) : getCurrentTab(pageIdentifier, includeExtensionViews, ignoreExtensionAccess);
}

RefPtr<WebExtensionTab> WebExtensionContext::getCurrentTab(WebPageProxyIdentifier identifier, IncludeExtensionViews includeExtensionViews, IgnoreExtensionAccess ignoreExtensionAccess) const
{
    RefPtr<WebExtensionTab> tab;
    if (isBackgroundPage(identifier)) {
        if (includeExtensionViews == IncludeExtensionViews::No)
            return nullptr;
        if (RefPtr window = frontmostWindow(ignoreExtensionAccess))
            tab = window->activeTab();
    } else {
        bool popupPage = false;
        for (auto entry : m_popupPageActionMap) {
            if (entry.key.identifier() != identifier)
                continue;
            popupPage = true;
            if (includeExtensionViews == IncludeExtensionViews::No)
                return nullptr;
            tab = entry.value->tab();
            break;
        }
        if (!popupPage)
            tab = getOrCreateNativeTab(identifier);
    }
    if (tab && !tab->isOpen())
        return nullptr;
    if (tab && ignoreExtensionAccess == IgnoreExtensionAccess::No && !tab->extensionHasAccess())
        return nullptr;
    return tab;
}

RefPtr<WebExtensionWindow> WebExtensionContext::getWindow(WebExtensionWindowIdentifier identifier, std::optional<WebPageProxyIdentifier> pageIdentifier, IgnoreExtensionAccess ignoreExtensionAccess) const
{
    if (!isValid(identifier))
        return nullptr;
    pruneNativeHostObjects();
    RefPtr<WebExtensionWindow> window;
    if (isCurrent(identifier)) {
        if (pageIdentifier) {
            if (RefPtr tab = getCurrentTab(*pageIdentifier, IncludeExtensionViews::Yes, ignoreExtensionAccess))
                window = tab->window();
        }
        if (!window)
            window = frontmostWindow(ignoreExtensionAccess);
    } else
        window = m_windowMap.get(identifier);
    if (window && ignoreExtensionAccess == IgnoreExtensionAccess::No && !window->extensionHasAccess())
        return nullptr;
    return window;
}

void WebExtensionContext::forgetTab(WebExtensionTabIdentifier identifier) const
{
    if (RefPtr tab = m_tabMap.get(identifier))
        tab->clearNativeUserGesture();
    m_tabMap.remove(identifier);
}

void WebExtensionContext::forgetWindow(WebExtensionWindowIdentifier identifier) const
{
    m_windowMap.remove(identifier);
    m_windowOrderVector.removeAll(identifier);
    if (m_focusedWindowIdentifier == identifier)
        m_focusedWindowIdentifier = std::nullopt;
}

WebExtensionContext::WindowVector WebExtensionContext::openWindows(IgnoreExtensionAccess ignoreExtensionAccess) const
{
    pruneNativeHostObjects();
    WindowVector result;
    RefPtr controller = extensionController();
    if (!isLoaded() || !controller)
        return result;
    for (auto& state : controller->nativeBrowserTabs()->windows()) {
        RefPtr window = getOrCreateNativeWindow(state.identifier);
        if (window && (ignoreExtensionAccess == IgnoreExtensionAccess::Yes || window->extensionHasAccess()))
            result.append(window.releaseNonNull());
    }
    return result;
}

WebExtensionContext::TabVector WebExtensionContext::openTabs(IgnoreExtensionAccess ignoreExtensionAccess) const
{
    TabVector result;
    for (auto& window : openWindows(ignoreExtensionAccess)) {
        for (auto& tab : window->tabs()) {
            if (ignoreExtensionAccess == IgnoreExtensionAccess::Yes || tab->extensionHasAccess())
                result.append(tab.copyRef());
        }
    }
    return result;
}

RefPtr<WebExtensionWindow> WebExtensionContext::focusedWindow(IgnoreExtensionAccess ignoreExtensionAccess) const
{
    RefPtr controller = extensionController();
    if (!isLoaded() || !controller)
        return nullptr;
    auto identifier = controller->nativeBrowserTabs()->focusedWindow();
    RefPtr window = identifier ? getOrCreateNativeWindow(*identifier) : nullptr;
    if (window && ignoreExtensionAccess == IgnoreExtensionAccess::No && !window->extensionHasAccess())
        return nullptr;
    return window;
}

RefPtr<WebExtensionWindow> WebExtensionContext::frontmostWindow(IgnoreExtensionAccess ignoreExtensionAccess) const
{
    auto windows = openWindows(ignoreExtensionAccess);
    return windows.isEmpty() ? nullptr : windows[0].ptr();
}

void WebExtensionContext::populateWindowsAndTabs()
{
    ASSERT(isLoaded());
    m_windowOrderVector.clear();
    m_nativeWindowEventIdentifiers.clear();
    m_nativeTabEventIdentifiers.clear();
    m_focusedWindowIdentifier = std::nullopt;
    for (auto& window : openWindows(IgnoreExtensionAccess::Yes)) {
        m_windowOrderVector.append(window->identifier());
        m_nativeWindowEventIdentifiers.set(window->nativeWindowIdentifier(), window->identifier());
        // Tabs that predate this load are known, not created: remember their
        // public identity so only later registry changes produce tab events.
        for (auto& tab : window->tabs()) {
            if (RefPtr page = tab->nativePage())
                m_nativeTabEventIdentifiers.set(page->identifier(), std::pair { tab->identifier(), window->identifier() });
        }
        if (window->isFocused())
            m_focusedWindowIdentifier = window->identifier();
    }
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
