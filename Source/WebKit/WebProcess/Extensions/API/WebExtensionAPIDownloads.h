/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionAPIDownloads.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"
#include "WebPageProxyIdentifier.h"
#include <wtf/JSONValues.h>

namespace WebKit {
class WebExtensionAPIDownloads final : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIDownloads, downloads, downloads);
public:
    void download(WebPageProxyIdentifier, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void search(RefPtr<JSON::Value> query, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void cancel(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void erase(RefPtr<JSON::Value> query, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void open(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void show(double downloadIdentifier, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void showDefaultFolder(Ref<WebExtensionCallbackHandler>&&);

    WebExtensionAPIEvent& onCreated();
    WebExtensionAPIEvent& onChanged();
    WebExtensionAPIEvent& onErased();

    void dispatchEvent(WebExtensionEventListenerType, const String& argumentJSON);

private:
    RefPtr<WebExtensionAPIEvent> m_onCreated;
    RefPtr<WebExtensionAPIEvent> m_onChanged;
    RefPtr<WebExtensionAPIEvent> m_onErased;
};
}
SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIDownloads, downloads);
#endif
