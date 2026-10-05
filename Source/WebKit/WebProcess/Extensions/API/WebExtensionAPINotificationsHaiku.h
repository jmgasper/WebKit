/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPINotificationsHaiku.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"

namespace WebKit {
class WebExtensionAPINotificationsHaiku final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPINotificationsHaiku, notificationsHaiku, notifications);
public:
    void createNotification(const String& identifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void update(const String& identifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void clear(const String& identifier, Ref<WebExtensionCallbackHandler>&&);
    void getAll(Ref<WebExtensionCallbackHandler>&&);
    void getPermissionLevel(Ref<WebExtensionCallbackHandler>&&);
    WebExtensionAPIEvent& onButtonClicked();
    WebExtensionAPIEvent& onClicked();
    WebExtensionAPIEvent& onClosed();
    WebExtensionAPIEvent& onShown();
    void dispatchShown(const String& identifier);
    void dispatchClosed(const String& identifier, bool byUser);
private:
    void present(const String& identifier, const JSON::Object& options);
    RefPtr<WebExtensionAPIEvent> m_onButtonClicked;
    RefPtr<WebExtensionAPIEvent> m_onClicked;
    RefPtr<WebExtensionAPIEvent> m_onClosed;
    RefPtr<WebExtensionAPIEvent> m_onShown;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPINotificationsHaiku, notificationsHaiku);
#endif
