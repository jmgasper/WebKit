/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionTabQueryParameters.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
// Structural validation is shared. The privileged receiver also validates
// match patterns against supported schemes before inspecting browser tabs.
std::expected<WebExtensionTabQueryParameters, String> parseWebExtensionTabQuery(const JSON::Value*);
}
#endif
