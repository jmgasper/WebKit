/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <JavaScriptCore/JSValueRef.h>
namespace WebKit {
struct WebExtensionWindowParameters;
JSValueRef toWebAPI(JSContextRef, const WebExtensionWindowParameters&);
}
#endif
