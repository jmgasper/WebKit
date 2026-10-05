/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIPrivacy.h"
#include "WebExtensionAPIPrivacyCategory.h"

namespace WebKit {
class WebExtensionAPIPrivacy final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIPrivacy, privacy, privacy);
public:
    WebExtensionAPIPrivacyCategory& network();
    WebExtensionAPIPrivacyCategory& services();
    WebExtensionAPIPrivacyCategory& websites();
    void dispatchChanged(const String& setting, const String& detailsJSON);
private:
    RefPtr<WebExtensionAPIPrivacyCategory> m_network;
    RefPtr<WebExtensionAPIPrivacyCategory> m_services;
    RefPtr<WebExtensionAPIPrivacyCategory> m_websites;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIPrivacy, privacy);
#endif
