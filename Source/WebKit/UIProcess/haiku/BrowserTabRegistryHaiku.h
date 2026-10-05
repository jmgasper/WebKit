/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#include "WebPageProxyIdentifier.h"
#include <Messenger.h>
#include <functional>
#include <SupportDefs.h>
#include <wtf/Vector.h>

namespace WebKit {

// Main-thread metadata only. The native SDK validates page ownership before
// updating this profile's registry; WebKit retains ownership of the pages.
class BrowserTabRegistryHaiku {
public:
    struct Window {
        uint64_t identifier;
        BMessenger messenger;
        Vector<WebPageProxyIdentifier> pages;
        std::optional<WebPageProxyIdentifier> activePage;
    };
    struct Tab {
        uint64_t windowIdentifier;
        size_t index;
        bool active;
    };

    struct Snapshot {
        Vector<Window> windows;
        std::optional<uint64_t> focusedWindow;
    };
    using Observer = std::function<void(const Snapshot& before, const Snapshot& after)>;
    // Observers see committed, independent snapshots and may unregister while
    // running. Setting an observer does not synthesize creation events.
    void setObserver(Observer observer) { m_observer = WTF::move(observer); }

    // An empty page list removes the window. Moving a page removes it from its
    // previous window. Focused windows move to the front of the window order.
    std::expected<uint64_t, status_t> update(const BMessenger&, const Vector<WebPageProxyIdentifier>&,
        std::optional<WebPageProxyIdentifier> activePage, bool focused);
    void removePage(WebPageProxyIdentifier);
    void clear();

    Vector<Window> windows() const;
    std::optional<Window> window(uint64_t) const;
    std::optional<Tab> tab(WebPageProxyIdentifier) const;
    std::optional<uint64_t> focusedWindow() const;

private:
    void publish(Vector<Window>&&, std::optional<uint64_t> focusedWindow);
    Observer m_observer;
    Vector<Window> m_windows;
    std::optional<uint64_t> m_focusedWindow;
    uint64_t m_nextWindowIdentifier { 1 };
};

} // namespace WebKit
