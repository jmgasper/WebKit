/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIPrivacySetting.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"
#include <wtf/text/MakeString.h>

namespace WebKit {
class WebExtensionAPIPrivacySetting final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIPrivacySetting, privacySetting, privacySetting);
public:
    void get(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void set(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void clear(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    WebExtensionAPIEvent& onChange();
    void dispatchChanged(const String& detailsJSON);
private:
    WebExtensionAPIPrivacySetting(const WebExtensionAPIObject& parent, const String& category, const String& name)
        : WebExtensionAPIObject(parent), m_setting(makeString(category, '.', name))
    { setPropertyPath(name, &parent); }
    bool isBrowserFeatureSetting() const { return m_setting.startsWith("services."_s); }
    void update(const String& operation, RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    String m_setting;
    RefPtr<WebExtensionAPIEvent> m_onChange;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIPrivacySetting, privacySetting);
#endif
