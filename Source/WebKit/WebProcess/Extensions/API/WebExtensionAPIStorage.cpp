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
#include "WebExtensionAPIStorage.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionString.h"
#include "Protected.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionAPIStorageArea.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionDataType.h"
#include <wtf/JSONValues.h>

namespace WebKit {

bool WebExtensionAPIStorage::isPropertyAllowed(const ASCIILiteral& propertyName, WebPage*)
{
    if (protect(extensionContext())->isUnsupportedAPI(propertyPath(), propertyName)) [[unlikely]]
        return false;

    if (isForTrustedContext())
        return true;

    // Chrome exposes the (policy-provided, read-only) managed area to content scripts as well.
    if (propertyName == "managed"_s)
        return true;

    return extensionContext().isStorageAllowedInUntrustedContexts(toWebExtensionDataType(propertyName));
}

WebExtensionAPIStorageArea& WebExtensionAPIStorage::local()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/storage/local

    if (!m_local)
        m_local = WebExtensionAPIStorageArea::create(*this, WebExtensionDataType::Local);

    return *m_local;
}

WebExtensionAPIStorageArea& WebExtensionAPIStorage::session()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/storage/session

    if (!m_session)
        m_session = WebExtensionAPIStorageArea::create(*this, WebExtensionDataType::Session);

    return *m_session;
}

WebExtensionAPIStorageArea& WebExtensionAPIStorage::sync()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/storage/sync

    if (!m_sync)
        m_sync = WebExtensionAPIStorageArea::create(*this, WebExtensionDataType::Sync);

    return *m_sync;
}

WebExtensionAPIStorageArea& WebExtensionAPIStorage::managed()
{
    // Documentation: https://developer.chrome.com/docs/extensions/reference/api/storage#property-managed
    // Grammarly calls storage.managed.get() unguarded during first start; the missing
    // area made that handler throw before it enabled itself in open tabs.
    if (!m_managed)
        m_managed = WebExtensionAPIStorageArea::create(*this, WebExtensionDataType::Local, WebExtensionAPIStorageArea::Managed::Yes);

    return *m_managed;
}

WebExtensionAPIStorageArea& WebExtensionAPIStorage::storageAreaForType(WebExtensionDataType storageType)
{
    switch (storageType) {
    case WebExtensionDataType::Local:
        return local();
    case WebExtensionDataType::Session:
        return session();
    case WebExtensionDataType::Sync:
        return sync();
    }

    ASSERT_NOT_REACHED();
    return local();
}

WebExtensionAPIEvent& WebExtensionAPIStorage::onChanged()
{
    // Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/storage/onChanged

    if (!m_onChanged)
        m_onChanged = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::StorageOnChanged);

    return *m_onChanged;
}

void WebExtensionContextProxy::dispatchStorageChangedEvent(const Vector<String>& changesJSON, WebExtensionDataType dataType, WebExtensionContentWorldType contentWorldType)
{
    if (!hasDOMWrapperWorld(contentWorldType))
        return;

    Ref changes = JSON::Object::create();
    for (auto& json : changesJSON) {
        auto parsed = JSON::Value::parseJSON(json);
        auto object = parsed ? parsed->asObject() : nullptr;
        if (!object)
            return;
        for (auto& entry : *object)
            changes->setValue(entry.key, entry.value.copyRef());
    }
    auto serializedChanges = changes->toJSONString();
    auto areaName = toAPIString(dataType);
    auto invokeListeners = [&](WebExtensionAPIEvent& event) {
        // A listener may add or remove listeners while it runs.
        auto listeners = event.listeners();
        for (RefPtr listener : listeners) {
            auto context = listener->globalContext();
            auto value = JSValueMakeFromJSONString(context, toJSString(serializedChanges).get());
            if (!value)
                continue;
            Protected<JSValueRef> protectedValue(context, value);
            listener->call(value, JSValueMakeString(context, toJSString(areaName).get()));
        }
    };

    enumerateFramesAndNamespaceObjects([&](WebFrame&, auto& namespaceObject) {
        invokeListeners(namespaceObject.storage().onChanged());
        invokeListeners(namespaceObject.storage().storageAreaForType(dataType).onChanged());
    }, toDOMWrapperWorld(contentWorldType));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
