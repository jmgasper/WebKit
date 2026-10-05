/*
 * Copyright (C) 2026 KunanyiOS contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(CONTENT_EXTENSIONS)

#include <WebCore/ResourceLoadInfo.h>
#include <WebCore/URLFilterParser.h>
#include <wtf/HashSet.h>
#include <wtf/Hasher.h>
#include <wtf/JSONValues.h>
#include <wtf/Vector.h>
#include <expected>

namespace WebCore::ContentExtensions {

struct Trigger;

enum class URLConditionType : uint32_t {
    IfRequestURL = 0x02000000,
    UnlessRequestURL = 0x04000000,
    IfTopURL = 0x08000000,
    UnlessTopURL = 0x10000000,
    IfFrameURL = 0x20000000,
    UnlessFrameURL = 0x40000000,
};

struct URLConditionGroup {
    URLConditionType type;
    Vector<String> urls;
    bool caseSensitive { false };

    URLConditionGroup isolatedCopy() const &;
    URLConditionGroup isolatedCopy() &&;
    friend bool operator==(const URLConditionGroup&, const URLConditionGroup&) = default;
};

inline void add(Hasher& hasher, const URLConditionGroup& group)
{
    add(hasher, group.type, group.urls, group.caseSensitive);
}

using URLConditionGroups = Vector<URLConditionGroup>;
using URLConditionActions = HashSet<uint64_t, DefaultHash<uint64_t>, WTF::UnsignedWithZeroKeyHashTraits<uint64_t>>;

WEBCORE_EXPORT std::expected<URLConditionGroups, String> parseURLConditionGroups(JSON::Value&);
WEBCORE_EXPORT ResourceFlags urlConditionRequirements(const URLConditionGroups&);
WEBCORE_EXPORT URLFilterParser::ParseStatus compileTriggerURLFilters(const Trigger&, uint32_t actionLocation, URLFilterParser&, URLConditionActions& universalActions);
WEBCORE_EXPORT bool matchesURLConditions(uint64_t actionAndFlags, const URLConditionActions& requestActions, const URLConditionActions& topActions, const URLConditionActions& frameActions);
WEBCORE_EXPORT void filterActionsByURLConditions(URLConditionActions& requestActions, const URLConditionActions& topActions, const URLConditionActions& frameActions);

constexpr bool isAuxiliaryURLConditionAction(uint64_t actionAndFlags)
{
    auto flags = static_cast<ResourceFlags>(actionAndFlags >> 32);
    return (flags & URLConditionMask) && !(flags & ActionConditionMask);
}

// Each condition gets its own tag at the original action location. Inclusion
// patterns form a union inside a group; separate groups form a conjunction.
// Request-condition tags share the request bytecode stream and must be removed
// from candidate actions before deserializing any action bytes.
WEBCORE_EXPORT URLFilterParser::ParseStatus compileURLConditions(const URLConditionGroups&, uint32_t actionLocation,
    URLFilterParser& requestParser, URLConditionActions& requestUniversalActions,
    URLFilterParser& topParser, URLConditionActions& topUniversalActions,
    URLFilterParser& frameParser, URLConditionActions& frameUniversalActions);

} // namespace WebCore::ContentExtensions

#endif
