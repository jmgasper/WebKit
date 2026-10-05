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
#include "WebExtensionAPITabs.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionAPIPort.h"
#include "WebExtensionAPIRuntime.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionMessageTargetParameters.h"
#include "WebExtensionScriptInjectionParameters.h"
#include "WebExtensionTab.h"
#include "WebExtensionTabCommandParser.h"
#include "WebExtensionTabQueryParser.h"
#include "WebExtensionTabScriptParser.h"
#include <WebCore/SerializedScriptValue.h>
#include "WebExtensionUtilities.h"
#include "JSWebExtensionTabParameters.h"
#include "WebFrame.h"
#include "WebProcess.h"
#include "WebPage.h"
#include <WebCore/UserGestureIndicator.h>

namespace WebKit {
static void callWithTab(WebExtensionCallbackHandler& callback, std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    auto context = callback.globalContext();
    Protected<JSValueRef> value(context, *result
        ? toWebAPI(context, **result)
        : JSValueMakeUndefined(context));
    callback.call(value.get());
}

void WebExtensionAPITabs::get(double identifier, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto tab = toWebExtensionTabIdentifier(identifier);
    if (!isValid(tab)) {
        exception = "tabID must be a valid tab identifier"_s;
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsGet(*tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&& result) {
            callWithTab(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::getCurrent(WebPageProxyIdentifier page, Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsGetCurrent(page),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&& result) {
            callWithTab(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::query(WebPageProxyIdentifier page, RefPtr<JSON::Value> info, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto parameters = parseWebExtensionTabQuery(info.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsQuery(page, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Vector<Protected<JSValueRef>> protectedTabs;
            Vector<JSValueRef> tabs;
            for (auto& tab : *result) {
                protectedTabs.append(Protected<JSValueRef>(context, toWebAPI(context, tab)));
                tabs.append(protectedTabs.last().get());
            }
            Protected<JSObjectRef> array(context, JSObjectMakeArray(context, tabs.size(), tabs.span().data(), nullptr));
            callback->call(array.get());
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::sendMessage(WebFrame& frame, double identifier, const String& messageJSON, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto tab = toWebExtensionTabIdentifier(identifier);
    if (!isValid(tab)) {
        exception = "tabID must be a valid tab identifier"_s;
        return;
    }
    if (messageJSON.length() > webExtensionMaxMessageLength) {
        exception = "message exceeded the maximum allowed length"_s;
        return;
    }
    auto document = toDocumentIdentifier(frame);
    RefPtr page = frame.page();
    if (!document || !page) {
        exception = "the sending document is unavailable"_s;
        return;
    }
    WebExtensionMessageTargetParameters target;
    if (options && !options->isNull()) {
        auto object = options->asObject();
        if (!object) {
            exception = "options must be an object"_s;
            return;
        }
        for (auto& key : object->keys()) {
            if (key != "frameId"_s && key != "documentId"_s) {
                exception = "options contains an unsupported field"_s;
                return;
            }
        }
        if (auto value = object->getValue("frameId"_s); value && !value->isNull()) {
            auto number = value->asDouble();
            auto frameIdentifier = number ? toWebExtensionFrameIdentifier(*number) : std::nullopt;
            if (!isValid(frameIdentifier)) {
                exception = "frameId must be a nonnegative integer frame identifier"_s;
                return;
            }
            target.frameIdentifier = *frameIdentifier;
        }
        if (auto value = object->getValue("documentId"_s); value && !value->isNull()) {
            auto documentIdentifier = WTF::UUID::parse(value->asString());
            if (!documentIdentifier) {
                exception = "documentId must be a document identifier"_s;
                return;
            }
            target.documentIdentifier = *documentIdentifier;
        }
    }
    WebExtensionMessageSenderParameters sender {
        extensionContext().uniqueIdentifier(), std::nullopt,
        toWebExtensionFrameIdentifier(frame), page->webPageProxyIdentifier(),
        contentWorldType(), frame.url(), *document,
    };
    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsSendMessage(*tab, messageJSON, target, sender, userGesture),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            callback->call(fromJSON(callback->globalContext(), JSON::Value::parseJSON(*result)));
        }, extensionContext().identifier());
}

bool WebExtensionAPITabs::isPropertyAllowed(const ASCIILiteral& name, WebPage*)
{
    // The dynamic tabs functions are the injection calls Manifest V3 replaced
    // with the scripting namespace.
    Ref context = extensionContext();
    return (name == "executeScript"_s || name == "insertCSS"_s || name == "removeCSS"_s) && isForMainWorld()
        && !context->supportsManifestVersion(3) && !context->isUnsupportedAPI(propertyPath(), name);
}

// An omitted, undefined or null tab identifier selects the calling context's
// current tab in the UI process. Anything else must name a real tab.
static bool parseOptionalTabIdentifier(double tabID, std::optional<WebExtensionTabIdentifier>& identifier, String& exception)
{
    identifier = std::nullopt;
    if (std::isnan(tabID) || !tabID)
        return true;
    identifier = toWebExtensionTabIdentifier(tabID);
    if (isValid(identifier) && tabID <= 9007199254740991.0)
        return true;
    exception = toErrorString(nullString(), "tabId"_s, isNone(identifier) ? "'tabs.TAB_ID_NONE' is not allowed"_s : "it is not a tab identifier"_s);
    return false;
}

static void callWithVoid(WebExtensionCallbackHandler& callback, std::expected<void, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    callback.call();
}

void WebExtensionAPITabs::createTab(WebPageProxyIdentifier page, RefPtr<JSON::Value> properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/create

    auto parameters = parseWebExtensionTabCreateProperties(properties.get(), extensionContext().baseURL());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsCreate(page, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&& result) {
            callWithTab(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::update(WebPageProxyIdentifier page, double tabID, RefPtr<JSON::Value> properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/update

    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    auto parameters = parseWebExtensionTabUpdateProperties(properties.get(), extensionContext().baseURL());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsUpdate(page, tab, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&& result) {
            callWithTab(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::captureVisibleTab(WebPageProxyIdentifier page, double windowID, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/captureVisibleTab

    auto window = toWebExtensionWindowIdentifier(windowID);
    if (window && !isValid(window)) {
        exception = "'windowId' value is invalid, because it is not a valid window identifier."_s;
        return;
    }
    auto format = WebExtensionTabImageFormat::PNG;
    uint8_t quality = 92;
    if (options && options->type() != JSON::Value::Type::Null) {
        RefPtr object = options->asObject();
        if (!object) {
            exception = "'options' value is invalid, because an object is expected."_s;
            return;
        }
        if (auto value = object->getValue("format"_s)) {
            auto name = value->asString();
            if (name == "jpeg"_s)
                format = WebExtensionTabImageFormat::JPEG;
            else if (name != "png"_s) {
                exception = "'format' value is invalid, because it must specify either 'png' or 'jpeg'."_s;
                return;
            }
        }
        if (auto value = object->getValue("quality"_s)) {
            auto number = value->asDouble();
            if (!number || *number < 0 || *number > 100) {
                exception = "'quality' value is invalid, because it must specify a number between 0 and 100."_s;
                return;
            }
            quality = static_cast<uint8_t>(*number);
        }
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsCaptureVisibleTab(page, window, format, quality),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<URL, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Protected<JSValueRef> value(context, JSValueMakeString(context, toJSString(result->string()).get()));
            callback->call(value.get());
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::move(JSContextRef context, JSValueRef tabIDs, RefPtr<JSON::Value> properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/move

    // One tab identifier (a number) or an array of them, as tabs.remove().
    RefPtr values = fromJSValue(context, tabIDs);
    auto identifiers = parseWebExtensionTabIdentifiers(values.get());
    if (!identifiers) {
        exception = identifiers.error();
        return;
    }
    RefPtr object = properties ? properties->asObject() : nullptr;
    if (!object) {
        exception = "'moveProperties' value is invalid, because an object is expected."_s;
        return;
    }
    auto index = object->getDouble("index"_s);
    if (!index) {
        exception = "'moveProperties' value is invalid, because it is missing required keys: 'index'."_s;
        return;
    }
    std::optional<WebExtensionWindowIdentifier> window;
    if (auto windowID = object->getDouble("windowId"_s)) {
        window = toWebExtensionWindowIdentifier(*windowID);
        if (!isValid(window)) {
            exception = "'windowId' value is invalid, because it is not a valid window identifier."_s;
            return;
        }
    }
    bool single = values && values->type() != JSON::Value::Type::Array;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsMove(WTF::move(*identifiers), window, *index),
        [protectedThis = Ref { *this }, callback = WTF::move(callback), single](std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            // A single identifier answers with its tab, an array with an array.
            if (single && result->size() == 1) {
                callWithTab(callback.get(), std::optional<WebExtensionTabParameters> { WTF::move(result->first()) });
                return;
            }
            auto context = callback->globalContext();
            Vector<JSValueRef> values;
            for (auto& parameters : *result)
                values.append(toWebAPI(context, parameters));
            Protected<JSValueRef> array(context, JSObjectMakeArray(context, values.size(), values.span().data(), nullptr));
            callback->call(array.get());
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::remove(JSContextRef context, JSValueRef tabIDs, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/remove

    RefPtr values = fromJSValue(context, tabIDs);
    auto identifiers = parseWebExtensionTabIdentifiers(values.get());
    if (!identifiers) {
        exception = identifiers.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsRemove(WTF::move(*identifiers)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::reload(WebPageProxyIdentifier page, double tabID, RefPtr<JSON::Value> properties, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/reload

    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    auto bypassCache = parseWebExtensionTabReloadProperties(properties.get());
    if (!bypassCache) {
        exception = bypassCache.error();
        return;
    }
    auto reloadFromOrigin = *bypassCache ? ReloadFromOrigin::Yes : ReloadFromOrigin::No;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsReload(page, tab, reloadFromOrigin),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::goBack(WebPageProxyIdentifier page, double tabID, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/goBack

    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsGoBack(page, tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::goForward(WebPageProxyIdentifier page, double tabID, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/goForward

    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsGoForward(page, tab),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::insertCSS(WebPageProxyIdentifier page, double tabID, RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/insertCSS

    if (!isForMainWorld() || extensionContext().supportsManifestVersion(3)) {
        exception = "tabs.insertCSS is available only to Manifest V2 extension pages"_s;
        return;
    }
    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    auto parameters = parseWebExtensionTabCSSDetails(details.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsInsertCSS(page, tab, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

void WebExtensionAPITabs::removeCSS(WebPageProxyIdentifier page, double tabID, RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/removeCSS

    if (!isForMainWorld() || extensionContext().supportsManifestVersion(3)) {
        exception = "tabs.removeCSS is available only to Manifest V2 extension pages"_s;
        return;
    }
    std::optional<WebExtensionTabIdentifier> tab;
    if (!parseOptionalTabIdentifier(tabID, tab, exception))
        return;
    auto parameters = parseWebExtensionTabCSSDetails(details.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsRemoveCSS(page, tab, WTF::move(*parameters)),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
            callWithVoid(callback.get(), WTF::move(result));
        }, extensionContext().identifier());
}

RefPtr<WebExtensionAPIPort> WebExtensionAPITabs::connect(WebFrame& frame, JSContextRef context, double tabID, RefPtr<JSON::Value> options, String& exception)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/tabs/connect

    auto tab = toWebExtensionTabIdentifier(tabID);
    if (!isValid(tab) || tabID > 9007199254740991.0) {
        exception = toErrorString(nullString(), "tabId"_s, isNone(tab) ? "'tabs.TAB_ID_NONE' is not allowed"_s : "it is not a tab identifier"_s);
        return nullptr;
    }
    auto document = toDocumentIdentifier(frame);
    RefPtr page = frame.page();
    if (!document || !page) {
        exception = toErrorString(nullString(), nullString(), "an unexpected error occured"_s);
        return nullptr;
    }

    String name;
    WebExtensionMessageTargetParameters target;
    if (options && !options->isNull()) {
        RefPtr object = options->asObject();
        if (!object) {
            exception = toErrorString(nullString(), "options"_s, "an object is expected"_s);
            return nullptr;
        }
        if (RefPtr value = object->getValue("name"_s); value && !value->isNull()) {
            if (value->type() != JSON::Value::Type::String) {
                exception = toErrorString(nullString(), "name"_s, "a string is expected"_s);
                return nullptr;
            }
            name = value->asString();
        }
        if (RefPtr value = object->getValue("frameId"_s); value && !value->isNull()) {
            auto number = value->asDouble();
            auto frameIdentifier = number ? toWebExtensionFrameIdentifier(*number) : std::nullopt;
            if (!isValid(frameIdentifier)) {
                exception = toErrorString(nullString(), "frameId"_s, "it is not a frame identifier"_s);
                return nullptr;
            }
            target.frameIdentifier = *frameIdentifier;
        }
        if (RefPtr value = object->getValue("documentId"_s); value && !value->isNull()) {
            auto documentIdentifier = WTF::UUID::parse(value->asString());
            if (!documentIdentifier) {
                exception = toErrorString(nullString(), "documentId"_s, "it is not a valid document identifier"_s);
                return nullptr;
            }
            target.documentIdentifier = *documentIdentifier;
        }
    }

    WebExtensionMessageSenderParameters sender {
        extensionContext().uniqueIdentifier(), std::nullopt,
        toWebExtensionFrameIdentifier(frame), page->webPageProxyIdentifier(),
        contentWorldType(), frame.url(), *document,
    };

    Ref port = WebExtensionAPIPort::create(*this, page->webPageProxyIdentifier(), WebExtensionContentWorldType::ContentScript, name);

    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsConnect(*tab, port->channelIdentifier(), name, target, sender, userGesture),
        [this, protectedThis = Ref { *this }, port, globalContext = JSRetainPtr { JSContextGetGlobalContext(context) }](std::expected<void, WebExtensionError>&& result) {
            if (result)
                return;

            port->setError(globalContext.get(), protect(runtime())->reportError(result.error(), globalContext.get()));
            port->disconnect();
        }, extensionContext().identifier());

    return port;
}

void WebExtensionAPITabs::executeScript(WebPageProxyIdentifier page, double tabID, RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    if (!isForMainWorld() || extensionContext().supportsManifestVersion(3)) {
        exception = "tabs.executeScript is available only to Manifest V2 extension pages"_s;
        return;
    }
    std::optional<WebExtensionTabIdentifier> tab;
    if (!std::isnan(tabID)) {
        tab = toWebExtensionTabIdentifier(tabID);
        if (!isValid(tab) || tabID > 9007199254740991.0) {
            exception = "tabID must be a valid tab identifier"_s;
            return;
        }
    }
    auto parameters = parseWebExtensionTabScript(details.get());
    if (!parameters) {
        exception = parameters.error();
        return;
    }
    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::TabsExecuteScriptNative(page, tab, *parameters, userGesture),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<RefPtr<WebCore::SerializedScriptValue>>, WebExtensionError>&& result) {
            if (!result) {
                callback->reportError(result.error());
                return;
            }
            auto context = callback->globalContext();
            Vector<Protected<JSValueRef>> values;
            Vector<JSValueRef> arguments;
            for (auto& serialized : *result) {
                JSValueRef exception = nullptr;
                auto value = serialized ? serialized->deserialize(context, &exception) : nullptr;
                if (!value || exception) {
                    callback->reportError("the script result could not be cloned into this extension"_s);
                    return;
                }
                values.append(Protected<JSValueRef>(context, value));
                arguments.append(values.last().get());
            }
            JSValueRef exception = nullptr;
            Protected<JSObjectRef> array(context, JSObjectMakeArray(context, arguments.size(), arguments.span().data(), &exception));
            if (!array.get() || exception) {
                callback->reportError("the script result array could not be created"_s);
                return;
            }
            callback->call(array.get());
        }, extensionContext().identifier());
}

WebExtensionAPIEvent& WebExtensionAPITabs::onActivated()
{
    if (!m_onActivated)
        m_onActivated = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnActivated);
    return *m_onActivated;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onAttached()
{
    if (!m_onAttached)
        m_onAttached = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnAttached);
    return *m_onAttached;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onCreated()
{
    if (!m_onCreated)
        m_onCreated = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnCreated);
    return *m_onCreated;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onDetached()
{
    if (!m_onDetached)
        m_onDetached = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnDetached);
    return *m_onDetached;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onHighlighted()
{
    if (!m_onHighlighted)
        m_onHighlighted = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnHighlighted);
    return *m_onHighlighted;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onMoved()
{
    if (!m_onMoved)
        m_onMoved = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnMoved);
    return *m_onMoved;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onRemoved()
{
    if (!m_onRemoved)
        m_onRemoved = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnRemoved);
    return *m_onRemoved;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onReplaced()
{
    if (!m_onReplaced)
        m_onReplaced = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnReplaced);
    return *m_onReplaced;
}

WebExtensionAPIEvent& WebExtensionAPITabs::onUpdated()
{
    if (!m_onUpdated)
        m_onUpdated = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TabsOnUpdated);
    return *m_onUpdated;
}

void WebExtensionContextProxy::dispatchTabsCreatedEvent(const WebExtensionTabParameters& tab)
{
    enumerateNamespaceObjects([&](auto& object) {
        object.tabs().onCreated().template invokeListenersWithArgumentFactory<1>([&](JSGlobalContextRef context) {
            return std::array { Protected<JSValueRef>(context, toWebAPI(context, tab)) };
        });
    });
}

void WebExtensionContextProxy::dispatchTabsUpdatedEvent(const WebExtensionTabParameters& tab, const WebExtensionTabParameters& changed)
{
    if (!tab.identifier)
        return;
    enumerateNamespaceObjects([&](auto& object) {
        object.tabs().onUpdated().template invokeListenersWithArgumentFactory<3>([&](JSGlobalContextRef context) {
            return std::array {
                Protected<JSValueRef>(context, JSValueMakeNumber(context, toWebAPI(*tab.identifier))),
                Protected<JSValueRef>(context, toWebAPI(context, changed)),
                Protected<JSValueRef>(context, toWebAPI(context, tab))
            };
        });
    });
}

void WebExtensionContextProxy::dispatchTabsReplacedEvent(WebExtensionTabIdentifier replaced, WebExtensionTabIdentifier replacement)
{
    auto oldID = String::number(toWebAPI(replaced));
    auto newID = String::number(toWebAPI(replacement));
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onReplaced().invokeListenersWithJSONArguments({ newID, oldID }); });
}

void WebExtensionContextProxy::dispatchTabsDetachedEvent(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, uint64_t position)
{
    for (auto entry : m_tabPageMap) {
        if (entry.value.first == tab) {
            entry.value.second = std::nullopt;
            break;
        }
    }
    auto info = JSON::Object::create();
    info->setDouble("oldWindowId"_s, toWebAPI(window));
    info->setDouble("oldPosition"_s, position);
    auto identifier = String::number(toWebAPI(tab));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onDetached().invokeListenersWithJSONArguments({ identifier, json }); });
}

void WebExtensionContextProxy::dispatchTabsAttachedEvent(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, uint64_t position)
{
    for (auto entry : m_tabPageMap) {
        if (entry.value.first == tab) {
            entry.value.second = window;
            break;
        }
    }
    auto info = JSON::Object::create();
    info->setDouble("newWindowId"_s, toWebAPI(window));
    info->setDouble("newPosition"_s, position);
    auto identifier = String::number(toWebAPI(tab));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onAttached().invokeListenersWithJSONArguments({ identifier, json }); });
}

void WebExtensionContextProxy::dispatchTabsMovedEvent(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, uint64_t from, uint64_t to)
{
    auto info = JSON::Object::create();
    info->setDouble("windowId"_s, toWebAPI(window));
    info->setDouble("fromIndex"_s, from);
    info->setDouble("toIndex"_s, to);
    auto identifier = String::number(toWebAPI(tab));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onMoved().invokeListenersWithJSONArguments({ identifier, json }); });
}

void WebExtensionContextProxy::dispatchTabsActivatedEvent(WebExtensionTabIdentifier previous, WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window)
{
    auto info = JSON::Object::create();
    if (!isNone(previous))
        info->setDouble("previousTabId"_s, toWebAPI(previous));
    info->setDouble("tabId"_s, toWebAPI(tab));
    info->setDouble("windowId"_s, toWebAPI(window));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onActivated().invokeListenersWithJSONArguments({ json }); });
}

void WebExtensionContextProxy::dispatchTabsHighlightedEvent(const Vector<WebExtensionTabIdentifier>& tabs, WebExtensionWindowIdentifier window)
{
    auto info = JSON::Object::create();
    auto identifiers = JSON::Array::create();
    for (auto tab : tabs)
        identifiers->pushDouble(toWebAPI(tab));
    info->setDouble("windowId"_s, toWebAPI(window));
    info->setArray("tabIds"_s, WTF::move(identifiers));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onHighlighted().invokeListenersWithJSONArguments({ json }); });
}

void WebExtensionContextProxy::dispatchTabsRemovedEvent(WebExtensionTabIdentifier tab, WebExtensionWindowIdentifier window, WebExtensionContext::WindowIsClosing closing)
{
    auto info = JSON::Object::create();
    info->setDouble("windowId"_s, toWebAPI(window));
    info->setBoolean("isWindowClosing"_s, closing == WebExtensionContext::WindowIsClosing::Yes);
    auto identifier = String::number(toWebAPI(tab));
    auto json = info->toJSONString();
    enumerateNamespaceObjects([&](auto& object) { object.tabs().onRemoved().invokeListenersWithJSONArguments({ identifier, json }); });
}
}
#endif
