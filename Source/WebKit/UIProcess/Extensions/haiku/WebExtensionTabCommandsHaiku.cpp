/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionTab.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContext.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionWindow.h"
#include "WebKitContext.h"
#include "WebPageProxy.h"
#include <Message.h>
#include <WebCore/MediaProducer.h>

// The native browser owns its tabs and windows. Every change below is a request
// to that host through the context's browser-command bridge; the read-only
// state in WebExtensionTabHaiku.cpp and WebExtensionWindowHaiku.cpp follows
// once the host resubmits its window through BWebKitContext::SetBrowserWindowTabs.

namespace WebKit {

using VoidCompletion = CompletionHandler<void(std::expected<void, WebExtensionError>&&)>;

static void failAsynchronously(VoidCompletion&& completion, const String& apiName, ASCIILiteral reason)
{
    RunLoop::mainSingleton().dispatch([completion = WTF::move(completion), error = toErrorString(apiName, nullString(), reason).isolatedCopy()]() mutable {
        completion(makeUnexpected(WTF::move(error)));
    });
}

static void sendTabCommand(const WebExtensionTab& tab, uint32_t command, const String& apiName, Function<bool(BMessage&)>&& addFields, VoidCompletion&& completion)
{
    RefPtr context = tab.extensionContext();
    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    if (!context || !tab.isOpen() || !tab.extensionHasAccess()) {
        failAsynchronously(WTF::move(completion), apiName, "tab not found"_s);
        return;
    }
    bool complete = message.AddUInt32("command", command) == B_OK && context->addNativeBrowserCommandTab(message, tab);
    if (RefPtr window = complete ? tab.window() : nullptr)
        complete = context->addNativeBrowserCommandWindow(message, *window);
    if (complete && addFields)
        complete = addFields(message);
    if (!complete) {
        failAsynchronously(WTF::move(completion), apiName, "the tab is not available in the browser"_s);
        return;
    }
    context->sendNativeBrowserCommand(WTF::move(message), apiName, [completion = WTF::move(completion)](WebExtensionContext::NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completion(makeUnexpected(result.error()));
            return;
        }
        completion({ });
    });
}

RefPtr<WebExtensionTab> WebExtensionTab::parentTab() const
{
    // The native browser does not record an opener relationship yet.
    return nullptr;
}

void WebExtensionTab::setParentTab(RefPtr<WebExtensionTab>, VoidCompletion&& completion)
{
    failAsynchronously(WTF::move(completion), "tabs.update()"_s, "it is not implemented for 'openerTabId'"_s);
}

bool WebExtensionTab::isPinned() const
{
    return false;
}

void WebExtensionTab::setPinned(bool, VoidCompletion&& completion)
{
    // Summit's tab strip has no pinned tabs. Treat pinning as a presentation
    // hint, as browsers without pinning do, instead of failing the whole
    // update: OneTab restores a saved pinned tab with
    // `await tabs.update(id, { pinned: true })` inside its restore loop, and
    // a rejection there abandoned the remaining tabs. The tab reports
    // `pinned: false`.
    completion({ });
}

bool WebExtensionTab::isPlayingAudio() const
{
    RefPtr page = nativePage();
    return page && isValid() && page->isPlayingAudio();
}

bool WebExtensionTab::isMuted() const
{
    RefPtr page = nativePage();
    return page && isValid() && page->isAudioMuted();
}

void WebExtensionTab::setMuted(bool muted, VoidCompletion&& completion)
{
    RefPtr page = nativePage();
    if (!page || !isOpen() || !extensionHasAccess()) {
        failAsynchronously(WTF::move(completion), "tabs.update()"_s, "tab not found"_s);
        return;
    }
    // Audio muting is page state without a native browser control, so the
    // engine applies it directly. Capture devices keep their current state.
    auto state = page->mutedStateFlags();
    if (muted)
        state.add(WebCore::MediaProducerMutedState::AudioIsMuted);
    else
        state.remove(WebCore::MediaProducerMutedState::AudioIsMuted);
    page->setMuted(state);
    if (RefPtr context = m_extensionContext.get())
        context->didChangeTabProperties(*this, { ChangedProperties::Muted });
    completion({ });
}

void WebExtensionTab::activate(VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_ACTIVATE_TAB, "tabs.update()"_s, nullptr, WTF::move(completion));
}

void WebExtensionTab::moveToIndex(int32_t index, VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_MOVE_TAB, "tabs.move()"_s, [index](BMessage& message) {
        return message.AddInt32("index", index) == B_OK;
    }, WTF::move(completion));
}

