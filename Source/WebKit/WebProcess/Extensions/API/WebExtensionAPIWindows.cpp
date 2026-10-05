/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "WebExtensionAPIWindows.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionWindowParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionTabCommandParser.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionWindowQueryParser.h"
#include "WebProcess.h"
namespace WebKit {
static void callWithWindow(WebExtensionCallbackHandler& callback, std::expected<WebExtensionWindowParameters, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    auto context = callback.globalContext();
    Protected<JSValueRef> value(context, toWebAPI(context, *result));
    callback.call(value.get());
}
void WebExtensionAPIWindows::get(WebPageProxyIdentifier page, double identifier, RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto window = toWebExtensionWindowIdentifier(identifier);
    if (!isValid(window) || identifier > 9007199254740991.0) {
        exception = "windowID must be a valid window identifier"_s;
        return;
    }
    auto query = parseWebExtensionWindowQuery(info.get(), WebExtensionWindowQueryKind::Get);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsGet(page, *window, query->types, query->populate),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<WebExtensionWindowParameters, WebExtensionError>&& result) {
            callWithWindow(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}
void WebExtensionAPIWindows::getCurrent(WebPageProxyIdentifier page, RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    get(page, WebExtensionWindowConstants::Current, WTF::move(info), WTF::move(callback), exception);
}
void WebExtensionAPIWindows::getLastFocused(RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto query = parseWebExtensionWindowQuery(info.get(), WebExtensionWindowQueryKind::Get);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsGetLastFocused(query->types, query->populate),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<WebExtensionWindowParameters, WebExtensionError>&& result) {
            callWithWindow(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}
void WebExtensionAPIWindows::getAll(RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto query = parseWebExtensionWindowQuery(info.get(), WebExtensionWindowQueryKind::Get);
    if (!query) {
        exception = query.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsGetAll(query->types, query->populate),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionWindowParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Vector<Protected<JSValueRef>> protectedWindows;
            Vector<JSValueRef> windows;
            for (auto& window : *result) {
                protectedWindows.append(Protected<JSValueRef>(context, toWebAPI(context, window)));
                windows.append(protectedWindows.last().get());
            }
            Protected<JSObjectRef> array(context, JSObjectMakeArray(context, windows.size(), windows.span().data(), nullptr));
            callback->call(array.get());
        }, extensionContext().identifier());
}
bool WebExtensionAPIWindows::isPropertyAllowed(const ASCIILiteral& name, WebPage*)
{
    // create(), update() and remove() ask the native browser; whether it
    // implements them is only known when it answers.
    Ref context = extensionContext();
    return isForMainWorld() && !context->isUnsupportedAPI(propertyPath(), name);
}

static bool parseWindowIdentifier(double windowID, std::optional<WebExtensionWindowIdentifier>& identifier, String& exception)
{
    identifier = toWebExtensionWindowIdentifier(windowID);
    if (identifier && isValid(*identifier) && windowID <= 9007199254740991.0)
        return true;
    exception = toErrorString(nullString(), "windowId"_s, identifier && isNone(*identifier) ? "'windows.WINDOW_ID_NONE' is not allowed"_s : "it is not a window identifier"_s);
    return false;
}

void WebExtensionAPIWindows::createWindow(RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/windows/create

    auto parameters = parseWebExtensionWindowCreateData(info.get(), extensionContext().baseURL());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsCreate(WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionWindowParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            // A window this extension may not see is reported as undefined.
            if (!*result) {
                callback->call();
                return;
            }
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, toWebAPI(context, **result));
            callback->call(value.get());
        }, extensionContext().identifier());
}

void WebExtensionAPIWindows::update(double windowID, RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/windows/update

    std::optional<WebExtensionWindowIdentifier> window;
    if (!parseWindowIdentifier(windowID, window, exception))
        return;
    auto parameters = parseWebExtensionWindowUpdateInfo(info.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsUpdate(*window, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<WebExtensionWindowParameters, WebExtensionError>&& result) {
            callWithWindow(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPIWindows::remove(double windowID, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/windows/remove

    std::optional<WebExtensionWindowIdentifier> window;
    if (!parseWindowIdentifier(windowID, window, exception))
        return;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::WindowsRemove(*window),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}

WebExtensionAPIWindowsEvent& WebExtensionAPIWindows::onCreated()
{
    if (!m_onCreated)
        m_onCreated = WebExtensionAPIWindowsEvent::create(*this, WebExtensionEventListenerType::WindowsOnCreated);
    return *m_onCreated;
}
WebExtensionAPIWindowsEvent& WebExtensionAPIWindows::onRemoved()
{
    if (!m_onRemoved)
        m_onRemoved = WebExtensionAPIWindowsEvent::create(*this, WebExtensionEventListenerType::WindowsOnRemoved);
    return *m_onRemoved;
}
WebExtensionAPIWindowsEvent& WebExtensionAPIWindows::onFocusChanged()
{
    if (!m_onFocusChanged)
        m_onFocusChanged = WebExtensionAPIWindowsEvent::create(*this, WebExtensionEventListenerType::WindowsOnFocusChanged);
    return *m_onFocusChanged;
}
void WebExtensionContextProxy::dispatchWindowsEvent(WebExtensionEventListenerType type, const std::optional<WebExtensionWindowParameters>& window)
{
    if (window && (!window->identifier || !window->type))
        return;
    auto filter = window ? OptionSet { *window->type == WebExtensionWindow::Type::Normal ? WebExtensionWindowTypeFilter::Normal : WebExtensionWindowTypeFilter::Popup }
                         : allWebExtensionWindowTypeFilters();
    enumerateNamespaceObjects([&](auto& object) {
        auto& windows = object.windows();
        switch (type) {
        case WebExtensionEventListenerType::WindowsOnCreated:
            if (window) {
                windows.onCreated().invokeListenersWithArgumentFactory(filter, [&](JSGlobalContextRef context) {
                    return Protected<JSValueRef>(context, toWebAPI(context, *window));
                });
            }
            break;
        case WebExtensionEventListenerType::WindowsOnRemoved:
            if (window) {
                windows.onRemoved().invokeListenersWithArgumentFactory(filter, [&](JSGlobalContextRef context) {
                    return Protected<JSValueRef>(context, JSValueMakeNumber(context, toWebAPI(*window->identifier)));
                });
            }
            break;
        case WebExtensionEventListenerType::WindowsOnFocusChanged:
            windows.onFocusChanged().invokeListenersWithArgumentFactory(filter, [&](JSGlobalContextRef context) {
                return Protected<JSValueRef>(context, JSValueMakeNumber(context, window ? toWebAPI(*window->identifier) : WebExtensionWindowConstants::None));
            });
            break;
        default:
            ASSERT_NOT_REACHED();
        }
    });
}
}
#endif
