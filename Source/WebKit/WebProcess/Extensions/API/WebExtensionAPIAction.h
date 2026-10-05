/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionAPIAction.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"
#include "WebPageProxyIdentifier.h"
#include <wtf/JSONValues.h>

OBJC_CLASS NSDictionary;
OBJC_CLASS NSString;

namespace WebKit {

class WebExtensionAPIAction : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIAction, action, action);

public:
    void getTitle(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setTitle(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void getBadgeText(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setBadgeText(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void enable(JSContextRef, JSValueRef, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void disable(JSContextRef, JSValueRef, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void isEnabled(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void getPopup(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setPopup(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    WebExtensionAPIEvent& onClicked();

    void getBadgeBackgroundColor(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setBadgeBackgroundColor(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    // Preserve native ImageData wrappers for the platform's icon converter.
    void setIcon(WebFrame&, JSContextRef, JSValueRef, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void openPopup(WebPageProxyIdentifier, RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
#if PLATFORM(HAIKU)
    void getBadgeTextColor(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setBadgeTextColor(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void getUserSettings(Ref<WebExtensionCallbackHandler>&&);
#endif
#if PLATFORM(COCOA)
    void getTitle(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);
    void setTitle(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, String& outExceptionString);

    void getBadgeText(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void setBadgeText(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    void getBadgeBackgroundColor(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void setBadgeBackgroundColor(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    void enable(double tabIdentifier, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void disable(double tabIdentifier, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void isEnabled(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    void setIcon(WebFrame&, NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    void getPopup(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void setPopup(NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void openPopup(WebPageProxyIdentifier, NSDictionary *details, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);


private:
    friend class WebExtensionAPIMenus;

    static bool isValidDimensionKey(NSString *);
    static NSString *parseIconPath(NSString *path, const URL& baseURL);
    static NSMutableDictionary *parseIconPathsDictionary(NSDictionary *, const URL& baseURL, bool forVariants, NSString *inputKey, NSString **outExceptionString);
    static NSMutableDictionary *parseIconImageDataDictionary(NSDictionary *, bool forVariants, NSString *inputKey, NSString **outExceptionString);

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    static NSArray *parseIconVariants(NSArray *, const URL& baseURL, NSString *inputKey, NSString **outExceptionString);
#endif

    static std::expected<void, String> parseActionDetails(NSDictionary *, std::optional<WebExtensionWindowIdentifier>&, std::optional<WebExtensionTabIdentifier>&);

#endif
private:
#if PLATFORM(HAIKU)
    void getBadgeColor(bool background, RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void setBadgeColor(bool background, RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&&, String& exception);
#endif
    RefPtr<WebExtensionAPIEvent> m_onClicked;
};

} // namespace WebKit

SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIAction, action);

#endif // ENABLE(WK_WEB_EXTENSIONS)
