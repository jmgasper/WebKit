/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionFrameIdentifier.h"
#include <WebCore/SerializedScriptValue.h>
#include <wtf/UUID.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

// One frame's outcome of scripting.executeScript(). The value is cloned into
// the calling extension page; a non-null error replaces it.
struct WebExtensionTabScriptFrameResult {
    WebExtensionFrameIdentifier frameIdentifier { WebExtensionFrameConstants::MainFrameIdentifier };
    std::optional<WTF::UUID> documentIdentifier;
    RefPtr<WebCore::SerializedScriptValue> value;
    String error;
};

}
#endif
