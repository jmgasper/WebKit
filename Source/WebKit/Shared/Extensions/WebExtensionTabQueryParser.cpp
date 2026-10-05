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

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionTabQueryParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <cmath>
#include <wtf/URL.h>
namespace WebKit {

std::expected<WebExtensionTabQueryParameters, String> parseWebExtensionTabQuery(const JSON::Value* input)
{
    RefPtr object = input ? input->asObject() : nullptr;
    if (!object)
        return makeUnexpected("query info must be an object"_s);
    WebExtensionTabQueryParameters result;
    for (auto& key : object->keys()) {
        Ref value = object->getValue(key).releaseNonNull();
        std::optional<bool>* boolean = nullptr;
        if (key == "active"_s)
            boolean = &result.active;
        else if (key == "audible"_s)
            boolean = &result.audible;
        else if (key == "discarded"_s)
            boolean = &result.discarded;
        else if (key == "currentWindow"_s)
            boolean = &result.currentWindow;
        else if (key == "hidden"_s)
            boolean = &result.hidden;
        else if (key == "highlighted"_s || key == "selected"_s)
            boolean = &result.selected;
        else if (key == "lastFocusedWindow"_s)
            boolean = &result.frontmostWindow;
        else if (key == "muted"_s)
            boolean = &result.muted;
        else if (key == "pinned"_s)
            boolean = &result.pinned;
        if (boolean) {
            auto parsed = value->asBoolean();
            if (!parsed)
                return makeUnexpected("tab query flags must be booleans"_s);
            *boolean = *parsed;
            continue;
        }
        if (key == "index"_s) {
            auto number = value->asDouble();
            if (!number || !std::isfinite(*number) || std::trunc(*number) != *number || *number < 0 || *number > 9007199254740991.0)
                return makeUnexpected("index must be a nonnegative safe integer"_s);
            result.index = static_cast<uint64_t>(*number);
        } else if (key == "windowId"_s) {
            auto number = value->asDouble();
            result.windowIdentifier = number && *number <= 9007199254740991.0 ? toWebExtensionWindowIdentifier(*number) : std::nullopt;
            if (!isValid(result.windowIdentifier))
                return makeUnexpected("windowId must be a valid window identifier"_s);
        } else if (key == "status"_s) {
            auto status = value->asString();
            if (status != "loading"_s && status != "complete"_s)
                return makeUnexpected("status must be loading or complete"_s);
            result.loading = status == "loading"_s;
        } else if (key == "windowType"_s) {
            auto type = value->asString();
            if (type == "normal"_s)
                result.windowType = OptionSet { WebExtensionWindow::TypeFilter::Normal };
            else if (type == "popup"_s)
                result.windowType = OptionSet { WebExtensionWindow::TypeFilter::Popup };
            else
                return makeUnexpected("unsupported window type"_s);
        } else if (key == "title"_s) {
            if (value->type() != JSON::Value::Type::String || value->asString().contains('\0'))
                return makeUnexpected("title must be a string without NUL"_s);
            result.titlePattern = value->asString();
        } else if (key == "url"_s) {
            Vector<String> patterns;
            if (value->type() == JSON::Value::Type::String)
                patterns.append(value->asString());
            else if (RefPtr array = value->asArray()) {
                for (auto& entry : *array) {
                    if (entry->type() != JSON::Value::Type::String)
                        return makeUnexpected("url must contain only match-pattern strings"_s);
                    patterns.append(entry->asString());
                }
            } else
                return makeUnexpected("url must be a match pattern or array of match patterns"_s);
            for (auto& pattern : patterns) {
                if (pattern.isEmpty() || pattern.contains('\0'))
                    return makeUnexpected("URL patterns must be nonempty and contain no NUL"_s);
            }
            result.urlPatterns = WTF::move(patterns);
        } else
            return makeUnexpected("unsupported tab query field"_s);
    }
    // The highlighted spelling has precedence independently of JSON key order.
    if (auto highlighted = object->getBoolean("highlighted"_s))
        result.selected = *highlighted;
    return result;
}
}
#endif
