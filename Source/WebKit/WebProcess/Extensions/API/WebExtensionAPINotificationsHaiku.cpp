/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPINotificationsHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionLocalization.h"
#include "WebExtensionUtilities.h"
#include <Notification.h>
#include <String.h>
#include <wtf/HashMap.h>
#include <wtf/JSONValues.h>
#include <wtf/NeverDestroyed.h>
#include <wtf/UUID.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/notifications
// Notifications are shown through the native notification server. It offers no
// activation callback to the sender, so onClicked and onButtonClicked never fire yet.
using NotificationMap = HashMap<String, Ref<JSON::Object>>;

static NotificationMap& notificationsForExtension(const String& extensionIdentifier)
{
    static NeverDestroyed<HashMap<String, NotificationMap>> notifications;
    return notifications->ensure(extensionIdentifier, [] { return NotificationMap { }; }).iterator->value;
}
static bool validateOptions(RefPtr<JSON::Value> options, bool creating, String& outExceptionString)
{
    return validateDictionary(options, "options"_s, creating ? Vector<String> { "title"_s, "message"_s } : Vector<String> { }, {
        { "type"_s, JSON::Value::Type::String },
        { "title"_s, JSON::Value::Type::String },
        { "message"_s, JSON::Value::Type::String },
        { "contextMessage"_s, JSON::Value::Type::String },
        { "iconUrl"_s, JSON::Value::Type::String },
    }, outExceptionString);
}
void WebExtensionAPINotificationsHaiku::present(const String& identifier, const JSON::Object& options)
{
    Ref context = extensionContext();
    RefPtr manifest = context->manifest();
    RefPtr localization = context->localization();
    auto group = manifest ? manifest->getString("name"_s) : String();
    if (localization && !group.isEmpty())
        group = localization->localizedStringForString(group);
    auto content = options.getString("message"_s);
    if (auto contextMessage = options.getString("contextMessage"_s); !contextMessage.isEmpty())
        content = makeString(content, '\n', contextMessage);

    BNotification notification(B_INFORMATION_NOTIFICATION);
    notification.SetGroup(BString(group.utf8().legacyCStringPointer()));
    notification.SetTitle(BString(options.getString("title"_s).utf8().legacyCStringPointer()));
    notification.SetContent(BString(content.utf8().legacyCStringPointer()));
    // Reusing the message identifier replaces a visible notification in place.
    notification.SetMessageID(BString(makeString(context->uniqueIdentifier(), ':', identifier).utf8().legacyCStringPointer()));
    notification.Send();
}
void WebExtensionAPINotificationsHaiku::createNotification(const String& identifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!validateOptions(options, true, outExceptionString))
        return;
    RefPtr object = options->asObject();
    if (!object)
        return;
    auto name = identifier.isEmpty() ? WTF::UUID::createVersion4().toString() : identifier;
    notificationsForExtension(extensionContext().uniqueIdentifier()).set(name, *object);
    present(name, *object);
    callback->call(toJSValueRef(callback->globalContext(), name));
    extensionContext().enumerateNamespaceObjects([&](auto& object) { object.notificationsHaiku().dispatchShown(name); });
}
void WebExtensionAPINotificationsHaiku::update(const String& identifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!validateOptions(options, false, outExceptionString))
        return;
    RefPtr changes = options->asObject();
    auto& notifications = notificationsForExtension(extensionContext().uniqueIdentifier());
    auto found = notifications.find(identifier);
    if (!changes || found == notifications.end()) {
        callback->call(JSValueMakeBoolean(callback->globalContext(), false));
        return;
    }
    Ref current = found->value;
    for (auto& key : changes->keys()) {
        if (RefPtr value = changes->getValue(key))
            current->setValue(key, value.releaseNonNull());
    }
    present(identifier, current);
    callback->call(JSValueMakeBoolean(callback->globalContext(), true));
}
void WebExtensionAPINotificationsHaiku::clear(const String& identifier, Ref<WebExtensionCallbackHandler>&& callback)
{
    bool removed = notificationsForExtension(extensionContext().uniqueIdentifier()).remove(identifier);
    callback->call(JSValueMakeBoolean(callback->globalContext(), removed));
    if (removed)
        extensionContext().enumerateNamespaceObjects([&](auto& object) { object.notificationsHaiku().dispatchClosed(identifier, false); });
}
void WebExtensionAPINotificationsHaiku::getAll(Ref<WebExtensionCallbackHandler>&& callback)
{
    Ref result = JSON::Object::create();
    for (auto& entry : notificationsForExtension(extensionContext().uniqueIdentifier()))
        result->setObject(entry.key, entry.value.copyRef());
    callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString(result->toJSONString()).get()));
}
void WebExtensionAPINotificationsHaiku::getPermissionLevel(Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->call(toJSValueRef(callback->globalContext(), "granted"_s));
}
void WebExtensionAPINotificationsHaiku::dispatchShown(const String& identifier)
{
    if (m_onShown)
        m_onShown->invokeListenersWithJSONArgument(JSON::Value::create(identifier)->toJSONString());
}
void WebExtensionAPINotificationsHaiku::dispatchClosed(const String& identifier, bool byUser)
{
    if (m_onClosed)
        m_onClosed->invokeListenersWithJSONArgument(JSON::Value::create(identifier)->toJSONString(), JSON::Value::create(byUser)->toJSONString());
}
WebExtensionAPIEvent& WebExtensionAPINotificationsHaiku::onButtonClicked()
{
    if (!m_onButtonClicked)
        m_onButtonClicked = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::NotificationsOnButtonClicked);
    return *m_onButtonClicked;
}
WebExtensionAPIEvent& WebExtensionAPINotificationsHaiku::onClicked()
{
    if (!m_onClicked)
        m_onClicked = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::NotificationsOnClicked);
    return *m_onClicked;
}
WebExtensionAPIEvent& WebExtensionAPINotificationsHaiku::onClosed()
{
    if (!m_onClosed)
        m_onClosed = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::NotificationsOnClosed);
    return *m_onClosed;
}
WebExtensionAPIEvent& WebExtensionAPINotificationsHaiku::onShown()
{
    if (!m_onShown)
        m_onShown = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::NotificationsOnShown);
    return *m_onShown;
}
}
#endif
