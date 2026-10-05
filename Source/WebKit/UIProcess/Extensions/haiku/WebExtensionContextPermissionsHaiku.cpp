/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContextProxyMessages.h"
#include "ExtensionPermissionPromptHaiku.h"
#include "WebExtensionPermission.h"
#include "WebExtensionPrivacyControllerHaiku.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include <wtf/MainThread.h>

namespace WebKit {

void WebExtensionContext::userGesturePerformed(WebExtensionTab& tab)
{
    ASSERT(isMainThread());
    if (!isLoaded() || !isValidTab(tab) || !tab.isOpen() || !tab.extensionHasAccess())
        return;
    tab.setActiveUserGesture(true);
    if (!hasPermission(WebExtensionPermission::activeTab()) || !tab.shouldGrantPermissionsOnUserGesture())
        return;
    // Read the committed security origin, never a provisional URL shown in the
    // address bar. This also rejects opaque documents and extension pages.
    auto origin = tab.nativeCommittedOrigin();
    auto state = permissionState(origin, nullptr, PermissionStateOptions::SkipRequestedPermissions);
    if (state == PermissionState::DeniedExplicitly || state == PermissionState::DeniedImplicitly)
        return;
    bool alreadyGranted = tab.extensionHasTemporaryPermission();
    if (!tab.grantNativeTemporaryPermission() || alreadyGranted)
        return;
    // Temporary access belongs to this tab. Global permission notifications
    // would install content scripts in every tab matching the origin.
    didChangeTabProperties(tab, { WebExtensionTab::ChangedProperties::URL, WebExtensionTab::ChangedProperties::Title });
}

void WebExtensionContext::setNativePermissionChangeHandler(RefPtr<NativePermissionChangeHandler>&& handler)
{
    ASSERT(isMainThread());
    RELEASE_ASSERT(++m_nativePermissionChangeHandlerGeneration);
    m_nativePermissionChangeHandler = WTF::move(handler);
}

void WebExtensionContext::notifyNativePermissionChange(PermissionNotification notification, const PermissionsSet& permissions, const MatchPatternSet& matchPatterns)
{
    ASSERT(isMainThread());
    if (isLoaded() && (permissions.contains(WebExtensionPermission::declarativeNetRequest()) || permissions.contains(WebExtensionPermission::declarativeNetRequestWithHostAccess()))) {
        if (nativeDeclarativeNetRequestAllowed())
            loadDeclarativeNetRequestRules([](bool) { });
        else
            clearNativeDeclarativeNetRequestRules();
    }
    RefPtr handler = m_nativePermissionChangeHandler;
    if (!handler || notification == PermissionNotification::None)
        return;

    auto generation = m_nativePermissionChangeHandlerGeneration;
    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }, handler = WTF::move(handler), generation, notification, permissions, matchPatterns] {
        // Removing or replacing the observer cancels its queued deliveries.
        // Keep the active task alive if it replaces itself during the callback.
        if (m_nativePermissionChangeHandlerGeneration == generation && m_nativePermissionChangeHandler == handler)
            handler->run(notification, permissions, matchPatterns);
    });
}

void WebExtensionContext::permissionsDidChange(PermissionNotification notification, const PermissionsSet& permissions)
{
    if (permissions.isEmpty() || notification == PermissionNotification::None)
        return;
    if (isLoaded() && permissions.contains(WebExtensionPermission::activeTab())
        && (notification == PermissionNotification::GrantedPermissionsWereRemoved || notification == PermissionNotification::PermissionsWereDenied)) {
        for (Ref tab : openTabs(IgnoreExtensionAccess::Yes))
            clearUserGesture(tab);
    }
    if (notification == PermissionNotification::GrantedPermissionsWereRemoved || notification == PermissionNotification::PermissionsWereDenied) {
        RELEASE_ASSERT(++m_nativePermissionRevocationGeneration);
        if (isLoaded()) {
            if (RefPtr controller = extensionController())
                controller->nativePermissionPrompts()->cancelOwner(privilegedIdentifier().toUInt64());
        }
    }

    if (isLoaded()) {
        RefPtr controller = extensionController();
        if (!controller)
            return;

        controller->sendToAllProcesses(Messages::WebExtensionContextProxy::UpdateGrantedPermissions(m_grantedPermissions), identifier());
        if (permissions.contains(WebExtensionPermission::privacy())) {
            RELEASE_ASSERT(++m_nativePrivacyGeneration);
            controller->nativePrivacy().refresh();
        }

        if (permissions.contains(WebExtensionPermission::clipboardWrite())) {
            bool granted = hasPermission(WebExtensionPermission::clipboardWrite());
            enumerateExtensionPages([&](auto& page, bool&) {
                page.preferences().setJavaScriptCanAccessClipboard(granted);
            });
        }

        if (notification == PermissionNotification::PermissionsWereGranted)
            firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnAdded, permissions, { });
        else if (notification == PermissionNotification::GrantedPermissionsWereRemoved)
            firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnRemoved, permissions, { });
    }

    notifyNativePermissionChange(notification, permissions, { });
}

