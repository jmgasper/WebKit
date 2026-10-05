/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserCommandBrokerHaiku.h"
#include "WebExtensionController.h"
#include "WebExtensionTabParameters.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionWindowParameters.h"
#include "WebKitContext.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <Message.h>
#include <limits>
#include <wtf/HashSet.h>

// Ported from Cocoa/API/WebExtensionContextAPITabsCocoa.mm and
// WebExtensionContextAPIWindowsCocoa.mm. Where those call the embedder's
// WKWebExtensionControllerDelegate, this port sends a browser command to the
// native host registered through BWebKitContext::SetBrowserCommandListener.

namespace WebKit {

// Extensions may send the browser to web content, a blank page and their own
// pages. Script, data, local-file, other extensions' and browser-internal URLs
// stay out of reach, which matches what Firefox accepts for these APIs.
static bool canOpenURLInNativeBrowser(const WebExtensionContext& context, const URL& url)
{
    if (!url.isValid())
        return false;
    if (url.protocolIsInHTTPFamily() || url.isAboutBlank())
        return true;
    return context.isURLForThisExtension(url);
}

static bool hasNativeBrowserCommandListener(const WebExtensionController& controller)
{
    auto commands = controller.nativeBrowserCommands();
    return commands && commands->hasListener();
}

void WebExtensionContext::tabsCreate(std::optional<WebPageProxyIdentifier> webPageProxyIdentifier, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&& completionHandler)
{
    ASSERT(!parameters.audible);
    ASSERT(!parameters.loading);
    ASSERT(!parameters.privateBrowsing);
    ASSERT(!parameters.readerModeAvailable);

    static constexpr auto apiName = "tabs.create()"_s;

    RefPtr extensionController = this->extensionController();
    if (!extensionController) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
        return;
    }

    if (!hasNativeBrowserCommandListener(*extensionController)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "it is not implemented"_s));
        return;
    }

    if (parameters.url && !canOpenURLInNativeBrowser(*this, *parameters.url)) {
        completionHandler(toWebExtensionError(apiName, "url"_s, "it is not a URL this extension can open"_s));
        return;
    }

    // `pinned: true` is ignored (no pinned tabs in Summit); see WebExtensionTab::setPinned().

    if (parameters.showingReaderMode.value_or(false)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "it is not implemented for 'openInReaderMode' set to `true`"_s));
        return;
    }

    RefPtr window = getWindow(parameters.windowIdentifier.value_or(WebExtensionWindowConstants::CurrentIdentifier), webPageProxyIdentifier);
    if (parameters.windowIdentifier && !window) {
        completionHandler(toWebExtensionError(apiName, nullString(), "window not found"_s));
        return;
    }

    RefPtr<WebExtensionTab> parentTab;
    if (parameters.parentTabIdentifier) {
        parentTab = getTab(parameters.parentTabIdentifier.value());
        if (!parentTab) {
            completionHandler(toWebExtensionError(apiName, nullString(), "parent tab not found"_s));
            return;
        }
    }

    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    bool complete = message.AddUInt32("command", B_WEBKIT_BROWSER_OPEN_TAB) == B_OK
        && message.AddBool("active", parameters.active.value_or(true)) == B_OK
        && message.AddInt32("index", parameters.index ? static_cast<int32>(std::min<uint64_t>(*parameters.index, std::numeric_limits<int32>::max())) : -1) == B_OK;
    if (complete && parameters.url)
        complete = message.AddString("url", parameters.url->string().utf8().legacyCStringPointer()) == B_OK;
    // Without an accessible window the host chooses where the tab opens.
    if (complete && window)
        complete = addNativeBrowserCommandWindow(message, *window);
    // The opener is advisory; a host without tab relationships ignores it.
    if (complete && parentTab)
        addNativeBrowserCommandTab(message, *parentTab, "opener", "opener_page_identifier");
    if (!complete) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the window is not available in the browser"_s));
        return;
    }

    sendNativeBrowserCommand(WTF::move(message), apiName, [this, protectedThis = Ref { *this }, muted = parameters.muted.value_or(false), completionHandler = WTF::move(completionHandler)](NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completionHandler(makeUnexpected(result.error()));
            return;
        }

        // The host registers the new view before it answers, so the page is
        // already a tab of one of this profile's windows.
        RefPtr newTab = result->size() == 1 ? getOrCreateNativeTab(result->first()) : nullptr;
        if (!newTab) {
            completionHandler(toWebExtensionError(apiName, nullString(), "the browser did not report the new tab"_s));
            return;
        }

        if (!newTab->extensionHasAccess()) {
            completionHandler({ });
            return;
        }

        if (muted) {
            newTab->mute([newTab, completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&&) mutable {
                completionHandler({ newTab->parameters() });
            });
            return;
        }

        completionHandler({ newTab->parameters() });
    });
}

