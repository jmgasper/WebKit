/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserCommandBrokerHaiku.h"
#include "BrowserTabRegistryHaiku.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebExtensionUtilities.h"
#include "WebKitContext.h"
#include "WebPageProxy.h"
#include "WebViewPrivate.h"
#include "WebsiteDataStore.h"
#include <Message.h>
#include <wtf/HashSet.h>

namespace WebKit {

static String nativeBrowserCommandError(const BrowserCommandBrokerHaiku::Result& result)
{
    // The host's text reaches extension script; keep it short and printable.
    if (!result.error.isEmpty() && !result.error.contains('\0'))
        return result.error.left(512);
    switch (result.status) {
    case B_NO_INIT:
    case B_NOT_SUPPORTED:
        return "it is not implemented"_s;
    case B_TIMED_OUT:
        return "the browser did not respond in time"_s;
    case B_CANCELED:
        return "the request was cancelled"_s;
    case B_NOT_ALLOWED:
    case B_PERMISSION_DENIED:
        return "the browser did not allow the request"_s;
    case B_BUSY:
        return "too many browser requests are pending"_s;
    default:
        return "the browser could not complete the request"_s;
    }
}

void WebExtensionContext::sendNativeBrowserCommand(BMessage&& message, const String& apiName, CompletionHandler<void(NativeBrowserCommandResult&&)>&& completion)
{
    RefPtr controller = extensionController();
    auto commands = controller ? controller->nativeBrowserCommands() : nullptr;
    if (!isLoaded() || !commands) {
        // Keep the asynchronous contract of the broker for every failure.
        RunLoop::mainSingleton().dispatch([apiName = apiName.isolatedCopy(), completion = WTF::move(completion)]() mutable {
            completion(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
        });
        return;
    }
    // Attribution only: the host decides how to present extension-made changes.
    message.AddString("extension_identifier", uniqueIdentifier().utf8().legacyCStringPointer());
    message.AddString("extension_name", protect(extension())->displayName().utf8().legacyCStringPointer());
    auto loadIdentifier = privilegedIdentifier();
    commands->request(loadIdentifier.toUInt64(), WTF::move(message),
        [this, protectedThis = Ref { *this }, loadIdentifier, apiName, completion = WTF::move(completion)](BrowserCommandBrokerHaiku::Result&& result) mutable {
            // The same context object can be unloaded and loaded again while
            // the host works. Its privileged identifier is per load.
            if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier) {
                completion(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
                return;
            }
            if (result.status != B_OK) {
                completion(toWebExtensionError(apiName, nullString(), nativeBrowserCommandError(result)));
                return;
            }
            completion(WTF::move(result.createdPages));
        });
}

bool WebExtensionContext::addNativeBrowserCommandTab(BMessage& message, const WebExtensionTab& tab, const char* viewField, const char* pageField) const
{
    RefPtr page = tab.nativePage();
    if (!page || !tab.isValid())
        return false;
    // Registered pages always belong to a native BWebKitView bridge.
    auto* client = static_cast<WebView*>(page->pageClient());
    if (!client || client->page() != page.get())
        return false;
    auto view = client->nativeView();
    if (!view.IsValid())
        return false;
    return message.AddMessenger(viewField, view) == B_OK
        && message.AddUInt64(pageField, page->identifier().toUInt64()) == B_OK;
}

bool WebExtensionContext::addNativeBrowserCommandWindow(BMessage& message, const WebExtensionWindow& window) const
{
    RefPtr controller = extensionController();
    if (!controller || !window.isValid())
        return false;
    auto state = controller->nativeBrowserTabs()->window(window.nativeWindowIdentifier());
    if (!state || !state->messenger.IsValid())
        return false;
    return message.AddMessenger("window", state->messenger) == B_OK
        && message.AddUInt64("window_identifier", state->identifier) == B_OK;
}

void WebExtensionContext::nativeBrowserTabsWillChange(const Vector<NativeBrowserTabState>&, const Vector<NativeBrowserTabState>& after)
{
    if (!isLoaded())
        return;
    RefPtr controller = extensionController();
    if (!controller)
        return;
    bool allowed = controller->configuration().defaultWebsiteDataStore().isPersistent() || hasAccessToPrivateData();

    HashSet<WebPageProxyIdentifier> remainingPages;
    HashSet<uint64_t> remainingWindows;
    for (auto& state : after) {
        remainingPages.add(state.page);
        remainingWindows.add(state.window);
    }
    // Report from the cached public identity. A dead native messenger or the
    // page close itself may already have pruned the wrapper objects.
    Vector<WebPageProxyIdentifier> removedPages;
    for (auto& entry : m_nativeTabEventIdentifiers) {
        if (!remainingPages.contains(entry.key))
            removedPages.append(entry.key);
    }
    for (auto page : removedPages) {
        auto cached = m_nativeTabEventIdentifiers.find(page);
        auto identity = cached->value;
        m_nativeTabEventIdentifiers.remove(cached);
        bool windowIsClosing = false;
        for (auto& entry : m_nativeWindowEventIdentifiers) {
            if (entry.value == identity.second) {
                windowIsClosing = !remainingWindows.contains(entry.key);
                break;
            }
        }
        if (allowed)
            fireTabsRemovedEventIfNeeded(identity.first, identity.second, windowIsClosing ? WindowIsClosing::Yes : WindowIsClosing::No);
    }
}

void WebExtensionContext::nativeBrowserTabsDidChange(const Vector<NativeBrowserTabState>& before, const Vector<NativeBrowserTabState>& after)
{
    if (!isLoaded())
        return;
    RefPtr controller = extensionController();
    if (!controller)
        return;
    bool allowed = controller->configuration().defaultWebsiteDataStore().isPersistent() || hasAccessToPrivateData();

    HashMap<WebPageProxyIdentifier, size_t> previousIndexes;
    for (size_t index = 0; index < before.size(); ++index)
        previousIndexes.set(before[index].page, index);

    for (auto& state : after) {
        RefPtr tab = getOrCreateNativeTab(state.page);
        RefPtr window = getOrCreateNativeWindow(state.window);
        if (!tab || !window)
            continue;
        if (!tab->extensionHasAccess()) {
            // A private tab of an extension not allowed there: it never hears of it.
            m_nativeTabEventIdentifiers.remove(state.page);
            continue;
        }
        auto cached = m_nativeTabEventIdentifiers.find(state.page);
        bool unseen = cached == m_nativeTabEventIdentifiers.end();
        auto previousWindow = unseen ? window->identifier() : cached->value.second;
        m_nativeTabEventIdentifiers.set(state.page, std::pair { tab->identifier(), window->identifier() });
        if (!allowed)
            continue;
        if (unseen) {
            // A tab this context has not seen may still predate it (the context was loaded
            // after the tab opened). Only tabs absent from the previous registry are created;
            // reporting older ones made Dark Reader skip injecting into tabs it never styled.
            if (!previousIndexes.contains(state.page))
                fireTabsCreatedEventIfNeeded(tab->parameters());
            continue;
        }
        if (previousWindow != window->identifier()) {
            auto previous = previousIndexes.find(state.page);
            size_t oldIndex = previous != previousIndexes.end() ? before[previous->value].index : 0;
            fireTabsDetachedEventIfNeeded(tab->identifier(), previousWindow, oldIndex);
            fireTabsAttachedEventIfNeeded(tab->identifier(), window->identifier(), state.index);
        }
    }
    if (!allowed)
        return;

    for (auto& state : after) {
        if (!state.active)
            continue;
        std::optional<WebPageProxyIdentifier> previousActivePage;
        for (auto& old : before) {
            if (old.window == state.window && old.active) {
                previousActivePage = old.page;
                break;
            }
        }
        if (previousActivePage == state.page)
            continue;
        auto current = m_nativeTabEventIdentifiers.find(state.page);
        if (current == m_nativeTabEventIdentifiers.end())
            continue;
        auto previousIdentifier = WebExtensionTabConstants::NoneIdentifier;
        if (previousActivePage) {
            // A closed previous tab has just been reported as removed.
            auto previous = m_nativeTabEventIdentifiers.find(*previousActivePage);
            if (previous != m_nativeTabEventIdentifiers.end())
                previousIdentifier = previous->value.first;
        }
        fireTabsActivatedEventIfNeeded(previousIdentifier, current->value.first, current->value.second);
    }
}

bool WebExtensionContext::canDeliverNativeTabEvent(WebExtensionContextIdentifier loadIdentifier) const
{
    // The same context object can be unloaded and loaded again while background
    // content wakes, and private access can be withdrawn in the meantime.
    if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier)
        return false;
    RefPtr controller = extensionController();
    return controller && (controller->configuration().defaultWebsiteDataStore().isPersistent() || hasAccessToPrivateData());
}

void WebExtensionContext::fireTabsCreatedEventIfNeeded(const WebExtensionTabParameters& parameters)
{
    if (!isLoaded())
        return;
    constexpr auto type = WebExtensionEventListenerType::TabsOnCreated;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, parameters, loadIdentifier = privilegedIdentifier()] {
        if (canDeliverNativeTabEvent(loadIdentifier))
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsCreatedEvent(parameters));
    });
}

