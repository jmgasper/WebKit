/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include <JavaScriptCore/JavaScript.h>
#include <WebCore/FormData.h>

namespace WebKit {

// Uses the real upload elements without reading file contents. Returns null for
// no body; unsupported elements produce an error object, never partial data.
JSObjectRef webExtensionWebRequestBody(JSContextRef, std::span<const WebCore::FormDataElement>, const String& contentType, JSValueRef& exception);

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
