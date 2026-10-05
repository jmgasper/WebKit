/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIDownloads.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"
#include <cmath>
#include <limits>

// Argument types are checked here and the UI process validates every value
// again. Downloads are started by the UI process on behalf of the calling
// extension page, so a blob: URL created by that page stays resolvable.

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/downloads

static bool isValidDownloadIdentifier(double identifier)
{
    return std::isfinite(identifier) && std::trunc(identifier) == identifier && identifier >= 0 && identifier <= static_cast<double>(1ull << 53);
}

static void replyWithVoid(WebExtensionCallbackHandler& callback, std::expected<void, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    callback.call();
}

static void replyWithJSON(WebExtensionCallbackHandler& callback, std::expected<String, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    callback.call(fromJSON(callback.globalContext(), JSON::Value::parseJSON(result.value())));
}

void WebExtensionAPIDownloads::download(WebPageProxyIdentifier webPageProxyIdentifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    RefPtr object = options ? options->asObject() : nullptr;
    if (!object) {
        outExceptionString = toErrorString(nullString(), "options"_s, "it must be an object"_s);
        return;
    }
    RefPtr url = object->getValue("url"_s);
    if (!url || url->asString().isEmpty()) {
        outExceptionString = toErrorString(nullString(), "url"_s, "it must be a non-empty string"_s);
        return;
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsDownload(webPageProxyIdentifier, object->toJSONString()), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<double, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        callback->call(JSValueMakeNumber(callback->globalContext(), result.value()));
    }, extensionContext().identifier());
}

void WebExtensionAPIDownloads::search(RefPtr<JSON::Value> query, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!query || !query->asObject()) {
        outExceptionString = toErrorString(nullString(), "query"_s, "it must be an object"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsSearch(query->toJSONString()), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        replyWithJSON(callback, WTF::move(result));
    }, extensionContext().identifier());
}

void WebExtensionAPIDownloads::cancel(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!isValidDownloadIdentifier(downloadIdentifier)) {
        outExceptionString = toErrorString(nullString(), "downloadId"_s, "it must be a non-negative integer"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsCancel(downloadIdentifier), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        replyWithVoid(callback, WTF::move(result));
    }, extensionContext().identifier());
}

void WebExtensionAPIDownloads::erase(RefPtr<JSON::Value> query, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!query || !query->asObject()) {
        outExceptionString = toErrorString(nullString(), "query"_s, "it must be an object"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsErase(query->toJSONString()), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        replyWithJSON(callback, WTF::move(result));
    }, extensionContext().identifier());
}

void WebExtensionAPIDownloads::open(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!isValidDownloadIdentifier(downloadIdentifier)) {
        outExceptionString = toErrorString(nullString(), "downloadId"_s, "it must be a non-negative integer"_s);
        return;
    }
    // Opening a file launches its preferred application. That needs the
    // separate downloads.open permission and a user-gesture policy first.
    callback->reportError("downloads.open() is not supported on this platform."_s);
}

void WebExtensionAPIDownloads::show(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!isValidDownloadIdentifier(downloadIdentifier)) {
        outExceptionString = toErrorString(nullString(), "downloadId"_s, "it must be a non-negative integer"_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsShow(downloadIdentifier), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        // Firefox resolves with true; Chrome's callback takes no argument.
        callback->call(JSValueMakeBoolean(callback->globalContext(), true));
    }, extensionContext().identifier());
}

void WebExtensionAPIDownloads::showDefaultFolder(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DownloadsShowDefaultFolder(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        replyWithVoid(callback, WTF::move(result));
    }, extensionContext().identifier());
}

WebExtensionAPIEvent& WebExtensionAPIDownloads::onCreated()
{
    if (!m_onCreated)
        m_onCreated = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::DownloadsOnCreated);
    return *m_onCreated;
}

WebExtensionAPIEvent& WebExtensionAPIDownloads::onChanged()
{
    if (!m_onChanged)
        m_onChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::DownloadsOnChanged);
    return *m_onChanged;
}

WebExtensionAPIEvent& WebExtensionAPIDownloads::onErased()
{
    if (!m_onErased)
        m_onErased = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::DownloadsOnErased);
    return *m_onErased;
}

void WebExtensionAPIDownloads::dispatchEvent(WebExtensionEventListenerType type, const String& argumentJSON)
{
    // Each listener receives its own object parsed from the JSON argument.
    switch (type) {
    case WebExtensionEventListenerType::DownloadsOnCreated:
        if (m_onCreated)
            m_onCreated->invokeListenersWithJSONArgument(argumentJSON);
        break;
    case WebExtensionEventListenerType::DownloadsOnChanged:
        if (m_onChanged)
            m_onChanged->invokeListenersWithJSONArgument(argumentJSON);
        break;
    case WebExtensionEventListenerType::DownloadsOnErased:
        if (m_onErased)
            m_onErased->invokeListenersWithJSONArgument(argumentJSON);
        break;
    default:
        ASSERT_NOT_REACHED();
        break;
    }
}

void WebExtensionContextProxy::dispatchDownloadsEvent(WebExtensionEventListenerType type, const String& argumentJSON)
{
    if (type != WebExtensionEventListenerType::DownloadsOnCreated && type != WebExtensionEventListenerType::DownloadsOnChanged && type != WebExtensionEventListenerType::DownloadsOnErased)
        return;
    if (!hasPermission(WebExtensionPermission::downloads()))
        return;
    enumerateNamespaceObjects([&](auto& namespaceObject) {
        namespaceObject.downloads().dispatchEvent(type, argumentJSON);
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
