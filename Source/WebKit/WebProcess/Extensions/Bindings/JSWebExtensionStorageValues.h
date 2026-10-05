/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Protected.h"
#include <JavaScriptCore/JavaScript.h>
#include <wtf/HashMap.h>
#include <wtf/JSONValues.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>
#include <expected>
#include <optional>

namespace WebKit {

struct WebExtensionStorageValues {
    Ref<JSON::Object> serializedValues;
    size_t sizeInBytes { 0 };
};

// Storage's wire format maps each key to its separately serialized JSON value.
std::expected<WebExtensionStorageValues, String> serializeWebExtensionStorageValues(JSContextRef, JSValueRef, std::optional<size_t> perItemQuota, size_t maximumBytes);

enum class WebExtensionStorageKeyMode : uint8_t { Get, GetBytesInUse, Remove };

struct WebExtensionStorageKeySelection {
    Vector<String> keys;
    HashMap<String, Protected<JSValueRef>> defaultValues;
    bool allKeys { false };
};

std::expected<WebExtensionStorageKeySelection, String> parseWebExtensionStorageKeys(JSContextRef, JSValueRef, WebExtensionStorageKeyMode);
std::expected<Protected<JSObjectRef>, String> deserializeWebExtensionStorageValues(JSContextRef, const Vector<String>&, const HashMap<String, Protected<JSValueRef>>& defaultValues);

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
