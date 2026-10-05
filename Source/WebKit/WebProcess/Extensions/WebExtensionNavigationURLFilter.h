/*
 * Copyright (C) 2022 Apple Inc. All rights reserved.
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

#include <JavaScriptCore/RegularExpression.h>
#include <wtf/JSONValues.h>
#include <wtf/URL.h>
#include <wtf/Vector.h>

namespace WebKit {

// Parsed filters own their values; later changes to the input cannot affect a listener.
class WebExtensionNavigationURLFilter {
public:
    static std::expected<WebExtensionNavigationURLFilter, String> parse(const JSON::Value&);
    bool matches(const URL&) const;

private:
    enum class Kind : uint8_t {
        HostContains, HostEquals, HostPrefix, HostSuffix,
        PathContains, PathEquals, PathPrefix, PathSuffix,
        QueryContains, QueryEquals, QueryPrefix, QuerySuffix,
        URLContains, URLEquals, URLPrefix, URLSuffix,
        URLMatches, OriginAndPathMatches, Schemes, Ports,
    };
    struct Predicate {
        explicit Predicate(Kind value) : kind(value) { }
        Kind kind;
        String text;
        std::optional<JSC::Yarr::RegularExpression> expression;
        Vector<String> schemes;
        Vector<std::pair<uint16_t, uint16_t>> ports;
    };
    Vector<Vector<Predicate>> m_groups;
};

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
