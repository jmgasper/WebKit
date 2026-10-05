/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionAPIPermissions.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionPermissionState.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"
#include <WebCore/UserGestureIndicator.h>

namespace WebKit {

template<typename Names, typename Origins>
static Ref<JSON::Object> permissionObject(const Names& names, const Origins& origins)
{
    auto object = JSON::Object::create();
    auto named = JSON::Array::create();
    auto hosts = JSON::Array::create();
    for (auto& name : names)
        named->pushString(name);
    for (auto& origin : origins)
        hosts->pushString(origin);
    object->setArray("permissions"_s, WTF::move(named));
    object->setArray("origins"_s, WTF::move(hosts));
    return object;
}

void WebExtensionAPIPermissions::getAll(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PermissionsGetAll(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](Vector<String> names, Vector<String> origins) {
        auto json = permissionObject(names, origins)->toJSONString();
        auto context = callback->globalContext();
        Protected<JSValueRef> result(context, JSValueMakeFromJSONString(context, toJSString(json).get()));
        callback->call(result.get());
    }, extensionContext().identifier());
}

void WebExtensionAPIPermissions::contains(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto parsed = parseWebExtensionPermissionDetails(details.get());
    if (!parsed) {
        exception = parsed.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PermissionsContains(parsed->permissions, parsed->origins), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<bool, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call(JSValueMakeBoolean(callback->globalContext(), *result));
    }, extensionContext().identifier());
}

void WebExtensionAPIPermissions::request(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto parsed = parseWebExtensionPermissionDetails(details.get());
    if (!parsed) {
        exception = parsed.error();
        return;
    }
    bool userGesture = WebCore::UserGestureIndicator::processingUserGesture();
    if (!userGesture) {
        callback->reportError(toWebExtensionError("permissions.request()"_s, nullString(), "a user gesture is required"_s).error());
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PermissionsRequest(parsed->permissions, parsed->origins, userGesture), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<bool, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call(JSValueMakeBoolean(callback->globalContext(), *result));
    }, extensionContext().identifier());
}

void WebExtensionAPIPermissions::remove(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    auto parsed = parseWebExtensionPermissionDetails(details.get());
    if (!parsed) {
        exception = parsed.error();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PermissionsRemove(parsed->permissions, parsed->origins), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<bool, WebExtensionError>&& result) {
        if (!result)
            callback->reportError(result.error());
        else
            callback->call(JSValueMakeBoolean(callback->globalContext(), *result));
    }, extensionContext().identifier());
}

WebExtensionAPIEvent& WebExtensionAPIPermissions::onAdded()
{
    if (!m_onAdded)
        m_onAdded = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::PermissionsOnAdded);
    return *m_onAdded;
}

WebExtensionAPIEvent& WebExtensionAPIPermissions::onRemoved()
{
    if (!m_onRemoved)
        m_onRemoved = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::PermissionsOnRemoved);
    return *m_onRemoved;
}

void WebExtensionContextProxy::dispatchPermissionsEvent(WebExtensionEventListenerType type, HashSet<String> names, HashSet<String> origins)
{
    if (type != WebExtensionEventListenerType::PermissionsOnAdded && type != WebExtensionEventListenerType::PermissionsOnRemoved)
        return;
    auto json = permissionObject(names, origins)->toJSONString();
    enumerateNamespaceObjects([&](auto& namespaceObject) {
        auto& api = namespaceObject.permissions();
        auto& event = type == WebExtensionEventListenerType::PermissionsOnAdded ? api.onAdded() : api.onRemoved();
        // The existing event implementation snapshots listeners and creates a
        // fresh rooted object for each one, preserving listener isolation.
        event.invokeListenersWithJSONArgument(json);
    });
}

} // namespace WebKit

#endif
