/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS)
#include "WebExtensionCookieParameters.h"
#include "WebExtensionTabIdentifier.h"
#include <JavaScriptCore/JavaScript.h>
#include <WebCore/CookieChange.h>
#include <pal/SessionID.h>
#include <wtf/HashMap.h>
namespace WebKit {
JSValueRef toWebAPI(JSContextRef, const WebExtensionCookieParameters&);
JSValueRef toWebAPICookieChange(JSContextRef, PAL::SessionID, const WebCore::CookieChange&);
JSValueRef toWebAPICookieRemoval(JSContextRef, const URL&, const WebExtensionCookieParameters&);
JSValueRef toWebAPI(JSContextRef, const Vector<WebExtensionCookieParameters>&);
JSValueRef toWebAPI(JSContextRef, const HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>>&, PAL::SessionID ownSession);
}
#endif
