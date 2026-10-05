/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIPrivacy.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"
#include <wtf/text/MakeString.h>

namespace WebKit {
WebExtensionAPIPrivacyCategory& WebExtensionAPIPrivacy::network()
{
    if (!m_network)
        m_network = WebExtensionAPIPrivacyCategory::create(*this, "network"_s);
    return *m_network;
}
WebExtensionAPIPrivacyCategory& WebExtensionAPIPrivacy::services()
{
    if (!m_services)
        m_services = WebExtensionAPIPrivacyCategory::create(*this, "services"_s);
    return *m_services;
}
WebExtensionAPIPrivacyCategory& WebExtensionAPIPrivacy::websites()
{
    if (!m_websites)
        m_websites = WebExtensionAPIPrivacyCategory::create(*this, "websites"_s);
    return *m_websites;
}
bool WebExtensionAPIPrivacyCategory::isPropertyAllowed(const ASCIILiteral& name, WebPage*)
{
    if (!extensionContext().hasPermission("privacy"_s))
        return false;
    return (m_name == "network"_s && (name == "networkPredictionEnabled"_s || name == "webRTCIPHandlingPolicy"_s))
        || (m_name == "websites"_s && name == "hyperlinkAuditingEnabled"_s)
        || (m_name == "services"_s && (name == "autofillAddressEnabled"_s || name == "autofillCreditCardEnabled"_s
            || name == "autofillEnabled"_s || name == "passwordSavingEnabled"_s));
}
WebExtensionAPIPrivacySetting& WebExtensionAPIPrivacyCategory::setting(const String& name)
{
    return m_settings.ensure(name, [&] { return WebExtensionAPIPrivacySetting::create(*this, m_name, name); }).iterator->value.get();
}
void WebExtensionAPIPrivacySetting::get(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    if (!validateDictionary(details, "details"_s, { }, { { "incognito"_s, JSON::Value::Type::Boolean } }, exception))
        return;
    if (isBrowserFeatureSetting()) {
        // This browser has no built-in password manager or form autofill, so these
        // are permanently off and there is nothing for an extension to take over.
        callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString("{\"value\":false,\"levelOfControl\":\"not_controllable\"}"_s).get()));
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PrivacyGet(m_setting, details->toJSONString()),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](String&& result, String&& error) {
            if (!error.isEmpty() || result.isNull()) {
                callback->reportError(error.isEmpty() ? "The privacy setting context is unavailable."_s : error);
                return;
            }
            callback->call(JSValueMakeFromJSONString(callback->globalContext(), toJSString(result).get()));
        }, extensionContext().identifier());
}
void WebExtensionAPIPrivacySetting::set(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    update("set"_s, WTF::move(details), WTF::move(callback), exception);
}
void WebExtensionAPIPrivacySetting::clear(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    update("clear"_s, WTF::move(details), WTF::move(callback), exception);
}
void WebExtensionAPIPrivacySetting::update(const String& operation, RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& exception)
{
    if (!validateDictionary(details, "details"_s, operation == "set"_s ? Vector<String> { "value"_s } : Vector<String> { }, {
        { "scope"_s, JSON::Value::Type::String },
        { "value"_s, m_setting == "network.webRTCIPHandlingPolicy"_s ? JSON::Value::Type::String : JSON::Value::Type::Boolean },
    }, exception))
        return;
    if (isBrowserFeatureSetting()) {
        // Turning an absent feature off, or clearing the request, already matches the permanent state.
        RefPtr object = details->asObject();
        if (operation == "set"_s && object && object->getBoolean("value"_s).value_or(false))
            callback->reportError("This browser has no such built-in feature to enable."_s);
        else
            callback->call();
        return;
    }
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::PrivacyUpdate(m_setting, operation, details->toJSONString()),
        [protectedThis = Ref { *this }, callback = WTF::move(callback)](bool success, String&& error) {
            if (!success) {
                callback->reportError(error.isEmpty() ? "The privacy setting context is unavailable."_s : error);
                return;
            }
            callback->call();
        }, extensionContext().identifier());
}
WebExtensionAPIEvent& WebExtensionAPIPrivacySetting::onChange()
{
    if (!m_onChange)
        m_onChange = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::PrivacyOnChange);
    return *m_onChange;
}
void WebExtensionAPIPrivacySetting::dispatchChanged(const String& detailsJSON)
{
    if (m_onChange)
        m_onChange->invokeListenersWithJSONArgument(detailsJSON);
}
void WebExtensionAPIPrivacyCategory::dispatchChanged(const String& settingName, const String& detailsJSON)
{
    for (auto& entry : m_settings) {
        if (makeString(m_name, '.', entry.key) == settingName)
            entry.value->dispatchChanged(detailsJSON);
    }
}
void WebExtensionAPIPrivacy::dispatchChanged(const String& settingName, const String& detailsJSON)
{
    if (m_network)
        m_network->dispatchChanged(settingName, detailsJSON);
    if (m_websites)
        m_websites->dispatchChanged(settingName, detailsJSON);
}
void WebExtensionContextProxy::dispatchPrivacyChanged(const String& settingName, const String& detailsJSON)
{
    if (!hasPermission("privacy"_s))
        return;
    enumerateNamespaceObjects([&](auto& object) { object.privacy().dispatchChanged(settingName, detailsJSON); });
}
}
#endif
