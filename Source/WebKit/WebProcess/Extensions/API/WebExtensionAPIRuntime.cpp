/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
 * Copyright (C) 2026 Igalia S.L. All rights reserved.
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
#include "WebExtensionAPIRuntime.h"


#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionMessageReply.h"
#include "JSWebExtensionTabParameters.h"
#include "MessageSenderInlines.h"
#include "WebExtensionAPIKeys.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionAPIPort.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionControllerProxy.h"
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionMessageTargetParameters.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <WebCore/LocalFrameInlines.h>
#include <WebCore/SecurityOrigin.h>
#include <wtf/Box.h>
#include <wtf/RunLoop.h>
#include <wtf/text/MakeString.h>

namespace WebKit {

JSValueRef toWebAPI(JSContextRef context, const WebExtensionMessageSenderParameters& parameters)
{
    Protected<JSObjectRef> result(JSContextGetGlobalContext(context), JSObjectMake(context, nullptr, nullptr));

    if (parameters.extensionUniqueIdentifier)
        JSObjectSetProperty(context, result.get(), toJSString("id"_s).get(), toJSValueRef(context, parameters.extensionUniqueIdentifier.value()), 0, nullptr);

    if (parameters.tabParameters)
        JSObjectSetProperty(context, result.get(), toJSString("tab"_s).get(), toWebAPI(context, parameters.tabParameters.value()), 0, nullptr);

    // The frame identifier is only included when tab is included.
    if (parameters.frameIdentifier && parameters.tabParameters)
        JSObjectSetProperty(context, result.get(), toJSString("frameId"_s).get(), JSValueMakeNumber(context, toWebAPI(parameters.frameIdentifier.value())), 0, nullptr);

    if (parameters.url.isValid()) {
        JSObjectSetProperty(context, result.get(), toJSString("url"_s).get(), toJSValueRef(context, parameters.url.string()), 0, nullptr);
        // Extension pages have hierarchical URLs, but their scheme may be opaque to SecurityOrigin
        // in the receiving process. Report "scheme://host" as Chrome and Firefox do; extensions
        // compare it with runtime.getURL("") to recognize their own pages (uBlock Origin does).
        Ref origin = WebCore::SecurityOrigin::create(parameters.url);
        auto originString = origin->isOpaque() && !parameters.url.host().isEmpty()
            ? makeString(parameters.url.protocol(), "://"_s, parameters.url.hostAndPort())
            : origin->toString();
        JSObjectSetProperty(context, result.get(), toJSString("origin"_s).get(), toJSValueRef(context, originString), 0, nullptr);
    }

    if (parameters.documentIdentifier)
        JSObjectSetProperty(context, result.get(), toJSString("documentId"_s).get(), toJSValueRef(context, parameters.documentIdentifier->toString()), 0, nullptr);

    return result.get();
}

static inline JSValueRef makeErrorValue(JSGlobalContextRef contextRef, const String& errorMessage)
{
    // This is a safer cpp false positive (rdar://163760990).
    SUPPRESS_UNCOUNTED_ARG auto argument = JSValueMakeString(contextRef, toJSString(errorMessage).get());
    return JSObjectMakeError(contextRef, 1, &argument, 0);
}

JSValueRef WebExtensionAPIRuntimeBase::reportError(String errorMessage, JSGlobalContextRef contextRef, NOESCAPE const Function<void()>& handler)
{
    ASSERT(!errorMessage.isEmpty());
    ASSERT(contextRef);

    RELEASE_LOG_ERROR(Extensions, "Runtime error reported: %" PUBLIC_LOG_STRING, errorMessage.utf8().legacyCStringPointer());

    auto result = Protected(contextRef, makeErrorValue(contextRef, errorMessage));

    m_lastErrorAccessed = false;
    m_lastError = result;

    if (handler) {
        errorMessage = makeString("Unchecked runtime.lastError: "_s, errorMessage);
        handler();
    }

    if (!m_lastErrorAccessed) {
        // Log the error to the console if it wasn't checked in the callback.
        JSRetainPtr consoleString = toJSString("console"_s);
        JSRetainPtr errorString = toJSString("error"_s);
        JSObjectRef globalObject = JSContextGetGlobalObject(contextRef);
        // This is a safer cpp false positive (rdar://163760990).
        SUPPRESS_UNCOUNTED_ARG JSObjectRef consoleObject = JSValueToObject(contextRef, JSObjectGetProperty(contextRef, globalObject, consoleString.get(), nullptr), nullptr);
        if (consoleObject) {
            SUPPRESS_UNCOUNTED_ARG JSValueRef consoleErrorFunction = JSObjectGetProperty(contextRef, consoleObject, errorString.get(), nullptr);

            callObjectWithArguments<1>(consoleErrorFunction, contextRef, { makeErrorValue(contextRef, errorMessage) });

            if (handler)
                RELEASE_LOG_DEBUG(Extensions, "Unchecked runtime.lastError");
        }
    }

    m_lastErrorAccessed = false;
    m_lastError = Protected<JSValueRef>();

    return result.get();
}

JSValueRef WebExtensionAPIRuntimeBase::reportError(const String& errorMessage, WebExtensionCallbackHandler& callback)
{
    return reportError(errorMessage, callback.globalContext(), [&]() {
        callback.call();
    });
}

bool WebExtensionAPIRuntime::parseConnectOptions(RefPtr<JSON::Value> options, std::optional<String>& name, const String& sourceKey, String& outExceptionString)
{
    if (!options)
        return true;

    if (!validateDictionary(options, sourceKey, { }, {
        { nameKey, JSON::Value::Type::String }
    }, outExceptionString))
        return false;

    auto runtimeObject = options->asObject();
    if (!runtimeObject)
        return false;

    auto nameString = runtimeObject->getString(nameKey);
    if (!nameString.isEmpty())
        name = nameString;

    return true;
}

bool WebExtensionAPIRuntime::isPropertyAllowed(const ASCIILiteral& name, WebPage*)
{
    Ref extensionContext = this->extensionContext();
    if (extensionContext->isUnsupportedAPI(propertyPath(), name)) [[unlikely]]
        return false;

    if (name == "connectNative"_s || name == "sendNativeMessage"_s)
        return extensionContext->hasPermission("nativeMessaging"_s);

    ASSERT_NOT_REACHED();
    return false;
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::getManifest()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getManifest

    return protect(extensionContext())->manifest();
}

URL WebExtensionAPIRuntime::getURL(const String& resourcePath, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getURL

    return URL { extensionContext().baseURL(), resourcePath };
}

String WebExtensionAPIRuntime::getVersion()
{
    RefPtr manifest = protect(extensionContext())->manifest();
    return manifest ? manifest->getString(versionKey) : nullString();
}

// Chrome enum objects map UPPER_CASE names to the string values the API uses.
static RefPtr<JSON::Object> enumObject(std::initializer_list<std::pair<ASCIILiteral, ASCIILiteral>> members)
{
    Ref object = JSON::Object::create();
    for (auto& [name, value] : members)
        object->setString(name, value);
    return object;
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::onInstalledReasons()
{
    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/runtime#type-OnInstalledReason
    return enumObject({ { "INSTALL"_s, "install"_s }, { "UPDATE"_s, "update"_s }, { "CHROME_UPDATE"_s, "chrome_update"_s }, { "SHARED_MODULE_UPDATE"_s, "shared_module_update"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::onRestartRequiredReasons()
{
    return enumObject({ { "APP_UPDATE"_s, "app_update"_s }, { "OS_UPDATE"_s, "os_update"_s }, { "PERIODIC"_s, "periodic"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::platformOperatingSystems()
{
    return enumObject({ { "MAC"_s, "mac"_s }, { "WIN"_s, "win"_s }, { "ANDROID"_s, "android"_s }, { "CROS"_s, "cros"_s }, { "LINUX"_s, "linux"_s }, { "OPENBSD"_s, "openbsd"_s }, { "FUCHSIA"_s, "fuchsia"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::platformArchitectures()
{
    return enumObject({ { "ARM"_s, "arm"_s }, { "ARM64"_s, "arm64"_s }, { "X86_32"_s, "x86-32"_s }, { "X86_64"_s, "x86-64"_s }, { "MIPS"_s, "mips"_s }, { "MIPS64"_s, "mips64"_s }, { "RISCV64"_s, "riscv64"_s } });
}

RefPtr<JSON::Object> WebExtensionAPIRuntime::contextTypes()
{
    // The values exist even though runtime.getContexts() does not; callers test for that function.
    return enumObject({ { "TAB"_s, "TAB"_s }, { "POPUP"_s, "POPUP"_s }, { "BACKGROUND"_s, "BACKGROUND"_s }, { "OFFSCREEN_DOCUMENT"_s, "OFFSCREEN_DOCUMENT"_s }, { "SIDE_PANEL"_s, "SIDE_PANEL"_s }, { "DEVELOPER_TOOLS"_s, "DEVELOPER_TOOLS"_s } });
}

String WebExtensionAPIRuntime::runtimeIdentifier()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/id

    return extensionContext().uniqueIdentifier();
}

#if PLATFORM(HAIKU)
void WebExtensionAPIRuntime::getBrowserInfo(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getBrowserInfo
    // Only Firefox defines this function, and its callers gate Gecko features on the result.
    // Report the Gecko release whose WebExtensions behavior this implementation follows.
    callback->call(JSValueMakeFromJSONString(callback->globalContext(),
        toJSString("{\"name\":\"Firefox\",\"vendor\":\"Mozilla\",\"version\":\"140.0\",\"buildID\":\"20250623000000\"}"_s).get()));
}

void WebExtensionAPIRuntime::requestUpdateCheck(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/runtime#method-requestUpdateCheck
    // Installed packages are only replaced through the extension manager.
    callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString("{\"status\":\"no_update\"}"_s).get()));
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onSuspend()
{
    if (!m_onSuspend)
        m_onSuspend = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnSuspend);
    return *m_onSuspend;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onSuspendCanceled()
{
    if (!m_onSuspendCanceled)
        m_onSuspendCanceled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnSuspendCanceled);
    return *m_onSuspendCanceled;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onUpdateAvailable()
{
    if (!m_onUpdateAvailable)
        m_onUpdateAvailable = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnUpdateAvailable);
    return *m_onUpdateAvailable;
}
#endif

void WebExtensionAPIRuntime::getPlatformInfo(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getPlatformInfo

#if PLATFORM(MAC)
    static constexpr auto osValue = "mac"_s;
#elif PLATFORM(IOS_FAMILY)
    static constexpr auto osValue = "ios"_s;
#elif PLATFORM(HAIKU)
    // The API defines a closed set of values. Extensions use it to choose keyboard
    // and path conventions, and "linux" is the closest member of that set.
    static constexpr auto osValue = "linux"_s;
#else
    static constexpr auto osValue = "unknown"_s;
#endif

#if CPU(X86_64)
    static constexpr auto archValue = "x86-64"_s;
#elif CPU(ARM) || CPU(ARM64)
    static constexpr auto archValue = "arm"_s;
#else
    static constexpr auto archValue = "unknown"_s;
#endif

    auto globalContext = callback->globalContext();
    // This is a safer cpp false positive (rdar://163760990).
    SUPPRESS_UNCOUNTED_ARG callback->call(fromObject(callback->globalContext(), {
        { "os"_s, Protected(globalContext, JSValueMakeString(globalContext, toJSString(osValue).get())) },
        { "arch"_s, Protected(globalContext, JSValueMakeString(globalContext, toJSString(archValue).get())) }
    }));
}

void WebExtensionAPIRuntime::getBackgroundPage(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getBackgroundPage

    if (auto backgroundPage = extensionContext().backgroundPage()) {
        callback->call(toWindowObject(callback->globalContext(), *backgroundPage));
        return;
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeGetBackgroundPage(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<std::optional<WebCore::PageIdentifier>, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }

        if (!result.value()) {
            callback->call(JSValueMakeNull(callback->globalContext()));
            return;
        }

        RefPtr page = WebProcess::singleton().webPage(result.value().value());
        if (!page) {
            callback->call(JSValueMakeNull(callback->globalContext()));
            return;
        }

        callback->call(toWindowObject(callback->globalContext(), *page));
    }, extensionContext().identifier());
}

double WebExtensionAPIRuntime::getFrameId(JSContextRef context, JSValueRef target)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/getFrameId

    if (!target)
        return WebExtensionFrameConstants::None;

    RefPtr frame = WebFrame::contentFrameForWindowOrFrameElement(context, target);
    if (!frame)
        return WebExtensionFrameConstants::None;

    return toWebAPI(toWebExtensionFrameIdentifier(*frame));
}

String WebExtensionAPIRuntime::getDocumentId(JSContextRef context, JSValueRef target, String& outExceptionString)
{
    // Documentation: https://github.com/w3c/webextensions/blob/main/proposals/runtime_get_document_id.md

    RefPtr frame = target ? WebFrame::contentFrameForWindowOrFrameElement(context, target) : nullptr;
    if (!frame) {
        outExceptionString = toErrorString(nullString(), "target"_s, "is not a valid window or frame element"_s);
        return String();
    }

    auto documentIdentifier = toDocumentIdentifier(*frame);
    if (!documentIdentifier) {
        outExceptionString = toErrorString(nullString(), nullString(), "an unexpected error occurred"_s);
        return String();
    }

    return documentIdentifier.value().toString();
}

void WebExtensionAPIRuntime::setUninstallURL(URL, Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/setUninstallURL

    // FIXME: rdar://58000001 Consider implementing runtime.setUninstallURL(), matching the behavior of other browsers.

    callback->call();
}

void WebExtensionAPIRuntime::openOptionsPage(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/openOptionsPage

#if PLATFORM(COCOA)
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeOpenOptionsPage(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }

        callback->call();
    }, extensionContext().identifier());
#endif
}

void WebExtensionAPIRuntime::reload()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/reload

    WebProcess::singleton().send(Messages::WebExtensionContext::RuntimeReload(), extensionContext().identifier());
}

JSValueRef WebExtensionAPIRuntime::lastError()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/lastError

    m_lastErrorAccessed = true;

    return m_lastError.get();
}

void WebExtensionAPIRuntime::sendMessage(WebPageProxyIdentifier webPageProxyIdentifier, WebFrame& frame, const String& extensionID, const String& messageJSON, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/sendMessage

    if (messageJSON.length() > webExtensionMaxMessageLength) {
        outExceptionString = toErrorString(nullString(), "message"_s, "it exceeded the maximum allowed length"_s);
        return;
    }

    auto documentIdentifier = toDocumentIdentifier(frame);
    if (!documentIdentifier) {
        outExceptionString = toErrorString(nullString(), nullString(), "an unexpected error occured"_s);
        return;
    }

    // No options are supported currently.

    WebExtensionMessageSenderParameters senderParameters {
        extensionContext().uniqueIdentifier(),
        std::nullopt, // tabParameters
        toWebExtensionFrameIdentifier(frame),
        webPageProxyIdentifier,
        contentWorldType(),
        frame.url(),
        documentIdentifier.value(),
    };

    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeSendMessage(extensionID, messageJSON, senderParameters, userGesture), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }

        callback->call(fromJSON(callback->globalContext(), JSON::Value::parseJSON(result.value())));
    }, extensionContext().identifier());
}

RefPtr<WebExtensionAPIPort> WebExtensionAPIRuntime::connect(WebPageProxyIdentifier webPageProxyIdentifier, WebFrame& frame, JSContextRef context, const String& extensionID, RefPtr<JSON::Value> options, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/connect

    auto documentIdentifier = toDocumentIdentifier(frame);
    if (!documentIdentifier) {
        outExceptionString = toErrorString(nullString(), nullString(), "an unexpected error occured"_s);
        return nullptr;
    }

    std::optional<String> name;
    if (!parseConnectOptions(options, name, "options"_s, outExceptionString))
        return nullptr;

    String resolvedName = name.value_or(nullString());

    WebExtensionMessageSenderParameters senderParameters {
        extensionContext().uniqueIdentifier(),
        std::nullopt, // tabParameters
        toWebExtensionFrameIdentifier(frame),
        webPageProxyIdentifier,
        contentWorldType(),
        frame.url(),
        documentIdentifier.value(),
    };

    Ref port = WebExtensionAPIPort::create(*this, webPageProxyIdentifier, WebExtensionContentWorldType::Main, resolvedName);

    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeConnect(extensionID, port->channelIdentifier(), resolvedName, senderParameters, userGesture), [=, this, protectedThis = Ref { *this }, globalContext = JSRetainPtr { JSContextGetGlobalContext(context) }](std::expected<void, WebExtensionError>&& result) {
        if (result)
            return;

        port->setError(globalContext.get(), protect(runtime())->reportError(result.error(), globalContext.get()));
        port->disconnect();
    }, extensionContext().identifier());

    return port;
}

void WebExtensionAPIRuntime::sendNativeMessage(WebFrame& frame, const String& applicationID, const String& messageJSON, Ref<WebExtensionCallbackHandler>&& callback)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/sendNativeMessage

#if PLATFORM(COCOA) || PLATFORM(HAIKU)
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeSendNativeMessage(applicationID, messageJSON), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }

        callback->call(fromJSON(callback->globalContext(), JSON::Value::parseJSON(result.value())));
    }, extensionContext().identifier());
