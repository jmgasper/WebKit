/*
 * Copyright (C) 2024 Apple Inc. All rights reserved.
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
#include "WebExtensionAPIStorageArea.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionStorageValues.h"
#include "MessageSenderInlines.h"
#include "WebExtensionConstants.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionStorageAccessLevel.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"

namespace WebKit {

bool WebExtensionAPIStorageArea::isPropertyAllowed(const ASCIILiteral& propertyName, WebPage*)
{
    if (protect(extensionContext())->isUnsupportedAPI(propertyPath(), propertyName)) [[unlikely]]
        return false;

    static NeverDestroyed<HashSet<AtomString>> syncStorageProperties { HashSet { AtomString("QUOTA_BYTES_PER_ITEM"_s), AtomString("MAX_ITEMS"_s), AtomString("MAX_WRITE_OPERATIONS_PER_HOUR"_s), AtomString("MAX_WRITE_OPERATIONS_PER_MINUTE"_s) } };

    if (syncStorageProperties.get().contains(propertyName))
        return m_type == WebExtensionDataType::Sync;

    if (propertyName == "setAccessLevel"_s)
        return isForTrustedContext();

    ASSERT_NOT_REACHED();
    return false;
}

void WebExtensionAPIStorageArea::get(WebPageProxyIdentifier webPageProxyIdentifier, JSContextRef context, JSValueRef items, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    auto selection = parseWebExtensionStorageKeys(context, items, WebExtensionStorageKeyMode::Get);
    if (!selection) {
        outExceptionString = selection.error();
        return;
    }
    if (!selection->allKeys && selection->keys.isEmpty()) {
        callback->call(JSObjectMake(callback->globalContext(), nullptr, nullptr));
        return;
    }
    if (m_managed) {
        // No policy values: only the caller's defaults.
        auto values = deserializeWebExtensionStorageValues(callback->globalContext(), { }, selection->defaultValues);
        if (!values)
            callback->reportError(values.error());
        else
            callback->call(values->get());
        return;
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageGet(webPageProxyIdentifier, m_type, selection->keys), [defaultValues = WTF::move(selection->defaultValues), protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<String>, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        auto values = deserializeWebExtensionStorageValues(callback->globalContext(), *result, defaultValues);
        if (!values)
            callback->reportError(values.error());
        else
            callback->call(values->get());
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::getKeys(WebPageProxyIdentifier webPageProxyIdentifier, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (m_managed) {
        callback->call(fromArray(callback->globalContext(), Vector<String> { }));
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageGetKeys(webPageProxyIdentifier, m_type), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<Vector<String>, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call(fromArray(callback->globalContext(), WTF::move(*result)));
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::getBytesInUse(WebPageProxyIdentifier webPageProxyIdentifier, JSContextRef context, JSValueRef keys, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    auto selection = parseWebExtensionStorageKeys(context, keys, WebExtensionStorageKeyMode::GetBytesInUse);
    if (!selection) {
        outExceptionString = selection.error();
        return;
    }
    if (m_managed) {
        callback->call(JSValueMakeNumber(callback->globalContext(), 0));
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageGetBytesInUse(webPageProxyIdentifier, m_type, selection->keys), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<uint64_t, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call(JSValueMakeNumber(callback->globalContext(), *result));
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::set(WebPageProxyIdentifier webPageProxyIdentifier, JSContextRef context, JSValueRef items, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (m_managed) {
        callback->reportError("This is a read-only store."_s);
        return;
    }
    auto perItemQuota = m_type == WebExtensionDataType::Sync ? std::optional<size_t>(webExtensionStorageAreaSyncQuotaBytesPerItem) : std::nullopt;
    auto values = serializeWebExtensionStorageValues(context, items, perItemQuota, webExtensionStorageAreaMaximumBytesPerCall);
    if (!values) {
        outExceptionString = values.error();
        return;
    }
    if (!values->serializedValues->size()) {
        callback->call();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageSet(webPageProxyIdentifier, m_type, values->serializedValues->toJSONString()), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::remove(WebPageProxyIdentifier webPageProxyIdentifier, JSContextRef context, JSValueRef keys, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (m_managed) {
        callback->reportError("This is a read-only store."_s);
        return;
    }
    auto selection = parseWebExtensionStorageKeys(context, keys, WebExtensionStorageKeyMode::Remove);
    if (!selection) {
        outExceptionString = selection.error();
        return;
    }
    if (selection->keys.isEmpty()) {
        callback->call();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageRemove(webPageProxyIdentifier, m_type, selection->keys), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::clear(WebPageProxyIdentifier webPageProxyIdentifier, Ref<WebExtensionCallbackHandler>&& callback)
{
    if (m_managed) {
        callback->reportError("This is a read-only store."_s);
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageClear(webPageProxyIdentifier, m_type), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPIStorageArea::setAccessLevel(WebPageProxyIdentifier webPageProxyIdentifier, RefPtr<JSON::Value> accessOptions, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!accessOptions) {
        outExceptionString = toErrorString(nullString(), "accessOptions"_s, "an object is expected"_s);
        return;
    }
    if (!validateDictionary(accessOptions, "accessOptions"_s, { "accessLevel"_s }, { { "accessLevel"_s, JSON::Value::Type::String } }, outExceptionString))
        return;

    auto accessLevelString = accessOptions->asObject()->getString("accessLevel"_s);
    if (accessLevelString != "TRUSTED_CONTEXTS"_s && accessLevelString != "TRUSTED_AND_UNTRUSTED_CONTEXTS"_s) {
        outExceptionString = toErrorString(nullString(), "accessLevel"_s, "it must specify either 'TRUSTED_CONTEXTS' or 'TRUSTED_AND_UNTRUSTED_CONTEXTS'"_s);
        return;
    }
    auto accessLevel = accessLevelString == "TRUSTED_CONTEXTS"_s ? WebExtensionStorageAccessLevel::TrustedContexts : WebExtensionStorageAccessLevel::TrustedAndUntrustedContexts;
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::StorageSetAccessLevel(webPageProxyIdentifier, m_type, accessLevel), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call();
    }, extensionContext().identifier());
}

WebExtensionAPIEvent& WebExtensionAPIStorageArea::onChanged()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/storage/StorageArea/onChanged

    if (!m_onChanged)
        m_onChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::StorageOnChanged);

    return *m_onChanged;
}

double WebExtensionAPIStorageArea::quotaBytes()
{
    switch (m_type) {
    case WebExtensionDataType::Local:
        return webExtensionStorageAreaLocalQuotaBytes;
    case WebExtensionDataType::Sync:
        return webExtensionStorageAreaSyncQuotaBytes;
    case WebExtensionDataType::Session:
        return webExtensionStorageAreaSessionQuotaBytes;
    }

    ASSERT_NOT_REACHED();
    return 0;
}

double WebExtensionAPIStorageArea::quotaBytesPerItem()
{
    ASSERT(m_type == WebExtensionDataType::Sync);
    return webExtensionStorageAreaSyncQuotaBytesPerItem;
}

double WebExtensionAPIStorageArea::maxItems()
{
    ASSERT(m_type == WebExtensionDataType::Sync);
    return webExtensionStorageAreaSyncMaximumItems;
}

double WebExtensionAPIStorageArea::maxWriteOperationsPerHour()
{
    ASSERT(m_type == WebExtensionDataType::Sync);
    return webExtensionStorageAreaSyncMaximumWriteOperationsPerHour;
}

double WebExtensionAPIStorageArea::maxWriteOperationsPerMinute()
{
    ASSERT(m_type == WebExtensionDataType::Sync);
    return webExtensionStorageAreaSyncMaximumWriteOperationsPerMinute;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
