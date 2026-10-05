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

#include "config.h"
#include "WebExtensionNavigationURLFilter.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include <wtf/text/MakeString.h>
#include <cmath>

namespace WebKit {

std::expected<WebExtensionNavigationURLFilter, String> WebExtensionNavigationURLFilter::parse(const JSON::Value& value)
{
    auto fail = [](const String& reason) -> std::expected<WebExtensionNavigationURLFilter, String> {
        return makeUnexpected(makeString("Invalid webNavigation URL filter: "_s, reason));
    };
    RefPtr object = value.asObject();
    RefPtr groups = object ? object->getArray("url"_s) : nullptr;
    if (!groups)
        return fail("'url' must be an array of objects."_s);

    static constexpr std::pair<ASCIILiteral, Kind> kinds[] {
        { "hostContains"_s, Kind::HostContains }, { "hostEquals"_s, Kind::HostEquals },
        { "hostPrefix"_s, Kind::HostPrefix }, { "hostSuffix"_s, Kind::HostSuffix },
        { "pathContains"_s, Kind::PathContains }, { "pathEquals"_s, Kind::PathEquals },
        { "pathPrefix"_s, Kind::PathPrefix }, { "pathSuffix"_s, Kind::PathSuffix },
        { "queryContains"_s, Kind::QueryContains }, { "queryEquals"_s, Kind::QueryEquals },
        { "queryPrefix"_s, Kind::QueryPrefix }, { "querySuffix"_s, Kind::QuerySuffix },
        { "urlContains"_s, Kind::URLContains }, { "urlEquals"_s, Kind::URLEquals },
        { "urlPrefix"_s, Kind::URLPrefix }, { "urlSuffix"_s, Kind::URLSuffix },
        { "urlMatches"_s, Kind::URLMatches }, { "originAndPathMatches"_s, Kind::OriginAndPathMatches },
        { "schemes"_s, Kind::Schemes }, { "ports"_s, Kind::Ports },
    };
    auto portNumber = [](const JSON::Value& value) -> std::optional<uint16_t> {
        auto number = value.asDouble();
        if (!number || !std::isfinite(*number) || *number < 0 || *number > 65535 || std::trunc(*number) != *number)
            return std::nullopt;
        return static_cast<uint16_t>(*number);
    };
    WebExtensionNavigationURLFilter filter;
    for (auto& groupValue : *groups) {
        RefPtr group = groupValue->asObject();
        if (!group)
            return fail("each 'url' entry must be an object."_s);
        Vector<Predicate> predicates;
        for (auto& key : group->keys()) {
            std::optional<Kind> kind;
            for (auto& entry : kinds) {
                if (key == entry.first) {
                    kind = entry.second;
                    break;
                }
            }
            if (!kind)
                return fail(makeString("unknown criterion '"_s, key, "'."_s));
            Predicate predicate { *kind };
            RefPtr raw = group->getValue(key);
            if (*kind == Kind::Schemes) {
                RefPtr array = raw->asArray();
                if (!array)
                    return fail("'schemes' must be an array of strings."_s);
                for (auto& item : *array) {
                    auto scheme = item->asString();
                    if (scheme.isNull())
                        return fail("'schemes' must be an array of strings."_s);
                    predicate.schemes.append(WTF::move(scheme));
                }
            } else if (*kind == Kind::Ports) {
                RefPtr array = raw->asArray();
                if (!array)
                    return fail("'ports' must be an array of ports or ranges."_s);
                for (auto& item : *array) {
                    if (auto port = portNumber(item.get())) {
                        predicate.ports.append({ *port, *port });
                        continue;
                    }
                    RefPtr range = item->asArray();
                    if (!range || range->length() != 2)
                        return fail("a port must be an integer from 0 to 65535; a range must contain two ports."_s);
                    auto first = portNumber(range->get(0).get());
                    auto last = portNumber(range->get(1).get());
                    if (!first || !last || *first >= *last)
                        return fail("a port range must contain two ascending ports from 0 to 65535."_s);
                    predicate.ports.append({ *first, *last });
                }
            } else {
                predicate.text = raw->asString();
                if (predicate.text.isNull())
                    return fail(makeString("'"_s, key, "' must be a string."_s));
                if (*kind == Kind::URLMatches || *kind == Kind::OriginAndPathMatches) {
                    predicate.expression.emplace(predicate.text);
                    if (!predicate.expression->isValid())
                        return fail(makeString("'"_s, key, "' contains an invalid regular expression."_s));
                }
            }
            predicates.append(WTF::move(predicate));
        }
        filter.m_groups.append(WTF::move(predicates));
    }
    return filter;
}

bool WebExtensionNavigationURLFilter::matches(const URL& url) const
{
    if (!url.isValid())
        return false;
    if (m_groups.isEmpty())
        return true;

    URL withoutFragment = url;
    withoutFragment.removeFragmentIdentifier();
    URL originAndPath = withoutFragment;
    originAndPath.removeQueryAndFragmentIdentifier();
    auto hostWithLeadingDot = makeString('.', url.host());
    auto port = url.port();
    if (!port)
        port = defaultPortForProtocol(url.protocol());

    auto matchesPredicate = [&](const Predicate& predicate) {
        StringView text = predicate.text;
        switch (predicate.kind) {
        case Kind::HostContains: return hostWithLeadingDot.contains(text);
        case Kind::HostEquals: return url.host() == text;
        case Kind::HostPrefix: return url.host().startsWith(text);
        case Kind::HostSuffix: return url.host().endsWith(text);
        case Kind::PathContains: return url.path().contains(text);
        case Kind::PathEquals: return url.path() == text;
        case Kind::PathPrefix: return url.path().startsWith(text);
        case Kind::PathSuffix: return url.path().endsWith(text);
        case Kind::QueryContains: return url.query().contains(text);
        case Kind::QueryEquals: return url.query() == text;
        case Kind::QueryPrefix: return url.query().startsWith(text);
        case Kind::QuerySuffix: return url.query().endsWith(text);
        case Kind::URLContains: return withoutFragment.string().contains(text);
        case Kind::URLEquals: return withoutFragment.string() == text;
        case Kind::URLPrefix: return withoutFragment.string().startsWith(text);
        case Kind::URLSuffix: return withoutFragment.string().endsWith(text);
        case Kind::URLMatches: return predicate.expression->match(withoutFragment.string()) >= 0;
        case Kind::OriginAndPathMatches: return predicate.expression->match(originAndPath.string()) >= 0;
        case Kind::Schemes: return predicate.schemes.containsIf([&](auto& scheme) { return url.protocol() == scheme; });
        case Kind::Ports: return port && predicate.ports.containsIf([&](auto& range) { return *port >= range.first && *port <= range.second; });
        }
        ASSERT_NOT_REACHED();
        return false;
    };
    for (auto& group : m_groups) {
        if (std::ranges::all_of(group, matchesPredicate))
            return true;
    }
    return false;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