#else
    // No native messaging hosts can be registered on this platform yet. Settle the
    // request so that callers can fall back instead of waiting forever.
    UNUSED_PARAM(frame);
    UNUSED_PARAM(messageJSON);
    callback->reportError(makeString("Specified native messaging host '"_s, applicationID, "' not found."_s));
#endif
}

RefPtr<WebExtensionAPIPort> WebExtensionAPIRuntime::connectNative(WebPageProxyIdentifier webPageProxyIdentifier, JSContextRef context, const String& applicationID)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/connectNative

    Ref port = WebExtensionAPIPort::create(*this, webPageProxyIdentifier, WebExtensionContentWorldType::Native, applicationID);

    // On Haiku the UI process accepts the port and then disconnects it with
    // the reason, so onDisconnect listeners observe runtime.lastError.
#if PLATFORM(COCOA) || PLATFORM(HAIKU)
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeConnectNative(applicationID, port->channelIdentifier(), webPageProxyIdentifier), [=, this, protectedThis = Ref { *this }, globalContext = JSRetainPtr { JSContextGetGlobalContext(context) }](std::expected<void, WebExtensionError>&& result) {
        if (result)
            return;

        port->setError(globalContext.get(), protect(runtime())->reportError(result.error(), globalContext.get()));
        port->disconnect();
    }, extensionContext().identifier());