void WebExtensionContext::tabsUpdate(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&& completionHandler)
{
    ASSERT(!parameters.audible);
    ASSERT(!parameters.index);
    ASSERT(!parameters.loading);
    ASSERT(!parameters.privateBrowsing);
    ASSERT(!parameters.readerModeAvailable);
    ASSERT(!parameters.showingReaderMode);
    ASSERT(!parameters.title);
    ASSERT(!parameters.windowIdentifier);

    static constexpr auto apiName = "tabs.update()"_s;

    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    if (!tab) {
        completionHandler(toWebExtensionError(apiName, nullString(), "tab not found"_s));
        return;
    }

    if (parameters.url && !canOpenURLInNativeBrowser(*this, *parameters.url)) {
        completionHandler(toWebExtensionError(apiName, "url"_s, "it is not a URL this extension can open"_s));
        return;
    }

    auto updateActiveAndSelected = [](WebExtensionTab& tab, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        if (parameters.active.value_or(false) && !tab.isActive()) {
            tab.activate(WTF::move(stepCompletionHandler));
            return;
        }

        if (!parameters.selected) {
            stepCompletionHandler({ });
            return;
        }

        bool shouldSelect = parameters.selected.value();
        if (shouldSelect && !tab.isSelected()) {
            // If active is not explicitly set to false, activate the tab. This matches Firefox.
            if (parameters.active.value_or(true))
                tab.activate(WTF::move(stepCompletionHandler));
            else
                tab.select(WTF::move(stepCompletionHandler));
            return;
        }

        if (!shouldSelect && tab.isSelected()) {
            tab.deselect(WTF::move(stepCompletionHandler));
            return;
        }

        stepCompletionHandler({ });
    };

    auto updateURL = [](WebExtensionTab& tab, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        if (!parameters.url) {
            stepCompletionHandler({ });
            return;
        }

        tab.loadURL(parameters.url.value(), WTF::move(stepCompletionHandler));
    };

    auto updatePinned = [](WebExtensionTab& tab, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        if (!parameters.pinned || parameters.pinned.value() == tab.isPinned()) {
            stepCompletionHandler({ });
            return;
        }

        if (parameters.pinned.value())
            tab.pin(WTF::move(stepCompletionHandler));
        else
            tab.unpin(WTF::move(stepCompletionHandler));
    };

    auto updateMuted = [](WebExtensionTab& tab, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        if (!parameters.muted || parameters.muted.value() == tab.isMuted()) {
            stepCompletionHandler({ });
            return;
        }

        if (parameters.muted.value())
            tab.mute(WTF::move(stepCompletionHandler));
        else
            tab.unmute(WTF::move(stepCompletionHandler));
    };

    auto updateParentTab = [this, protectedThis = Ref { *this }](WebExtensionTab& tab, const WebExtensionTabParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        auto currentParentTab = tab.parentTab();
        auto newParentTab = parameters.parentTabIdentifier ? getTab(parameters.parentTabIdentifier.value()) : nullptr;

        if (currentParentTab == newParentTab) {
            stepCompletionHandler({ });
            return;
        }

        tab.setParentTab(newParentTab, WTF::move(stepCompletionHandler));
    };

    updateActiveAndSelected(*tab, parameters, [tab = Ref { *tab }, parameters, updateURL = WTF::move(updateURL), updatePinned = WTF::move(updatePinned), updateMuted = WTF::move(updateMuted), updateParentTab = WTF::move(updateParentTab), completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& activeOrSelectedResult) mutable {
        if (!activeOrSelectedResult) {
            completionHandler(makeUnexpected(activeOrSelectedResult.error()));
            return;
        }

        updateURL(tab, parameters, [tab, parameters, updatePinned = WTF::move(updatePinned), updateMuted = WTF::move(updateMuted), updateParentTab = WTF::move(updateParentTab), completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& urlResult) mutable {
            if (!urlResult) {
                completionHandler(makeUnexpected(urlResult.error()));
                return;
            }

            updatePinned(tab, parameters, [tab, parameters, updateMuted = WTF::move(updateMuted), updateParentTab = WTF::move(updateParentTab), completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& pinnedResult) mutable {
                if (!pinnedResult) {
                    completionHandler(makeUnexpected(pinnedResult.error()));
                    return;
                }

                updateMuted(tab, parameters, [tab, parameters, updateParentTab = WTF::move(updateParentTab), completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& mutedResult) mutable {
                    if (!mutedResult) {
                        completionHandler(makeUnexpected(mutedResult.error()));
                        return;
                    }

                    updateParentTab(tab, parameters, [tab, completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& parentResult) mutable {
                        if (!parentResult) {
                            completionHandler(makeUnexpected(parentResult.error()));
                            return;
                        }

                        // The host may have closed the tab while it worked.
                        if (!tab->isOpen() || !tab->extensionHasAccess()) {
                            completionHandler({ });
                            return;
                        }

                        completionHandler({ tab->parameters() });
                    });
                });
            });
        });
    });
}

