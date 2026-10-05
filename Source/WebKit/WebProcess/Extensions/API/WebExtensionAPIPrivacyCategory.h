/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIPrivacyCategory.h"
#include "WebExtensionAPIPrivacySetting.h"
#include <wtf/HashMap.h>

namespace WebKit {
class WebExtensionAPIPrivacyCategory final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIPrivacyCategory, privacyCategory, privacyCategory);
public:
    bool isPropertyAllowed(const ASCIILiteral&, WebPage*);
    WebExtensionAPIPrivacySetting& networkPredictionEnabled() { return setting("networkPredictionEnabled"_s); }
    WebExtensionAPIPrivacySetting& webRTCIPHandlingPolicy() { return setting("webRTCIPHandlingPolicy"_s); }
    WebExtensionAPIPrivacySetting& hyperlinkAuditingEnabled() { return setting("hyperlinkAuditingEnabled"_s); }
    WebExtensionAPIPrivacySetting& autofillAddressEnabled() { return setting("autofillAddressEnabled"_s); }
    WebExtensionAPIPrivacySetting& autofillCreditCardEnabled() { return setting("autofillCreditCardEnabled"_s); }
    WebExtensionAPIPrivacySetting& autofillEnabled() { return setting("autofillEnabled"_s); }
    WebExtensionAPIPrivacySetting& passwordSavingEnabled() { return setting("passwordSavingEnabled"_s); }
    void dispatchChanged(const String& setting, const String& detailsJSON);
private:
    WebExtensionAPIPrivacyCategory(const WebExtensionAPIObject& parent, const String& name)
        : WebExtensionAPIObject(parent), m_name(name)
    { setPropertyPath(name, &parent); }
    WebExtensionAPIPrivacySetting& setting(const String&);
    String m_name;
    HashMap<String, Ref<WebExtensionAPIPrivacySetting>> m_settings;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIPrivacyCategory, privacyCategory);
#endif
