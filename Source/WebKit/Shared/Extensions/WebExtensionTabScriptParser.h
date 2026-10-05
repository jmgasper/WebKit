/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionTabScriptParameters.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
// Parse the request only. Frame ownership, current-document identity, resource
// resolution, permissions and runAt scheduling belong to the execution path.
std::expected<WebExtensionTabScriptParameters, String> parseWebExtensionTabScript(const JSON::Value*);
}
#endif