void WebExtensionContext::tabsReload(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, ReloadFromOrigin reloadFromOrigin, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    if (!tab) {
        completionHandler(toWebExtensionError("tabs.reload()"_s, nullString(), "tab not found"_s));
        return;
    }

    tab->reload(reloadFromOrigin, WTF::move(completionHandler));
}

void WebExtensionContext::tabsGoBack(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    if (!tab) {
        completionHandler(toWebExtensionError("tabs.goBack()"_s, nullString(), "tab not found"_s));
        return;
    }

    tab->goBack(WTF::move(completionHandler));
}

void WebExtensionContext::tabsGoForward(WebPageProxyIdentifier webPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> tabIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr tab = getTab(webPageProxyIdentifier, tabIdentifier, IncludeExtensionViews::Yes);
    if (!tab) {
        completionHandler(toWebExtensionError("tabs.goForward()"_s, nullString(), "tab not found"_s));
        return;
    }

    tab->goForward(WTF::move(completionHandler));
}

void WebExtensionContext::tabsRemove(Vector<WebExtensionTabIdentifier> tabIdentifiers, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "tabs.remove()"_s;

    Vector<Ref<WebExtensionTab>> tabs;
    tabs.reserveInitialCapacity(tabIdentifiers.size());
    HashSet<WebExtensionTabIdentifier> seenIdentifiers;

    for (auto& tabIdentifier : tabIdentifiers) {
        RefPtr tab = getTab(tabIdentifier);
        if (!tab || !tab->isOpen()) {
            completionHandler(toWebExtensionError(apiName, nullString(), makeString("tab '"_s, tabIdentifier.toUInt64(), "' was not found"_s)));
            return;
        }
        if (seenIdentifiers.add(tabIdentifier).isNewEntry)
            tabs.append(tab.releaseNonNull());
    }

    if (tabs.isEmpty()) {
        completionHandler({ });
        return;
    }

    // One command names every tab, so the host can close them together and
    // answer once instead of interleaving several close handshakes.
    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    bool complete = message.AddUInt32("command", B_WEBKIT_BROWSER_CLOSE_TABS) == B_OK;
    for (auto& tab : tabs) {
        RefPtr window = tab->window();
        complete = complete && window && addNativeBrowserCommandTab(message, tab) && addNativeBrowserCommandWindow(message, *window);
    }
    if (!complete) {
        completionHandler(toWebExtensionError(apiName, nullString(), "a tab is not available in the browser"_s));
        return;
    }

    sendNativeBrowserCommand(WTF::move(message), apiName, [completionHandler = WTF::move(completionHandler)](NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completionHandler(makeUnexpected(result.error()));
            return;
        }

        completionHandler({ });
    });
}