void WebExtensionContext::permissionsDidChange(PermissionNotification notification, const MatchPatternSet& matchPatterns)
{
    if (matchPatterns.isEmpty() || notification == PermissionNotification::None)
        return;
    if (isLoaded() && (notification == PermissionNotification::GrantedPermissionMatchPatternsWereRemoved || notification == PermissionNotification::PermissionMatchPatternsWereDenied)) {
        for (Ref tab : openTabs(IgnoreExtensionAccess::Yes)) {
            if (tab->nativeTemporaryPermissionMatches(matchPatterns))
                clearUserGesture(tab);
        }
    }
    if (notification == PermissionNotification::GrantedPermissionMatchPatternsWereRemoved || notification == PermissionNotification::PermissionMatchPatternsWereDenied) {
        RELEASE_ASSERT(++m_nativePermissionRevocationGeneration);
        if (isLoaded()) {
            if (RefPtr controller = extensionController())
                controller->nativePermissionPrompts()->cancelOwner(privilegedIdentifier().toUInt64());
        }
    }

    clearCachedPermissionStates();

    if (isLoaded()) {
        updateCORSDisablingPatternsOnAllExtensionPages();

        if (notification == PermissionNotification::PermissionMatchPatternsWereGranted) {
            addInjectedContent(injectedContents(), matchPatterns);
            firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnAdded, { }, matchPatterns);
        } else if (notification == PermissionNotification::GrantedPermissionMatchPatternsWereRemoved) {
            removeInjectedContent(matchPatterns);
            firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType::PermissionsOnRemoved, { }, matchPatterns);
        } else
            updateInjectedContent();

        constexpr auto changedProperties = OptionSet { WebExtensionTab::ChangedProperties::URL, WebExtensionTab::ChangedProperties::Title };
        for (Ref tab : openTabs()) {
            auto url = tab->url();
            for (auto& pattern : matchPatterns) {
                if (pattern->matchesURL(url)) {
                    didChangeTabProperties(tab, changedProperties);
                    break;
                }
            }
        }
    }

    notifyNativePermissionChange(notification, { }, matchPatterns);
}

void WebExtensionContext::firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType type, const PermissionsSet& permissions, const MatchPatternSet& matchPatterns)
{
    ASSERT(type == WebExtensionEventListenerType::PermissionsOnAdded || type == WebExtensionEventListenerType::PermissionsOnRemoved);
    if (!isLoaded() || m_nativePermissionTransaction)
        return;

    auto loadIdentifier = privilegedIdentifier();
    auto origins = toStrings(matchPatterns);
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, type, permissions, origins = WTF::move(origins), loadIdentifier] {
        // The same context object can be unloaded and loaded again while a
        // background task waits. Its privileged identifier is per load.
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier)
            return;
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchPermissionsEvent(type, permissions, origins));
    });
}

void WebExtensionContext::didChangeTabProperties(WebExtensionTab& tab, OptionSet<WebExtensionTab::ChangedProperties> properties)
{
    if (!isLoaded() || !isValidTab(tab) || !tab.isOpen() || !tab.extensionHasAccess())
        return;
    if (properties.isEmpty())
        properties = WebExtensionTab::allChangedProperties();

    bool hasChangesPending = !tab.changedProperties().isEmpty();
    tab.addChangedProperties(properties);
    if (hasChangesPending)
        return;

    auto loadIdentifier = privilegedIdentifier();
    RunLoop::mainSingleton().dispatchAfter(25_ms, [this, protectedThis = Ref { *this }, tab = Ref { tab }, loadIdentifier] {
        auto changes = tab->changedProperties();
        tab->clearChangedProperties();
        if (changes.isEmpty() || !isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier
            || !isValidTab(tab) || !tab->isOpen() || !tab->extensionHasAccess())
            return;

        constexpr auto type = WebExtensionEventListenerType::TabsOnUpdated;
        wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, tab, changes, loadIdentifier] {
            if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier
                || !isValidTab(tab) || !tab->isOpen() || !tab->extensionHasAccess())
                return;

            // Construct both payloads at delivery, after revalidating identity
            // and access. Never retain URL/title data across a permission change.
            auto parameters = tab->parameters();
            auto changedParameters = tab->changedParameters(changes);
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsUpdatedEvent(parameters, changedParameters));
        });
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