void WebExtensionContext::fireTabsDetachedEventIfNeeded(WebExtensionTabIdentifier tabIdentifier, WebExtensionWindowIdentifier oldWindowIdentifier, size_t oldIndex)
{
    if (!isLoaded())
        return;
    constexpr auto type = WebExtensionEventListenerType::TabsOnDetached;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, tabIdentifier, oldWindowIdentifier, oldIndex, loadIdentifier = privilegedIdentifier()] {
        if (canDeliverNativeTabEvent(loadIdentifier))
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsDetachedEvent(tabIdentifier, oldWindowIdentifier, oldIndex));
    });
}

void WebExtensionContext::fireTabsAttachedEventIfNeeded(WebExtensionTabIdentifier tabIdentifier, WebExtensionWindowIdentifier newWindowIdentifier, size_t newIndex)
{
    if (!isLoaded())
        return;
    constexpr auto type = WebExtensionEventListenerType::TabsOnAttached;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, tabIdentifier, newWindowIdentifier, newIndex, loadIdentifier = privilegedIdentifier()] {
        if (canDeliverNativeTabEvent(loadIdentifier))
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsAttachedEvent(tabIdentifier, newWindowIdentifier, newIndex));
    });
}

void WebExtensionContext::fireTabsActivatedEventIfNeeded(WebExtensionTabIdentifier previousActiveTabIdentifier, WebExtensionTabIdentifier newActiveTabIdentifier, WebExtensionWindowIdentifier windowIdentifier)
{
    if (!isLoaded())
        return;
    constexpr auto type = WebExtensionEventListenerType::TabsOnActivated;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, previousActiveTabIdentifier, newActiveTabIdentifier, windowIdentifier, loadIdentifier = privilegedIdentifier()] {
        if (canDeliverNativeTabEvent(loadIdentifier))
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsActivatedEvent(previousActiveTabIdentifier, newActiveTabIdentifier, windowIdentifier));
    });
}

void WebExtensionContext::fireTabsRemovedEventIfNeeded(WebExtensionTabIdentifier tabIdentifier, WebExtensionWindowIdentifier windowIdentifier, WindowIsClosing windowIsClosing)
{
    if (!isLoaded())
        return;
    constexpr auto type = WebExtensionEventListenerType::TabsOnRemoved;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, tabIdentifier, windowIdentifier, windowIsClosing, loadIdentifier = privilegedIdentifier()] {
        if (canDeliverNativeTabEvent(loadIdentifier))
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchTabsRemovedEvent(tabIdentifier, windowIdentifier, windowIsClosing));
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