void WebExtensionContext::windowsCreate(const WebExtensionWindowParameters& creationParameters, CompletionHandler<void(std::expected<std::optional<WebExtensionWindowParameters>, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "windows.create()"_s;

    RefPtr extensionController = this->extensionController();
    if (!extensionController) {
        completionHandler(toWebExtensionError(apiName, nullString(), "the extension is not loaded"_s));
        return;
    }

    if (!hasNativeBrowserCommandListener(*extensionController)) {
        completionHandler(toWebExtensionError(apiName, nullString(), "it is not implemented"_s));
        return;
    }

    // Each native profile is entirely private or entirely persistent, and an
    // extension runs inside exactly one of them.
    bool profileIsPrivate = !extensionController->configuration().defaultWebsiteDataStore().isPersistent();
    if (creationParameters.privateBrowsing && *creationParameters.privateBrowsing != profileIsPrivate) {
        completionHandler(toWebExtensionError(apiName, "incognito"_s, profileIsPrivate
            ? "only private windows can be opened from a private profile"_s : "private windows cannot be opened from this profile"_s));
        return;
    }
    if (profileIsPrivate && !hasAccessToPrivateData()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "this extension does not have access to private windows"_s));
        return;
    }

    auto stateName = [](WebExtensionWindow::State state) {
        switch (state) {
        case WebExtensionWindow::State::Normal:
            return "normal";
        case WebExtensionWindow::State::Minimized:
            return "minimized";
        case WebExtensionWindow::State::Maximized:
            return "maximized";
        case WebExtensionWindow::State::Fullscreen:
            return "fullscreen";
        }
        return "normal";
    };

    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    bool popup = creationParameters.type.value_or(WebExtensionWindow::Type::Normal) == WebExtensionWindow::Type::Popup;
    bool complete = message.AddUInt32("command", B_WEBKIT_BROWSER_OPEN_WINDOW) == B_OK
        && message.AddBool("focused", creationParameters.focused.value_or(true)) == B_OK
        && message.AddBool("private", profileIsPrivate) == B_OK
        && message.AddString("type", popup ? "popup" : "normal") == B_OK
        && message.AddString("state", stateName(creationParameters.state.value_or(WebExtensionWindow::State::Normal))) == B_OK;

    if (creationParameters.tabs) {
        for (auto& tabParameters : creationParameters.tabs.value()) {
            if (tabParameters.identifier) {
                RefPtr tab = getTab(tabParameters.identifier.value());
                if (!tab) {
                    completionHandler(toWebExtensionError(apiName, nullString(), makeString("tab '"_s, tabParameters.identifier.value().toUInt64(), "' was not found"_s)));
                    return;
                }

                complete = complete && addNativeBrowserCommandTab(message, *tab);
            } else if (tabParameters.url) {
                if (!canOpenURLInNativeBrowser(*this, *tabParameters.url)) {
                    completionHandler(toWebExtensionError(apiName, "url"_s, "it is not a URL this extension can open"_s));
                    return;
                }

                complete = complete && message.AddString("url", tabParameters.url->string().utf8().legacyCStringPointer()) == B_OK;
            }
        }
    }

    if (!complete) {
        completionHandler(toWebExtensionError(apiName, nullString(), "a tab is not available in the browser"_s));
        return;
    }

    sendNativeBrowserCommand(WTF::move(message), apiName, [this, protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler)](NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completionHandler(makeUnexpected(result.error()));
            return;
        }

        // The window is whichever registered window now owns the tabs the
        // host created; a single-window host reports its existing window.
        RefPtr firstTab = result->isEmpty() ? nullptr : getOrCreateNativeTab(result->first());
        RefPtr window = firstTab ? firstTab->window() : nullptr;
        if (!window) {
            completionHandler(toWebExtensionError(apiName, nullString(), "the browser did not report the new window"_s));
            return;
        }

        completionHandler(window->extensionHasAccess() ? std::optional(window->parameters(WebExtensionWindow::PopulateTabs::Yes)) : std::nullopt);
    });
}

void WebExtensionContext::windowsUpdate(WebExtensionWindowIdentifier windowIdentifier, WebExtensionWindowParameters updateParameters, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "windows.update()"_s;

    RefPtr window = getWindow(windowIdentifier);
    if (!window) {
        completionHandler(toWebExtensionError(apiName, nullString(), "window not found"_s));
        return;
    }

    auto updateState = [](WebExtensionWindow& window, const WebExtensionWindowParameters& updateParameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        // The registry cannot observe the native state, so every explicit
        // request is forwarded instead of being compared with a cached one.
        if (!updateParameters.state) {
            stepCompletionHandler({ });
            return;
        }

        window.setState(updateParameters.state.value(), WTF::move(stepCompletionHandler));
    };

    auto updateFocus = [](WebExtensionWindow& window, const WebExtensionWindowParameters& updateParameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& stepCompletionHandler) {
        if (!updateParameters.focused || !updateParameters.focused.value()) {
            stepCompletionHandler({ });
            return;
        }

        window.focus(WTF::move(stepCompletionHandler));
    };

    updateState(*window, updateParameters, [window = Ref { *window }, updateParameters, updateFocus = WTF::move(updateFocus), completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& stateResult) mutable {
        if (!stateResult) {
            completionHandler(makeUnexpected(stateResult.error()));
            return;
        }

        updateFocus(window, updateParameters, [window, completionHandler = WTF::move(completionHandler)](std::expected<void, WebExtensionError>&& focusResult) mutable {
            if (!focusResult) {
                completionHandler(makeUnexpected(focusResult.error()));
                return;
            }

            if (!window->isOpen() || !window->extensionHasAccess()) {
                completionHandler(toWebExtensionError(apiName, nullString(), "window not found"_s));
                return;
            }

            completionHandler(window->parameters());
        });
    });
}

void WebExtensionContext::windowsRemove(WebExtensionWindowIdentifier windowIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr window = getWindow(windowIdentifier);
    if (!window) {
        completionHandler(toWebExtensionError("windows.remove()"_s, nullString(), "window not found"_s));
        return;
    }

    window->close(WTF::move(completionHandler));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