void WebExtensionTab::setSelected(bool selected, VoidCompletion&& completion)
{
    // The native tab strip has a single selection, which is its active tab.
    if (selected == isSelected()) {
        completion({ });
        return;
    }
    if (!selected) {
        failAsynchronously(WTF::move(completion), "tabs.update()"_s, "it is not implemented for 'highlighted' or 'selected' set to `false`"_s);
        return;
    }
    activate(WTF::move(completion));
}

void WebExtensionTab::loadURL(URL url, VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_NAVIGATE_TAB, "tabs.update()"_s, [url = WTF::move(url)](BMessage& message) {
        return message.AddString("url", url.string().utf8().legacyCStringPointer()) == B_OK;
    }, WTF::move(completion));
}

void WebExtensionTab::reload(ReloadFromOrigin fromOrigin, VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_RELOAD_TAB, "tabs.reload()"_s, [fromOrigin](BMessage& message) {
        return message.AddBool("bypass_cache", fromOrigin == ReloadFromOrigin::Yes) == B_OK;
    }, WTF::move(completion));
}

void WebExtensionTab::goBack(VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_GO_BACK, "tabs.goBack()"_s, nullptr, WTF::move(completion));
}

void WebExtensionTab::goForward(VoidCompletion&& completion)
{
    sendTabCommand(*this, B_WEBKIT_BROWSER_GO_FORWARD, "tabs.goForward()"_s, nullptr, WTF::move(completion));
}

void WebExtensionTab::close(VoidCompletion&& completion)
{
    // The command names one tab here; tabs.remove() batches its own request.
    RefPtr context = extensionContext();
    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    static constexpr auto apiName = "tabs.remove()"_s;
    if (!context || !isOpen() || !extensionHasAccess()) {
        failAsynchronously(WTF::move(completion), apiName, "tab not found"_s);
        return;
    }
    RefPtr window = this->window();
    if (message.AddUInt32("command", B_WEBKIT_BROWSER_CLOSE_TABS) != B_OK || !context->addNativeBrowserCommandTab(message, *this)
        || !window || !context->addNativeBrowserCommandWindow(message, *window)) {
        failAsynchronously(WTF::move(completion), apiName, "the tab is not available in the browser"_s);
        return;
    }
    context->sendNativeBrowserCommand(WTF::move(message), apiName, [completion = WTF::move(completion)](WebExtensionContext::NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completion(makeUnexpected(result.error()));
            return;
        }
        completion({ });
    });
}

static void sendWindowCommand(const WebExtensionWindow& window, uint32_t command, const String& apiName, Function<bool(BMessage&)>&& addFields, VoidCompletion&& completion)
{
    RefPtr context = window.extensionContext();
    BMessage message(B_WEBKIT_BROWSER_COMMAND);
    if (!context || !window.isOpen() || !window.extensionHasAccess()) {
        failAsynchronously(WTF::move(completion), apiName, "window not found"_s);
        return;
    }
    bool complete = message.AddUInt32("command", command) == B_OK && context->addNativeBrowserCommandWindow(message, window);
    if (complete && addFields)
        complete = addFields(message);
    if (!complete) {
        failAsynchronously(WTF::move(completion), apiName, "the window is not available in the browser"_s);
        return;
    }
    context->sendNativeBrowserCommand(WTF::move(message), apiName, [completion = WTF::move(completion)](WebExtensionContext::NativeBrowserCommandResult&& result) mutable {
        if (!result) {
            completion(makeUnexpected(result.error()));
            return;
        }
        completion({ });
    });
}

WebExtensionWindow::Type WebExtensionWindow::type() const
{
    // The registered native browser currently creates normal windows.
    return Type::Normal;
}

WebExtensionWindow::State WebExtensionWindow::state() const
{
    // The registry does not track minimized, zoomed or fullscreen windows.
    return State::Normal;
}

void WebExtensionWindow::setState(State state, VoidCompletion&& completion)
{
    sendWindowCommand(*this, B_WEBKIT_BROWSER_SET_WINDOW_STATE, "windows.update()"_s, [state](BMessage& message) {
        const char* name = "normal";
        switch (state) {
        case State::Normal:
            break;
        case State::Minimized:
            name = "minimized";
            break;
        case State::Maximized:
            name = "maximized";
            break;
        case State::Fullscreen:
            name = "fullscreen";
            break;
        }
        return message.AddString("state", name) == B_OK;
    }, WTF::move(completion));
}

void WebExtensionWindow::focus(VoidCompletion&& completion)
{
    sendWindowCommand(*this, B_WEBKIT_BROWSER_FOCUS_WINDOW, "windows.update()"_s, nullptr, WTF::move(completion));
}

void WebExtensionWindow::close(VoidCompletion&& completion)
{
    sendWindowCommand(*this, B_WEBKIT_BROWSER_CLOSE_WINDOW, "windows.remove()"_s, nullptr, WTF::move(completion));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