#else
    // Disconnect on a later turn, after the caller has attached its onDisconnect listener.
    RunLoop::mainSingleton().dispatch([port, protectedThis = Ref { *this }, applicationID = applicationID.isolatedCopy(), globalContext = JSRetainPtr { JSContextGetGlobalContext(context) }] {
        port->setError(globalContext.get(), protect(protectedThis->runtime())->reportError(makeString("Specified native messaging host '"_s, applicationID, "' not found."_s), globalContext.get()));
        port->disconnect();
    });
#endif

    return port;
}

void WebExtensionAPIWebPageRuntime::sendMessage(WebPage& page, WebFrame& frame, const String& extensionID, const String& messageJSON, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/sendMessage

    if (messageJSON.length() > webExtensionMaxMessageLength) {
        outExceptionString = toErrorString(nullString(), "message"_s, "it exceeded the maximum allowed length"_s);
        return;
    }

    auto documentIdentifier = toDocumentIdentifier(frame);
    if (!documentIdentifier) {
        outExceptionString = toErrorString(nullString(), nullString(), "an unexpected error occured"_s);
        return;
    }

    WebExtensionMessageSenderParameters senderParameters {
        std::nullopt, // unique identifer
        std::nullopt, // tabParameters
        toWebExtensionFrameIdentifier(frame),
        page.webPageProxyIdentifier(),
        WebExtensionContentWorldType::WebPage,
        frame.url(),
        documentIdentifier.value(),
    };

    RefPtr destinationExtensionContext = page.webExtensionControllerProxy()->extensionContext(extensionID);
    if (!destinationExtensionContext) {
        // Respond after a random delay to prevent the page from easily detecting if extensions are not installed.
        callAfterRandomDelay([callback = WTF::move(callback)]() {
            callback->call();
        });

        return;
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeWebPageSendMessage(extensionID, messageJSON, senderParameters), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        if (!result) {
            callback->call();
            return;
        }

        callback->call(fromJSON(callback->globalContext(), JSON::Value::parseJSON(result.value())));
    }, destinationExtensionContext->identifier());
}

