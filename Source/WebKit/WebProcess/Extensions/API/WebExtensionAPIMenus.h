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

#include "JSWebExtensionAPIMenus.h"
#include "WebExtensionAPIEvent.h"
#include "WebExtensionAPIObject.h"
#include "WebExtensionMenuItemParameters.h"
#if PLATFORM(HAIKU)
#include "WebExtensionMenuClickHandlerStore.h"
#endif

OBJC_CLASS NSDictionary;
OBJC_CLASS NSString;

namespace WebKit {

class WebExtensionMenuClickHandlerStore;

class WebExtensionAPIMenus : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPIMenus, menus, menus);

public:
    JSValueRef createMenu(WebPage&, WebFrame&, JSContextRef, JSValueRef properties, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void update(WebPage&, WebFrame&, JSContextRef, JSValueRef identifier, JSValueRef properties, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void remove(JSContextRef, JSValueRef identifier, Ref<WebExtensionCallbackHandler>&&, String& exception);
    void removeAll(Ref<WebExtensionCallbackHandler>&&);
    WebExtensionAPIEvent& onClicked();
    double actionMenuTopLevelLimit() const { return webExtensionActionMenuItemTopLevelLimit; }
#if PLATFORM(HAIKU)
    ~WebExtensionAPIMenus();
#endif
#if PLATFORM(COCOA)
    using ClickHandlerMap = HashMap<String, Ref<WebExtensionCallbackHandler>>;

    id createMenu(WebPage&, WebFrame&, NSDictionary *properties, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);
    void update(WebPage&, WebFrame&, id identifier, NSDictionary *properties, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    void remove(id identifier, Ref<WebExtensionCallbackHandler>&&, NSString **outExceptionString);

    const ClickHandlerMap& clickHandlers() const LIFETIME_BOUND { return m_clickHandlerMap; }

private:
    enum class ForUpdate : bool { No, Yes };
    bool parseCreateAndUpdateProperties(ForUpdate, NSDictionary *, const URL& baseURL, std::optional<WebExtensionMenuItemParameters>&, RefPtr<WebExtensionCallbackHandler>&, NSString **outExceptionString);

    Markable<WebCore::FrameIdentifier> m_frameIdentifier;
    ClickHandlerMap m_clickHandlerMap;
#endif
private:
    RefPtr<WebExtensionAPIEvent> m_onClicked;
#if PLATFORM(HAIKU)
    RefPtr<WebExtensionMenuClickHandlerStore> ensureClickHandlerStore(JSContextRef, WebFrame&, String& exception);
    Vector<Ref<WebExtensionMenuClickHandlerStore>> m_clickHandlerStores;
#endif
};

} // namespace WebKit

SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPIMenus, menus);

#endif // ENABLE(WK_WEB_EXTENSIONS)
