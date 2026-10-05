/*
 * Copyright (C) 2026 KunanyiOS contributors.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "ContentExtensionURLConditions.h"

#if ENABLE(CONTENT_EXTENSIONS)

#include "ContentExtensionRule.h"
#include <wtf/CrossThreadCopier.h>
#include <wtf/text/MakeString.h>

namespace WebCore::ContentExtensions {

static_assert(!(URLConditionMask & (LoadTypeMask | ResourceTypeMask | LoadContextMask | ActionConditionMask | RequestMethodMask)));
static_assert((static_cast<uint64_t>(AllResourceFlags) << 32) == ActionFlagMask);
static_assert(!(ActionFlagMask & (uint64_t { 1 } << 63)));

static std::optional<URLConditionType> conditionType(const String& name)
{
    if (name == "if-request-url"_s)
        return URLConditionType::IfRequestURL;
    if (name == "unless-request-url"_s)
        return URLConditionType::UnlessRequestURL;
    if (name == "if-top-url"_s)
        return URLConditionType::IfTopURL;
    if (name == "unless-top-url"_s)
        return URLConditionType::UnlessTopURL;
    if (name == "if-frame-url"_s)
        return URLConditionType::IfFrameURL;
    if (name == "unless-frame-url"_s)
        return URLConditionType::UnlessFrameURL;
    return std::nullopt;
}

std::expected<URLConditionGroups, String> parseURLConditionGroups(JSON::Value& value)
{
    auto array = value.asArray();
    if (!array || !array->length() || array->length() > 6)
        return makeUnexpected("url-conditions must contain one to six condition groups."_s);
    URLConditionGroups result;
    ResourceFlags types = 0;
    for (auto& entry : *array) {
        auto object = entry->asObject();
        if (!object)
            return makeUnexpected("Each URL condition group must be an object."_s);
        for (auto& key : object->keys()) {
            if (key != "type"_s && key != "urls"_s && key != "case-sensitive"_s)
                return makeUnexpected(makeString("Unsupported URL condition property: "_s, key));
        }
        auto type = conditionType(object->getString("type"_s));
        if (!type)
            return makeUnexpected("Unknown URL condition type."_s);
        auto flag = static_cast<ResourceFlags>(*type);
        if (types & flag)
            return makeUnexpected("URL condition types must not be repeated."_s);
        types |= flag;
        URLConditionGroup group { *type, { }, false };
        if (auto sensitive = object->getValue("case-sensitive"_s)) {
            auto boolean = sensitive->asBoolean();
            if (!boolean)
                return makeUnexpected("URL condition case-sensitive must be a boolean."_s);
            group.caseSensitive = *boolean;
        }
        auto urls = object->getArray("urls"_s);
        if (!urls || !urls->length())
            return makeUnexpected("URL condition urls must be a non-empty array."_s);
        for (auto& url : *urls) {
            auto pattern = url->asString();
            if (pattern.isEmpty() || !StringView(pattern).containsOnlyASCII() || pattern.contains('\0'))
                return makeUnexpected("URL condition patterns must be non-empty ASCII strings without NUL."_s);
            group.urls.append(WTF::move(pattern));
        }
        result.append(WTF::move(group));
    }
    return result;
}

ResourceFlags urlConditionRequirements(const URLConditionGroups& groups)
{
    ResourceFlags flags = 0;
    for (auto& group : groups)
        flags |= static_cast<ResourceFlags>(group.type);
    return flags;
}

URLFilterParser::ParseStatus compileTriggerURLFilters(const Trigger& trigger, uint32_t actionLocation, URLFilterParser& parser, URLConditionActions& universalActions)
{
    // All alternatives identify the same serialized action, so overlapping
    // patterns cannot apply a header mutation or other action more than once.
    uint64_t actionAndFlags = (static_cast<uint64_t>(trigger.flags) << 32) | actionLocation;
    auto addPattern = [&](const String& pattern) {
        auto status = parser.addPattern(pattern, trigger.urlFilterIsCaseSensitive, actionAndFlags);
        if (status == URLFilterParser::MatchesEverything) {
            universalActions.add(actionAndFlags);
            return URLFilterParser::Ok;
        }
        return status;
    };
    auto status = addPattern(trigger.urlFilter);
    if (status != URLFilterParser::Ok)
        return status;
    for (auto& alternative : trigger.urlFilterAlternatives) {
        status = addPattern(alternative);
        if (status != URLFilterParser::Ok)
            return status;
    }
    return URLFilterParser::Ok;
}

bool matchesURLConditions(uint64_t actionAndFlags, const URLConditionActions& requestActions, const URLConditionActions& topActions, const URLConditionActions& frameActions)
{
    ResourceFlags required = (actionAndFlags >> 32) & URLConditionMask;
    if (!required)
        return false;
    uint32_t location = actionAndFlags;
    auto accepts = [&](URLConditionType type, const URLConditionActions& actions, bool excluded) {
        auto flag = static_cast<ResourceFlags>(type);
        if (!(required & flag))
            return true;
        bool found = actions.contains((static_cast<uint64_t>(flag) << 32) | location);
        return excluded ? !found : found;
    };
    return accepts(URLConditionType::IfRequestURL, requestActions, false)
        && accepts(URLConditionType::UnlessRequestURL, requestActions, true)
        && accepts(URLConditionType::IfTopURL, topActions, false)
        && accepts(URLConditionType::UnlessTopURL, topActions, true)
        && accepts(URLConditionType::IfFrameURL, frameActions, false)
        && accepts(URLConditionType::UnlessFrameURL, frameActions, true);
}

URLFilterParser::ParseStatus compileURLConditions(const URLConditionGroups& groups, uint32_t actionLocation,
    URLFilterParser& requestParser, URLConditionActions& requestUniversalActions,
    URLFilterParser& topParser, URLConditionActions& topUniversalActions,
    URLFilterParser& frameParser, URLConditionActions& frameUniversalActions)
{
    for (auto& group : groups) {
        URLFilterParser* parser = nullptr;
        URLConditionActions* universalActions = nullptr;
        switch (group.type) {
        case URLConditionType::IfRequestURL:
        case URLConditionType::UnlessRequestURL:
            parser = &requestParser;
            universalActions = &requestUniversalActions;
            break;
        case URLConditionType::IfTopURL:
        case URLConditionType::UnlessTopURL:
            parser = &topParser;
            universalActions = &topUniversalActions;
            break;
        case URLConditionType::IfFrameURL:
        case URLConditionType::UnlessFrameURL:
            parser = &frameParser;
            universalActions = &frameUniversalActions;
            break;
        }
        uint64_t tag = (static_cast<uint64_t>(group.type) << 32) | actionLocation;
        for (auto& url : group.urls) {
            auto status = parser->addPattern(url, group.caseSensitive, tag);
            if (status == URLFilterParser::MatchesEverything)
                universalActions->add(tag);
            else if (status != URLFilterParser::Ok)
                return status;
        }
    }
    return URLFilterParser::Ok;
}

void filterActionsByURLConditions(URLConditionActions& requestActions, const URLConditionActions& topActions, const URLConditionActions& frameActions)
{
    // Snapshot before removal: hash iteration order must not decide whether
    // an auxiliary tag is still present when its owning action is evaluated.
    URLConditionActions requestConditionActions;
    for (auto action : requestActions) {
        if (isAuxiliaryURLConditionAction(action))
            requestConditionActions.add(action);
    }
    requestActions.removeIf([&](uint64_t actionAndFlags) {
        if (isAuxiliaryURLConditionAction(actionAndFlags))
            return true;
        auto mode = static_cast<ActionCondition>((actionAndFlags >> 32) & ActionConditionMask);
        switch (mode) {
        case ActionCondition::None:
            return false;
        case ActionCondition::Conjunction:
            return !matchesURLConditions(actionAndFlags, requestConditionActions, topActions, frameActions);
        case ActionCondition::IfTopURL:
            return !topActions.contains(actionAndFlags);
        case ActionCondition::UnlessTopURL:
            return topActions.contains(actionAndFlags);
        case ActionCondition::IfFrameURL:
            return !frameActions.contains(actionAndFlags);
        case ActionCondition::UnlessFrameURL:
            return frameActions.contains(actionAndFlags);
        }
        ASSERT_NOT_REACHED();
        return true;
    });
}

URLConditionGroup URLConditionGroup::isolatedCopy() const &
{
    return { type, crossThreadCopy(urls), caseSensitive };
}

URLConditionGroup URLConditionGroup::isolatedCopy() &&
{
    return { type, crossThreadCopy(WTF::move(urls)), caseSensitive };
}

} // namespace WebCore::ContentExtensions

#endif