RefPtr<WebExtensionAPIPort> WebExtensionAPIWebPageRuntime::connect(WebPage& page, WebFrame& frame, JSContextRef context, const String& extensionID, RefPtr<JSON::Value> options, String& outExceptionString)
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/connect

    std::optional<String> name;
    if (!WebExtensionAPIRuntime::parseConnectOptions(options, name, "options"_s, outExceptionString))
        return nullptr;

    auto documentIdentifier = toDocumentIdentifier(frame);
    if (!documentIdentifier) {
        outExceptionString = toErrorString(nullString(), nullString(), "an unexpected error occured"_s);
        return nullptr;
    }

    String resolvedName = name.value_or(nullString());

    WebExtensionMessageSenderParameters senderParameters {
        std::nullopt, // unique identifier
        std::nullopt, // tabParameters
        toWebExtensionFrameIdentifier(frame),
        page.webPageProxyIdentifier(),
        WebExtensionContentWorldType::WebPage,
        frame.url(),
        documentIdentifier.value(),
    };

    RefPtr destinationExtensionContext = page.webExtensionControllerProxy()->extensionContext(extensionID);
    if (!destinationExtensionContext) {
        // Return a port that cant send messages, and disconnect after a random delay to prevent the page from easily detecting if extensions are not installed.
        Ref port = WebExtensionAPIPort::create(*this, resolvedName);

        callAfterRandomDelay([=]() {
            port->disconnect();
        });

        return port;
    }

    Ref port = WebExtensionAPIPort::create(contentWorldType(), protect(runtime()), *destinationExtensionContext, page.webPageProxyIdentifier(), WebExtensionContentWorldType::Main, resolvedName);

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::RuntimeWebPageConnect(extensionID, port->channelIdentifier(), resolvedName, senderParameters), [=, this, protectedThis = Ref { *this }, globalContext = JSRetainPtr { JSContextGetGlobalContext(context) }](std::expected<void, WebExtensionError>&& result) {
        if (result)
            return;

        port->setError(globalContext.get(), protect(runtime())->reportError(result.error(), globalContext.get()));
        port->disconnect();
    }, destinationExtensionContext->identifier());

    return port;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onMessage()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onMessage

    if (!m_onMessage)
        m_onMessage = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnMessage);

    return *m_onMessage;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onConnect()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onConnect

    if (!m_onConnect)
        m_onConnect = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnConnect);

    return *m_onConnect;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onInstalled()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onInstalled

    if (!m_onInstalled)
        m_onInstalled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnInstalled);

    return *m_onInstalled;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onStartup()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onStartup

    if (!m_onStartup)
        m_onStartup = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnStartup);

    return *m_onStartup;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onConnectExternal()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onConnectExternal

    if (!m_onConnectExternal)
        m_onConnectExternal = WebExtensionAPIEvent::create(*this,  WebExtensionEventListenerType::RuntimeOnConnectExternal);

    return *m_onConnectExternal;
}

