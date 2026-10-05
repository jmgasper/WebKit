/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebsiteDataStore.h"
namespace WebKit {
void WebExtensionContext::nativeBrowserWindowsDidChange(const Vector<uint64_t>& before, const Vector<uint64_t>& after,
    std::optional<uint64_t> previousFocus, std::optional<uint64_t> newFocus)
{
    if (!isLoaded())
        return;
    RefPtr controller = extensionController();
    if (!controller)
        return;
    validateNativeActionPopup();
    bool allowed = controller->configuration().defaultWebsiteDataStore().isPersistent() || hasAccessToPrivateData();
    Vector<WebExtensionWindowParameters> removed;
    // A dead native messenger may already have pruned its wrapper. Keep the
    // original public identity until the registry reports the actual removal.
    for (auto identifier : before) {
        if (after.contains(identifier))
            continue;
        auto cached = m_nativeWindowEventIdentifiers.find(identifier);
        if (cached == m_nativeWindowEventIdentifiers.end())
            continue;
        WebExtensionWindowParameters parameters;
        parameters.identifier = cached->value;
        parameters.type = WebExtensionWindow::Type::Normal;
        removed.append(WTF::move(parameters));
        m_nativeWindowEventIdentifiers.remove(cached);
    }
    Vector<WebExtensionWindowParameters> created;
    std::optional<WebExtensionWindowParameters> focused;
    // A private window the extension may not see is not reported (it is
    // neither created nor removed for it), and while it has the focus the
    // extension's last focused window stays the one it saw last, as in Chrome.
    auto previousFocusedWindow = m_focusedWindowIdentifier;
    bool focusHidden = false;
    m_windowOrderVector.clear();
    m_focusedWindowIdentifier = std::nullopt;
    for (Ref window : openWindows(IgnoreExtensionAccess::Yes)) {
        auto identifier = window->nativeWindowIdentifier();
        bool visible = window->extensionHasAccess();
        m_windowOrderVector.append(window->identifier());
        if (visible)
            m_nativeWindowEventIdentifiers.set(identifier, window->identifier());
        else
            m_nativeWindowEventIdentifiers.remove(identifier);
        if (identifier == newFocus) {
            if (visible) {
                focused = window->minimalParameters();
                m_focusedWindowIdentifier = window->identifier();
            } else
                focusHidden = true;
        }
        if (!before.contains(identifier) && after.contains(identifier)) {
            window->tabs();
            if (allowed && visible)
                created.append(window->parameters());
        }
    }
    if (focusHidden)
        m_focusedWindowIdentifier = previousFocusedWindow;
    if (!allowed)
        return;
    for (auto& window : removed)
        fireWindowsEventIfNeeded(WebExtensionEventListenerType::WindowsOnRemoved, WTF::move(window));
    for (auto& window : created)
        fireWindowsEventIfNeeded(WebExtensionEventListenerType::WindowsOnCreated, WTF::move(window));
    if (previousFocus != newFocus)
        fireWindowsEventIfNeeded(WebExtensionEventListenerType::WindowsOnFocusChanged, WTF::move(focused));
}

void WebExtensionContext::fireWindowsEventIfNeeded(WebExtensionEventListenerType type, std::optional<WebExtensionWindowParameters> window)
{
    if (!isLoaded())
        return;
    auto loadIdentifier = privilegedIdentifier();
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, type, window = WTF::move(window), loadIdentifier] {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier)
            return;
        RefPtr controller = extensionController();
        if (!controller || (!controller->configuration().defaultWebsiteDataStore().isPersistent() && !hasAccessToPrivateData()))
            return;
        // Send the committed transition even if a later update changed focus
        // or removed the window while background content was waking.
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchWindowsEvent(type, window));
    });
}
}
#endif
