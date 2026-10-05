/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionMatchPattern.h"
#include <wtf/HashMap.h>
#include <wtf/WallTime.h>
namespace WebKit {
// Evaluate existing profile-wide host grants, including expiration and denial.
// Cookie API permission, context lifetime and private-store access are separate.
bool hasWebExtensionCookieHostAccess(const URL&, const HashMap<Ref<WebExtensionMatchPattern>, WallTime>& granted,
    const HashMap<Ref<WebExtensionMatchPattern>, WallTime>& denied, WallTime now);
}
#endif
