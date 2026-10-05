/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS)
#include <pal/SessionID.h>
#include <wtf/text/WTFString.h>
namespace WebKit {
// Session identity conversion only. The context must separately check access.
std::optional<PAL::SessionID> parseWebExtensionCookieStoreIdentifier(const String&);
String webExtensionCookieStoreIdentifier(PAL::SessionID);
}
#endif