WebExtensionAPIEvent& WebExtensionAPIRuntime::onMessageExternal()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/runtime/onMessageExternal

    if (!m_onMessageExternal)
        m_onMessageExternal = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::RuntimeOnMessageExternal);

    return *m_onMessageExternal;
}

bool WebExtensionContextProxy::matchesTarget(WebFrame& frame, const std::optional<WebExtensionMessageTargetParameters>& targetParameters)
{
    if (!targetParameters)
        return true;

    // Skip all pages / frames / documents that don't match the target parameters.
    auto& pageProxyIdentifier = targetParameters.value().pageProxyIdentifier;
    if (pageProxyIdentifier && pageProxyIdentifier != frame.page()->webPageProxyIdentifier())
        return false;

    auto& frameIdentifier = targetParameters.value().frameIdentifier;
    if (frameIdentifier && !matchesFrame(frameIdentifier.value(), frame))
        return false;

    if (auto& documentIdentifier = targetParameters.value().documentIdentifier) {
        auto frameDocumentIdentifier = toDocumentIdentifier(frame);
        if (!frameDocumentIdentifier)
            return false;

        if (documentIdentifier != frameDocumentIdentifier)
            return false;
    }

    return true;
}

void WebExtensionContextProxy::internalDispatchRuntimeMessageEvent(WebExtensionContentWorldType contentWorldType, const String& messageJSON, const std::optional<WebExtensionMessageTargetParameters>& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(String&& replyJSON)>&& completionHandler, bool* hadListener)
{
    if (!hasDOMWrapperWorld(contentWorldType)) {
        // A null reply to the completionHandler means no listeners replied.
        completionHandler({ });
        return;
    }

    auto sourceContentWorldType = senderParameters.contentWorldType;
    auto callbackAggregator = createWebExtensionMessageReply(WTF::move(completionHandler));

    bool anyListenerHandledMessage = false;
    enumerateFramesAndNamespaceObjects([&](WebFrame& frame, WebExtensionAPINamespace& namespaceObject) {
        // Don't send the message to any listeners in the sender's page.
        if (senderParameters.pageProxyIdentifier == frame.page()->webPageProxyIdentifier())
            return;

        // Skip all frames that don't match the target parameters.
        if (!matchesTarget(frame, targetParameters))
            return;

        WebExtensionAPIEvent::ListenerVector listeners;
        if (sourceContentWorldType == WebExtensionContentWorldType::WebPage)
            listeners = protect(namespaceObject.runtime())->onMessageExternal().listeners();
        else
            listeners = protect(namespaceObject.runtime())->onMessage().listeners();

        if (listeners.isEmpty())
            return;

        if (hadListener)
            *hadListener = true;

        std::optional<WebCore::UserGestureIndicator> gestureIndicator;
        if (userGesture) {
            RefPtr coreFrame = frame.coreLocalFrame();
            gestureIndicator.emplace(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));
        }

        for (auto& listener : listeners) {
            auto context = listener->globalContext();
            Protected<JSValueRef> senderInfo(context, toWebAPI(context, senderParameters));
            Protected<JSValueRef> replyFunction(context, createWebExtensionMessageReplyFunction(context, callbackAggregator.copyRef()));
            Protected<JSValueRef> returnValue(context, listener->call(deserializeJSONString(context, messageJSON), senderInfo.get(), replyFunction.get()));

            if (JSValueIsBoolean(context, returnValue.get()) && JSValueToBoolean(context, returnValue.get())) {
                anyListenerHandledMessage = true;
                continue;
            }

            if (!isThenable(context, returnValue.get()))
                continue;

            anyListenerHandledMessage = true;

            JSValueRef exception = nullptr;
            invokeWebExtensionMessageReplyThenable(context, returnValue.get(), callbackAggregator.copyRef(), exception);

            if (exception) {
                auto* globalObject = toJS(context);
                JSC::JSLockHolder lock(globalObject->vm());
                WebCore::reportException(globalObject, toJS(globalObject, exception));
            }
        }
    }, toDOMWrapperWorld(contentWorldType));

    if (!anyListenerHandledMessage)
        callbackAggregator.get()(nullptr, nullptr);
}

