/*
 * Copyright (C) 2026 KunanyiOS contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS) && ENABLE(CONTENT_EXTENSIONS)

#include <wtf/JSONValues.h>
#include <wtf/Vector.h>
#include <wtf/text/WTFString.h>
#include <expected>

namespace WebKit {

struct WebExtensionDeclarativeNetRequestURLFilterLimits {
    size_t maximumInputLength { 64 * 1024 };
    size_t maximumPatternCount { 2048 };
    size_t maximumOutputLength { 1024 * 1024 };
};

// Validates and translates only the URL fields of a DNR condition. The caller
// must separately validate and implement its other conditions and action.
// Patterns form a union: every pattern must carry the same action/conditions.
// In particular, '^' can match a separator OR end-of-URL; WebCore's filter
// compiler cannot express that union inside a single regular expression.
struct WebExtensionDeclarativeNetRequestURLFilter {
    Vector<String> patterns;
    bool caseSensitive { false };
    bool regularExpression { false };

    static std::expected<WebExtensionDeclarativeNetRequestURLFilter, String> parse(const JSON::Object&, WebExtensionDeclarativeNetRequestURLFilterLimits = { });
};

} // namespace WebKit

#endif
