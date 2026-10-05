/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
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

#include "JSWebExtensionAPINamespace.h"
#include "WebExtensionAPIAction.h"
#include "WebExtensionAPIAlarms.h"
#if ENABLE(WK_WEB_EXTENSIONS_BOOKMARKS)
#include "WebExtensionAPIBookmarks.h"
#endif
#include "WebExtensionAPICommands.h"
#include "WebExtensionAPICookies.h"
#include "WebExtensionAPIDOM.h"
#include "WebExtensionAPIDeclarativeNetRequest.h"
#include "WebExtensionAPIDevTools.h"
#include "WebExtensionAPIExtension.h"
#include "WebExtensionAPILocalization.h"
#include "WebExtensionAPIMenus.h"
#include "WebExtensionAPINotifications.h"
#include "WebExtensionAPIObject.h"
#include "WebExtensionAPIOffscreen.h"
#include "WebExtensionAPIPermissions.h"
#if PLATFORM(HAIKU)
#include "WebExtensionAPIDownloads.h"
#include "WebExtensionAPIIdle.h"
#include "WebExtensionAPIManagement.h"
#include "WebExtensionAPINotificationsHaiku.h"
#include "WebExtensionAPIPrivacy.h"
#endif
#include "WebExtensionAPIRuntime.h"
#include "WebExtensionAPIScripting.h"
#include "WebExtensionAPISidePanel.h"
#include "WebExtensionAPISidebarAction.h"
#include "WebExtensionAPIStorage.h"
#include "WebExtensionAPITabs.h"
#include "WebExtensionAPITest.h"
#include "WebExtensionAPIWebNavigation.h"
#include "WebExtensionAPIWebRequest.h"
#include "WebExtensionAPIWindows.h"

namespace WebKit {

class WebExtensionAPIExtension;
class WebExtensionAPIRuntime;

class WebExtensionAPINamespace : public WebExtensionAPIObject, public JSWebExtensionWrappable {
    WEB_EXTENSION_DECLARE_JS_WRAPPER_CLASS(WebExtensionAPINamespace, namespace, browser);

public:
    bool isPropertyAllowed(const ASCIILiteral& propertyName, WebPage*);
    WebExtensionAPIAction& action();
    WebExtensionAPIAlarms& alarms();
    WebExtensionAPICommands& commands();
    WebExtensionAPIAction& browserAction() { return action(); }
    WebExtensionAPIAction& pageAction() { return action(); }
    WebExtensionAPILocalization& i18n();
    WebExtensionAPICookies& cookies();
    WebExtensionAPIMenus& menus();
    WebExtensionAPIMenus& contextMenus() { return menus(); }
    WebExtensionAPIExtension& extension();
    WebExtensionAPIDeclarativeNetRequest& declarativeNetRequest();
#if PLATFORM(COCOA)
#if ENABLE(INSPECTOR_EXTENSIONS)
    WebExtensionAPIDevTools& devtools();
#endif
    WebExtensionAPIDOM& dom();
    WebExtensionAPINotifications& notifications();
#endif
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    WebExtensionAPIOffscreen& offscreen();
#endif
    WebExtensionAPIPermissions& permissions();
#if PLATFORM(HAIKU)
    WebExtensionAPIDownloads& downloads();
    WebExtensionAPIIdle& idle();
    WebExtensionAPIManagement& management();
    WebExtensionAPINotificationsHaiku& notificationsHaiku();
    WebExtensionAPIPrivacy& privacy();
#endif
    WebExtensionAPIRuntime& runtime() const final;
    WebExtensionAPIScripting& scripting();
#if PLATFORM(COCOA)
#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
    WebExtensionAPISidePanel& sidePanel();
    WebExtensionAPISidebarAction& sidebarAction();
#endif
#if ENABLE(WK_WEB_EXTENSIONS_BOOKMARKS)
    WebExtensionAPIBookmarks& bookmarks();
#endif
#endif
    WebExtensionAPITabs& tabs();
    WebExtensionAPIStorage& storage();
    WebExtensionAPITest& test();
    WebExtensionAPIWebRequest& webRequest();
    WebExtensionAPIWebNavigation& webNavigation();
    WebExtensionAPIWindows& windows();

private:
    RefPtr<WebExtensionAPIAction> m_action;
    RefPtr<WebExtensionAPIAlarms> m_alarms;
    RefPtr<WebExtensionAPICommands> m_commands;
    RefPtr<WebExtensionAPICookies> m_cookies;
    RefPtr<WebExtensionAPIDeclarativeNetRequest> m_declarativeNetRequest;
#if ENABLE(INSPECTOR_EXTENSIONS)
    RefPtr<WebExtensionAPIDevTools> m_devtools;
#endif
    RefPtr<WebExtensionAPIDOM> m_dom;
    RefPtr<WebExtensionAPIExtension> m_extension;
    RefPtr<WebExtensionAPILocalization> m_i18n;
    RefPtr<WebExtensionAPIMenus> m_menus;
    RefPtr<WebExtensionAPINotifications> m_notifications;
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    RefPtr<WebExtensionAPIOffscreen> m_offscreen;
#endif
    RefPtr<WebExtensionAPIPermissions> m_permissions;
#if PLATFORM(HAIKU)
    RefPtr<WebExtensionAPIDownloads> m_downloads;
    RefPtr<WebExtensionAPIIdle> m_idle;
    RefPtr<WebExtensionAPIManagement> m_management;
    RefPtr<WebExtensionAPINotificationsHaiku> m_notificationsHaiku;
    RefPtr<WebExtensionAPIPrivacy> m_privacy;
#endif
    mutable RefPtr<WebExtensionAPIRuntime> m_runtime;
    RefPtr<WebExtensionAPIScripting> m_scripting;
#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
    RefPtr<WebExtensionAPISidePanel> m_sidePanel;
    RefPtr<WebExtensionAPISidebarAction> m_sidebarAction;
#endif
#if ENABLE(WK_WEB_EXTENSIONS_BOOKMARKS)
    RefPtr<WebExtensionAPIBookmarks> m_bookmarks;
#endif
    RefPtr<WebExtensionAPIStorage> m_storage;
    RefPtr<WebExtensionAPITabs> m_tabs;
    RefPtr<WebExtensionAPITest> m_test;
    RefPtr<WebExtensionAPIWindows> m_windows;
    RefPtr<WebExtensionAPIWebNavigation> m_webNavigation;
    RefPtr<WebExtensionAPIWebRequest> m_webRequest;
};

} // namespace WebKit

SPECIALIZE_TYPE_TRAITS_WEB_EXTENSION(WebExtensionAPINamespace, namespace);

#endif // ENABLE(WK_WEB_EXTENSIONS)
