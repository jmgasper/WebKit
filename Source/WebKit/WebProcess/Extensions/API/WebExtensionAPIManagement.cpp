/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIManagement.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContextProxy.h"
#include "WebExtensionLocalization.h"
#include "WebExtensionUtilities.h"
#include <wtf/JSONValues.h>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/management
// An extension only learns about itself here. Other installed extensions are not
// disclosed, which matches what an unprivileged extension may rely on across browsers.
static JSValueRef toJSValue(WebExtensionCallbackHandler& callback, const JSON::Value& value)
{
    return JSValueMakeFromJSONString(callback.globalContext(), toJSString(value.toJSONString()).get());
}
Ref<JSON::Object> WebExtensionAPIManagement::selfInfo()
{
    Ref context = extensionContext();
    RefPtr manifest = context->manifest();
    RefPtr localization = context->localization();
    auto text = [&](ASCIILiteral key) {
        auto value = manifest ? manifest->getString(key) : String();
        return localization && !value.isEmpty() ? localization->localizedStringForString(value) : value;
    };
    Ref info = JSON::Object::create();
    info->setString("id"_s, context->uniqueIdentifier());
    info->setString("name"_s, text("name"_s));
    info->setString("shortName"_s, text("short_name"_s).isEmpty() ? text("name"_s) : text("short_name"_s));
    info->setString("description"_s, text("description"_s));
    info->setString("version"_s, text("version"_s));
    if (auto versionName = text("version_name"_s); !versionName.isEmpty())
        info->setString("versionName"_s, versionName);
    info->setBoolean("enabled"_s, true);
    info->setBoolean("mayDisable"_s, true);
    info->setBoolean("mayEnable"_s, true);
    info->setString("type"_s, "extension"_s);
    info->setString("installType"_s, "normal"_s);
    if (auto homepage = text("homepage_url"_s); !homepage.isEmpty())
        info->setString("homepageUrl"_s, homepage);
    auto optionsPage = manifest ? manifest->getString("options_page"_s) : String();
    if (optionsPage.isEmpty() && manifest) {
        if (RefPtr optionsUI = manifest->getObject("options_ui"_s))
            optionsPage = optionsUI->getString("page"_s);
    }
    info->setString("optionsUrl"_s, optionsPage.isEmpty() ? emptyString() : URL { context->baseURL(), optionsPage }.string());
    Ref icons = JSON::Array::create();
    if (RefPtr manifestIcons = manifest ? manifest->getObject("icons"_s) : nullptr) {
        for (auto& key : manifestIcons->keys()) {
            auto size = parseInteger<unsigned>(key);
            auto path = manifestIcons->getString(key);
            if (!size || path.isEmpty())
                continue;
            Ref icon = JSON::Object::create();
            icon->setInteger("size"_s, *size);
            icon->setString("url"_s, URL { context->baseURL(), path }.string());
            icons->pushObject(WTF::move(icon));
        }
    }
    info->setArray("icons"_s, WTF::move(icons));
    Ref permissions = JSON::Array::create();
    Ref hostPermissions = JSON::Array::create();
    for (auto key : { "permissions"_s, "host_permissions"_s }) {
        RefPtr values = manifest ? manifest->getArray(key) : nullptr;
        if (!values)
            continue;
        for (Ref value : *values) {
            auto name = value->asString();
            if (name.isEmpty())
                continue;
            if (name.contains(':') || name == "<all_urls>"_s)
                hostPermissions->pushString(name);
            else
                permissions->pushString(name);
        }
    }
    info->setArray("permissions"_s, WTF::move(permissions));
    info->setArray("hostPermissions"_s, WTF::move(hostPermissions));
    return info;
}
void WebExtensionAPIManagement::getSelf(Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->call(toJSValue(callback, selfInfo()));
}
void WebExtensionAPIManagement::getAll(Ref<WebExtensionCallbackHandler>&& callback)
{
    Ref result = JSON::Array::create();
    result->pushObject(selfInfo());
    callback->call(toJSValue(callback, result));
}
void WebExtensionAPIManagement::get(const String& identifier, Ref<WebExtensionCallbackHandler>&& callback)
{
    if (identifier != extensionContext().uniqueIdentifier()) {
        callback->reportError(makeString("No extension with the identifier '"_s, identifier, "' is visible to this extension."_s));
        return;
    }
    callback->call(toJSValue(callback, selfInfo()));
}
void WebExtensionAPIManagement::setEnabled(const String& identifier, bool, Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->reportError(makeString("Extensions cannot enable or disable '"_s, identifier, "'; use the browser's extension manager."_s));
}
void WebExtensionAPIManagement::uninstallSelf(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->reportError("Removal must be confirmed in the browser's extension manager."_s);
}
WebExtensionAPIEvent& WebExtensionAPIManagement::onDisabled()
{
    if (!m_onDisabled)
        m_onDisabled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::ManagementOnDisabled);
    return *m_onDisabled;
}
WebExtensionAPIEvent& WebExtensionAPIManagement::onEnabled()
{
    if (!m_onEnabled)
        m_onEnabled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::ManagementOnEnabled);
    return *m_onEnabled;
}
WebExtensionAPIEvent& WebExtensionAPIManagement::onInstalled()
{
    if (!m_onInstalled)
        m_onInstalled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::ManagementOnInstalled);
    return *m_onInstalled;
}
WebExtensionAPIEvent& WebExtensionAPIManagement::onUninstalled()
{
    if (!m_onUninstalled)
        m_onUninstalled = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::ManagementOnUninstalled);
    return *m_onUninstalled;
}
}
#endif
