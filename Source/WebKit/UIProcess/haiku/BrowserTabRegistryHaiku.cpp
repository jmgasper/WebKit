/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "BrowserTabRegistryHaiku.h"

#include <wtf/HashSet.h>
#include <wtf/MainThread.h>

namespace WebKit {

void BrowserTabRegistryHaiku::publish(Vector<Window>&& windows, std::optional<uint64_t> focusedWindow)
{
    bool changed = focusedWindow != m_focusedWindow || windows.size() != m_windows.size();
    for (size_t i = 0; !changed && i < windows.size(); ++i) {
        auto& oldWindow = m_windows[i];
        auto& newWindow = windows[i];
        changed = oldWindow.identifier != newWindow.identifier || oldWindow.messenger != newWindow.messenger
            || oldWindow.pages != newWindow.pages || oldWindow.activePage != newWindow.activePage;
    }
    if (!changed)
        return;
    Snapshot before { WTF::move(m_windows), m_focusedWindow };
    m_windows = WTF::move(windows);
    m_focusedWindow = focusedWindow;
    // Copy both the callback and new state so a nested update or unregister
    // cannot invalidate the arguments of the current callback.
    if (auto observer = m_observer) {
        Snapshot after { m_windows, m_focusedWindow };
        observer(before, after);
    }
}

std::expected<uint64_t, status_t> BrowserTabRegistryHaiku::update(const BMessenger& messenger, const Vector<WebPageProxyIdentifier>& pages,
    std::optional<WebPageProxyIdentifier> activePage, bool focused)
{
    ASSERT(isMainThread());
    HashSet<WebPageProxyIdentifier> incomingPages;
    for (auto page : pages) {
        if (!incomingPages.add(page).isNewEntry)
            return makeUnexpected(B_BAD_VALUE);
    }
    if (activePage && !incomingPages.contains(*activePage))
        return makeUnexpected(B_BAD_VALUE);
    auto existing = m_windows.findIf([&](auto& window) { return window.messenger == messenger; });
    if (pages.isEmpty()) {
        if (existing == notFound)
            return uint64_t { 0 };
        auto identifier = m_windows[existing].identifier;
        auto windows = m_windows;
        windows.removeAt(existing);
        publish(WTF::move(windows), m_focusedWindow == identifier ? std::nullopt : m_focusedWindow);
        return identifier;
    }
    if (!messenger.IsValid())
        return makeUnexpected(B_BAD_VALUE);
    if (existing == notFound && m_nextWindowIdentifier == std::numeric_limits<uint64_t>::max())
        return makeUnexpected(B_ERROR);

    auto identifier = existing != notFound ? m_windows[existing].identifier : m_nextWindowIdentifier++;
    // Validate first, then publish a complete replacement. A move may affect
    // several windows, but observers only see the final ownership and order.
    auto windows = m_windows;
    for (auto& window : windows) {
        if (window.identifier == identifier)
            continue;
        window.pages.removeAllMatching([&](auto page) { return incomingPages.contains(page); });
        if (window.activePage && !window.pages.contains(*window.activePage))
            window.activePage = std::nullopt;
    }
    windows.removeAllMatching([](auto& window) { return window.pages.isEmpty(); });
    auto index = windows.findIf([&](auto& window) { return window.identifier == identifier; });
    Window replacement { identifier, messenger, pages, activePage };
    if (index == notFound) {
        windows.append(WTF::move(replacement));
        index = windows.size() - 1;
    } else
        windows[index] = WTF::move(replacement);
    auto focusedWindow = m_focusedWindow;
    if (focused) {
        focusedWindow = identifier;
        if (index) {
            auto window = WTF::move(windows[index]);
            windows.removeAt(index);
            windows.insert(0, WTF::move(window));
        }
    } else if (focusedWindow == identifier)
        focusedWindow = std::nullopt;
    if (focusedWindow && !windows.containsIf([&](auto& window) { return window.identifier == *focusedWindow; }))
        focusedWindow = std::nullopt;
    publish(WTF::move(windows), focusedWindow);
    return identifier;
}

void BrowserTabRegistryHaiku::removePage(WebPageProxyIdentifier identifier)
{
    ASSERT(isMainThread());
    auto windows = m_windows;
    for (auto& window : windows) {
        window.pages.removeFirst(identifier);
        if (window.activePage == identifier)
            window.activePage = std::nullopt;
    }
    windows.removeAllMatching([](auto& window) { return window.pages.isEmpty(); });
    auto focusedWindow = m_focusedWindow;
    if (focusedWindow && !windows.containsIf([&](auto& window) { return window.identifier == *focusedWindow; }))
        focusedWindow = std::nullopt;
    publish(WTF::move(windows), focusedWindow);
}

void BrowserTabRegistryHaiku::clear()
{
    ASSERT(isMainThread());
    publish({ }, std::nullopt);
    // Do not recycle IDs: a retained extension window must stay invalid.
}

Vector<BrowserTabRegistryHaiku::Window> BrowserTabRegistryHaiku::windows() const
{
    ASSERT(isMainThread());
    return m_windows;
}

std::optional<BrowserTabRegistryHaiku::Window> BrowserTabRegistryHaiku::window(uint64_t identifier) const
{
    ASSERT(isMainThread());
    auto index = m_windows.findIf([&](auto& window) { return window.identifier == identifier; });
    if (index == notFound)
        return std::nullopt;
    return m_windows[index];
}

std::optional<BrowserTabRegistryHaiku::Tab> BrowserTabRegistryHaiku::tab(WebPageProxyIdentifier identifier) const
{
    ASSERT(isMainThread());
    for (auto& window : m_windows) {
        auto index = window.pages.find(identifier);
        if (index != notFound)
            return Tab { window.identifier, index, window.activePage == identifier };
    }
    return std::nullopt;
}

std::optional<uint64_t> BrowserTabRegistryHaiku::focusedWindow() const
{
    ASSERT(isMainThread());
    return m_focusedWindow;
}

} // namespace WebKit