#if PLATFORM(HAIKU)
void WebExtensionContextProxy::dispatchTabsMessageEvent(WebExtensionContentWorldType contentWorldType, const String& messageJSON, const WebExtensionMessageTargetParameters& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(String&& replyJSON, bool hadListener)>&& completionHandler)
{
    auto hadListener = Box<bool>::create(false);
    // Frame enumeration is synchronous. The box also survives asynchronous
    // sendResponse/Promise callbacks so delivery and response stay distinct.
    internalDispatchRuntimeMessageEvent(contentWorldType, messageJSON, targetParameters, senderParameters, userGesture,
        [hadListener, completionHandler = WTF::move(completionHandler)](String&& reply) mutable {
            completionHandler(WTF::move(reply), *hadListener);
        }, hadListener.get());
}
#endif

void WebExtensionContextProxy::dispatchRuntimeMessageEvent(WebExtensionContentWorldType contentWorldType, const String& messageJSON, const std::optional<WebExtensionMessageTargetParameters>& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(String&& replyJSON)>&& completionHandler)
{
    switch (contentWorldType) {
    case WebExtensionContentWorldType::Main:
#if ENABLE(INSPECTOR_EXTENSIONS)
    case WebExtensionContentWorldType::Inspector:
#endif
        internalDispatchRuntimeMessageEvent(contentWorldType, messageJSON, targetParameters, senderParameters, userGesture, WTF::move(completionHandler));
        return;

    case WebExtensionContentWorldType::ContentScript:
        internalDispatchRuntimeMessageEvent(contentWorldType, messageJSON, targetParameters, senderParameters, userGesture, WTF::move(completionHandler));
        return;

    case WebExtensionContentWorldType::Native:
    case WebExtensionContentWorldType::WebPage:
        ASSERT_NOT_REACHED();
        return;
    }
}

