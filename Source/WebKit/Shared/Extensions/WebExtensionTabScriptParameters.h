/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionFrameIdentifier.h"
#include <wtf/UUID.h>
#include <wtf/text/WTFString.h>
namespace WebKit {
enum class WebExtensionTabScriptRunAt : uint8_t { DocumentStart, DocumentEnd, DocumentIdle };
struct WebExtensionTabScriptParameters {
    std::optional<String> code;
    std::optional<String> file;
    std::optional<WebExtensionFrameIdentifier> frameIdentifier;
    std::optional<WTF::UUID> documentIdentifier;
    WebExtensionTabScriptRunAt runAt { WebExtensionTabScriptRunAt::DocumentIdle };
    bool allFrames { false };
    bool matchAboutBlank { false };
};
}
#endif
