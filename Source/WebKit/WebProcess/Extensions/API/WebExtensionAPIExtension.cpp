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
#include "WebExtensionAPIExtension.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionUtilities.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <wtf/JSONValues.h>
#include <wtf/Vector.h>

namespace WebKit {

bool WebExtensionAPIExtension::isPropertyAllowed(const ASCIILiteral& name, WebPage*)
{
    Ref context = extensionContext();
    if (context->isUnsupportedAPI(propertyPath(), name))
        return false;
    return name == "getURL"_s && !context->supportsManifestVersion(3);
}

URL WebExtensionAPIExtension::getURL(const String& resourcePath, String&)
{
    return URL { extensionContext().baseURL(), resourcePath };
}

JSValueRef WebExtensionAPIExtension::getBackgroundPageForJavaScript(JSContextRef context)
{
    if (!isForMainWorld())
        return JSValueMakeUndefined(context);
    RefPtr page = extensionContext().backgroundPage();
    return page ? toWindowObject(context, *page) : JSValueMakeNull(context);
}

JSValueRef WebExtensionAPIExtension::getViews(JSContextRef context, RefPtr<JSON::Value> filter, String& exception)
{
    if (!isForMainWorld())
        return JSValueMakeUndefined(context);
    std::optional<ViewType> type;
    std::optional<WebExtensionTabIdentifier> tabIdentifier;
    std::optional<WebExtensionWindowIdentifier> windowIdentifier;
    if (filter && !filter->isNull()) {
        RefPtr object = filter->asObject();
        if (!object) {
            exception = "filter must be an object"_s;
            return JSValueMakeUndefined(context);
        }
        for (auto& key : object->keys()) {
            if (key != "type"_s && key != "tabId"_s && key != "windowId"_s) {
                exception = "filter contains an unsupported field"_s;
                return JSValueMakeUndefined(context);
            }
        }
        if (auto value = object->getValue("type"_s); value && !value->isNull()) {
            if (value->asString() == "popup"_s)
                type = ViewType::Popup;
            else if (value->asString() == "tab"_s)
                type = ViewType::Tab;
            else {
                exception = "filter type must be popup or tab"_s;
                return JSValueMakeUndefined(context);
            }
        }
        if (auto value = object->getValue("tabId"_s); value && !value->isNull()) {
            auto number = value->asDouble();
            tabIdentifier = number ? toWebExtensionTabIdentifier(*number) : std::nullopt;
            if (!isValid(tabIdentifier)) {
                exception = "filter tabId must be a valid tab identifier"_s;
                return JSValueMakeUndefined(context);
            }
        }
        if (auto value = object->getValue("windowId"_s); value && !value->isNull()) {
            auto number = value->asDouble();
            windowIdentifier = number ? toWebExtensionWindowIdentifier(*number) : std::nullopt;
            if (!isValid(windowIdentifier)) {
                exception = "filter windowId must be a valid window identifier"_s;
                return JSValueMakeUndefined(context);
            }
        }
    }
    auto globalContext = JSContextGetGlobalContext(context);
    Vector<Protected<JSValueRef>> retained;
    Vector<JSValueRef> views;
    const auto append = [&](WebPage& page) {
        auto value = toWindowObject(context, page);
        if (!JSValueIsUndefined(context, value)) {
            retained.append(Protected<JSValueRef>(globalContext, value));
            views.append(value);
        }
    };
    Ref extensionContext = this->extensionContext();
    if (!type && !tabIdentifier && !windowIdentifier) {
        if (RefPtr page = extensionContext->backgroundPage())
            append(*page);
    }
    if (!type || type == ViewType::Popup) {
        for (auto& page : extensionContext->popupPages(tabIdentifier, windowIdentifier))
            append(page);
    }
    if (!type || type == ViewType::Tab) {
        for (auto& page : extensionContext->tabPages(tabIdentifier, windowIdentifier))
            append(page);
    }
    return JSObjectMakeArray(context, views.size(), views.span().data(), nullptr);
}

bool WebExtensionAPIExtension::isInIncognitoContext(WebPage& page)
{
    if (extensionContext().isURLForThisExtension(page.mainWebFrame().url()))
        return false;
    if (page.sessionID() == extensionContext().defaultSessionID())
        return false;
    return page.usesEphemeralSession();
}

void WebExtensionAPIExtension::isAllowedFileSchemeAccess(Ref<WebExtensionCallbackHandler>&& callback)
{
    if (!isForMainWorld()) {
        callback->reportError("this API is available only in extension pages"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ExtensionIsAllowedFileSchemeAccess(),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](bool allowed) {
            callback->call(JSValueMakeBoolean(callback->globalContext(), allowed));
        }, extensionContext().identifier());
}

void WebExtensionAPIExtension::isAllowedIncognitoAccess(Ref<WebExtensionCallbackHandler>&& callback)
{
    if (!isForMainWorld()) {
        callback->reportError("this API is available only in extension pages"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::ExtensionIsAllowedIncognitoAccess(),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](bool allowed) {
            callback->call(JSValueMakeBoolean(callback->globalContext(), allowed));
        }, extensionContext().identifier());
}

} // namespace WebKit
#endif