void WebExtensionContextProxy::internalDispatchRuntimeConnectEvent(WebExtensionContentWorldType contentWorldType, WebExtensionPortChannelIdentifier channelIdentifier, const String& name, const std::optional<WebExtensionMessageTargetParameters>& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(HashCountedSet<WebPageProxyIdentifier>&&)>&& completionHandler)
{
    if (!hasDOMWrapperWorld(contentWorldType)) {
        completionHandler({ });
        return;
    }

    HashCountedSet<WebPageProxyIdentifier> firedEventCounts;
    auto sourceContentWorldType = senderParameters.contentWorldType;

    enumerateFramesAndNamespaceObjects([&](auto& frame, auto& namespaceObject) {
        // Don't send the event to any listeners in the sender's page.
        auto webPageProxyIdentifier = frame.page()->webPageProxyIdentifier();
        if (senderParameters.pageProxyIdentifier == webPageProxyIdentifier)
            return;

        // Skip all frames that don't match the target parameters.
        if (!matchesTarget(frame, targetParameters))
            return;

        WebExtensionAPIEvent::ListenerVector listeners;
        if (sourceContentWorldType == WebExtensionContentWorldType::WebPage)
            listeners = namespaceObject.runtime().onConnectExternal().listeners();
        else
            listeners = namespaceObject.runtime().onConnect().listeners();

        if (listeners.isEmpty())
            return;

        firedEventCounts.add(webPageProxyIdentifier, listeners.size());

        std::optional<WebCore::UserGestureIndicator> gestureIndicator;
        if (userGesture) {
            RefPtr coreFrame = frame.coreLocalFrame();
            gestureIndicator.emplace(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));
        }

        auto globalContext = frame.jsContextForWorld(toDOMWrapperWorld(contentWorldType));
        for (auto& listener : listeners) {
            Ref port = WebExtensionAPIPort::create(namespaceObject, frame.page()->webPageProxyIdentifier(), sourceContentWorldType, channelIdentifier, name, senderParameters);
            listener->call(toJS(globalContext, port.ptr()));
        }
    }, toDOMWrapperWorld(contentWorldType));

    completionHandler(WTF::move(firedEventCounts));
}

