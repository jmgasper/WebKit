/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "ContentWorldData.h"
#include "WebExtensionContextIdentifier.h"
#include "WebExtensionTabScriptFrameResult.h"
#include "WebExtensionTabScriptTarget.h"
#include <WebCore/SerializedScriptValue.h>

namespace WebKit {
using WebExtensionTabScriptExecutionResult = std::expected<RefPtr<WebCore::SerializedScriptValue>, String>;
}
#endif
