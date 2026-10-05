/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIManagement.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"

namespace WebKit {
class WebExtensionAPIManagement final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIManagement, management, management);
public:
    void getSelf(Ref<WebExtensionCallbackHandler>&&);
    void getAll(Ref<WebExtensionCallbackHandler>&&);
    void get(const String& identifier, Ref<WebExtensionCallbackHandler>&&);
    void setEnabled(const String& identifier, bool enabled, Ref<WebExtensionCallbackHandler>&&);
    void uninstallSelf(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&&);
    WebExtensionAPIEvent& onDisabled();
    WebExtensionAPIEvent& onEnabled();
    WebExtensionAPIEvent& onInstalled();
    WebExtensionAPIEvent& onUninstalled();
private:
    Ref<JSON::Object> selfInfo();
    RefPtr<WebExtensionAPIEvent> m_onDisabled;
    RefPtr<WebExtensionAPIEvent> m_onEnabled;
    RefPtr<WebExtensionAPIEvent> m_onInstalled;
    RefPtr<WebExtensionAPIEvent> m_onUninstalled;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIManagement, management);
#endif