void WebExtensionContextProxy::dispatchRuntimeConnectEvent(WebExtensionContentWorldType contentWorldType, WebExtensionPortChannelIdentifier channelIdentifier, const String& name, const std::optional<WebExtensionMessageTargetParameters>& targetParameters, const WebExtensionMessageSenderParameters& senderParameters, bool userGesture, CompletionHandler<void(HashCountedSet<WebPageProxyIdentifier>&&)>&& completionHandler)
{
    switch (contentWorldType) {
    case WebExtensionContentWorldType::Main:
#if ENABLE(INSPECTOR_EXTENSIONS)
    case WebExtensionContentWorldType::Inspector:
#endif
        internalDispatchRuntimeConnectEvent(contentWorldType, channelIdentifier, name, targetParameters, senderParameters, userGesture, WTF::move(completionHandler));
        return;

    case WebExtensionContentWorldType::ContentScript:
        internalDispatchRuntimeConnectEvent(contentWorldType, channelIdentifier, name, targetParameters, senderParameters, userGesture, WTF::move(completionHandler));
        return;

    case WebExtensionContentWorldType::Native:
    case WebExtensionContentWorldType::WebPage:
        ASSERT_NOT_REACHED();
        return;
    }
}

inline String toWebAPI(WebExtensionContext::InstallReason installReason)
{
    switch (installReason) {
    case WebExtensionContext::InstallReason::None:
        ASSERT_NOT_REACHED();
        return nullString();

    case WebExtensionContext::InstallReason::ExtensionInstall:
        return "install"_s;

    case WebExtensionContext::InstallReason::ExtensionUpdate:
        return "update"_s;

    case WebExtensionContext::InstallReason::BrowserUpdate:
        return "browser_update"_s;
    }

    ASSERT_NOT_REACHED();
    return nullString();
}

void WebExtensionContextProxy::dispatchRuntimeInstalledEvent(WebExtensionContext::InstallReason installReason, String previousVersion)
{
    Ref<JSON::Object> details = JSON::Object::create();

    details->setString(reasonKey, toWebAPI(installReason));

    if (installReason == WebExtensionContext::InstallReason::ExtensionUpdate && !previousVersion.isEmpty())
        details->setString(previousVersionKey, previousVersion);

    enumerateNamespaceObjects([&](auto& namespaceObject) {
        namespaceObject.runtime().onInstalled().invokeListenersWithJSONArgument(details->toJSONString());
    });
}

void WebExtensionContextProxy::dispatchRuntimeStartupEvent()
{
    enumerateNamespaceObjects([&](auto& namespaceObject) {
        namespaceObject.runtime().onStartup().invokeListeners();
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
