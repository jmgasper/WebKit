/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionDeclarativeNetRequestRulesHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && ENABLE(CONTENT_EXTENSIONS)
#include "WebExtensionDeclarativeNetRequestURLFilter.h"
#include <algorithm>
#include <cmath>
#include <wtf/text/MakeString.h>
#include <wtf/text/StringBuilder.h>

namespace WebKit {
namespace {
using Error = std::expected<void, String>;
Error checkKeys(const JSON::Object& object, std::initializer_list<ASCIILiteral> allowed, ASCIILiteral where)
{
    for (auto& entry : object) {
        if (std::ranges::find(allowed, entry.key) == allowed.end())
            return makeUnexpected(makeString("Unsupported "_s, where, " property: "_s, entry.key));
    }
    return { };
}
std::optional<double> positiveInteger(JSON::Value* value)
{
    if (!value)
        return std::nullopt;
    auto number = value->asDouble();
    if (!number || !std::isfinite(*number) || std::floor(*number) != *number || *number < 1 || *number > 2147483647)
        return std::nullopt;
    return number;
}
struct Resource { ASCIILiteral input; ASCIILiteral output; };
constexpr Resource resources[] {
    { "main_frame"_s, "top-document"_s }, { "sub_frame"_s, "child-document"_s },
    { "stylesheet"_s, "style-sheet"_s }, { "script"_s, "script"_s },
    { "image"_s, "image"_s }, { "font"_s, "font"_s }, { "media"_s, "media"_s },
    { "xmlhttprequest"_s, "fetch"_s }, { "websocket"_s, "websocket"_s },
    { "ping"_s, "ping"_s }, { "csp_report"_s, "csp-report"_s },
    // WebCore's "other" also covers some loads Chrome types more precisely;
    // Safari's translator makes the same approximation.
    { "other"_s, "other"_s },
};
// Chrome types WebCore cannot distinguish. A rule naming only these never matches.
constexpr ASCIILiteral unmatchableResourceTypes[] { "object"_s, "webtransport"_s, "webbundle"_s };

std::expected<Ref<JSON::Array>, String> resourceTypes(const JSON::Object& condition)
{
    auto included = condition.getValue("resourceTypes"_s);
    auto excluded = condition.getValue("excludedResourceTypes"_s);
    if (included && excluded)
        return makeUnexpected("resourceTypes and excludedResourceTypes cannot be combined."_s);
    Ref output = JSON::Array::create();
    if (!included && !excluded) {
        // Chrome: every type except main_frame.
        for (auto& resource : resources) {
            if (resource.input != "main_frame"_s)
                output->pushString(resource.output);
        }
        output->pushString("svg-document"_s);
        output->pushString("popup"_s);
        return output;
    }
    auto array = (included ? included : excluded)->asArray();
    if (!array || (included && !array->length()))
        return makeUnexpected("resourceTypes must be a non-empty array."_s);
    HashSet<String> names;
    for (auto& value : *array) {
        auto name = value->asString();
        bool known = std::ranges::find(resources, name, &Resource::input) != std::end(resources)
            || std::ranges::find(unmatchableResourceTypes, name) != std::end(unmatchableResourceTypes);
        if (!known)
            return makeUnexpected(makeString("Unsupported native resource type: "_s, name));
        if (!names.add(name).isNewEntry)
            return makeUnexpected("resourceTypes must not contain duplicates."_s);
    }
    for (auto& resource : resources) {
        if (names.contains(String { resource.input }) == !!included)
            output->pushString(resource.output);
    }
    if (!output->length())
        return makeUnexpected("The rule only names resource types this platform cannot distinguish."_s);
    return output;
}

// "example.com" matches example.com and its subdomains, as a URL condition regex.
std::expected<Ref<JSON::Array>, String> domainPatterns(const JSON::Object& condition, ASCIILiteral key)
{
    auto array = condition.getArray(key);
    if (!array || !array->length())
        return makeUnexpected(makeString(key, " must be a non-empty array of domains."_s));
    Ref output = JSON::Array::create();
    for (auto& value : *array) {
        auto domain = value->asString();
        if (domain.isEmpty() || !domain.containsOnlyASCII() || domain.length() > 253)
            return makeUnexpected(makeString(key, " must contain ASCII domain names."_s));
        StringBuilder pattern;
        pattern.append("^[^:]+://+([^:/]+\\.)?"_s);
        for (auto character : StringView { domain }.codeUnits()) {
            if (!isASCIIAlphanumeric(character) && character != '.' && character != '-' && character != '_')
                return makeUnexpected(makeString(key, " contains an invalid domain."_s));
            if (character == '.')
                pattern.append('\\');
            pattern.append(toASCIILower(character));
        }
        pattern.append("[:/]"_s);
        output->pushString(pattern.toString());
    }
    return output;
}

enum class Kind : uint8_t { Allow, AllowAllRequests, Block, UpgradeScheme, Redirect, ModifyHeaders };
// Chrome: at equal priority allow/allowAllRequests > block > upgradeScheme > redirect;
// modifyHeaders is not terminal and applies unless a higher-ranked rule stops matching.
uint8_t rank(Kind kind)
{
    switch (kind) {
    case Kind::Allow:
    case Kind::AllowAllRequests:
        return 0;
    case Kind::Block:
        return 1;
    case Kind::UpgradeScheme:
        return 2;
    case Kind::Redirect:
        return 3;
    case Kind::ModifyHeaders:
        return 4;
    }
    return 4;
}

struct Rule {
    double identifier;
    double priority;
    Kind kind;
    size_t declaration;
    Vector<Ref<JSON::Object>> triggers; // a union: every trigger carries the same action
    RefPtr<JSON::Object> action; // WebKit action for redirect / modify-headers
};

std::expected<Ref<JSON::Object>, String> redirectAction(const JSON::Object& input)
{
    // Chrome's redirect dictionary, renamed to WebCore's content-rule keys
    // (see Cocoa/_WKWebExtensionDeclarativeNetRequestRule.mm).
    auto redirect = input.getObject("redirect"_s);
    if (!redirect)
        return makeUnexpected("A redirect action needs a redirect object."_s);
    if (auto valid = checkKeys(*redirect, { "url"_s, "extensionPath"_s, "regexSubstitution"_s, "transform"_s }, "redirect"_s); !valid)
        return makeUnexpected(valid.error());
    Ref output = JSON::Object::create();
    if (auto url = redirect->getString("url"_s); !url.isNull())
        output->setString("url"_s, url);
    if (auto path = redirect->getString("extensionPath"_s); !path.isNull())
        output->setString("extension-path"_s, path);
    if (auto substitution = redirect->getString("regexSubstitution"_s); !substitution.isNull())
        output->setString("regex-substitution"_s, substitution);
    if (auto transform = redirect->getObject("transform"_s)) {
        Ref converted = JSON::Object::create();
        for (auto& entry : *transform) {
            if (entry.key != "queryTransform"_s) {
                converted->setValue(entry.key, entry.value.copyRef());
                continue;
            }
            auto query = entry.value->asObject();
            if (!query)
                return makeUnexpected("redirect.transform.queryTransform must be an object."_s);
            Ref convertedQuery = JSON::Object::create();
            if (auto remove = query->getArray("removeParams"_s))
                convertedQuery->setArray("remove-parameters"_s, remove.releaseNonNull());
            if (auto add = query->getArray("addOrReplaceParams"_s)) {
                Ref parameters = JSON::Array::create();
                for (auto& item : *add) {
                    auto parameter = item->asObject();
                    if (!parameter)
                        return makeUnexpected("addOrReplaceParams must contain objects."_s);
                    Ref copy = JSON::Object::create();
                    for (auto& field : *parameter)
                        copy->setValue(field.key == "replaceOnly"_s ? String { "replace-only"_s } : field.key, field.value.copyRef());
                    parameters->pushObject(WTF::move(copy));
                }
                convertedQuery->setArray("add-or-replace-parameters"_s, WTF::move(parameters));
            }
            converted->setObject("query-transform"_s, WTF::move(convertedQuery));
        }
        output->setObject("transform"_s, WTF::move(converted));
    }
    if (!output->size())
        return makeUnexpected("A redirect needs url, extensionPath, regexSubstitution or transform."_s);
    Ref action = JSON::Object::create();
    action->setString("type"_s, "redirect"_s);
    action->setObject("redirect"_s, WTF::move(output));
    return action;
}

std::expected<Rule, String> parseRule(const JSON::Object& input, size_t declaration)
{
    // Unknown rule-level keys (AdGuard ships "metadata") are ignored, as in Chrome.
    auto identifier = positiveInteger(input.getValue("id"_s).get());
    auto priorityValue = input.getValue("priority"_s);
    auto priority = priorityValue ? positiveInteger(priorityValue.get()) : std::optional<double>(1);
    if (!identifier || !priority)
        return makeUnexpected("Rule id and priority must be positive 32-bit integers."_s);
    auto action = input.getObject("action"_s);
    auto condition = input.getObject("condition"_s);
    if (!action || !condition)
        return makeUnexpected("Rule action and condition must be objects."_s);
    auto type = action->getString("type"_s);
    Kind kind;
    RefPtr<JSON::Object> webKitAction;
    if (type == "allow"_s)
        kind = Kind::Allow;
    else if (type == "allowAllRequests"_s)
        kind = Kind::AllowAllRequests;
    else if (type == "block"_s)
        kind = Kind::Block;
    else if (type == "upgradeScheme"_s)
        kind = Kind::UpgradeScheme;
    else if (type == "redirect"_s) {
        kind = Kind::Redirect;
        auto redirect = redirectAction(*action);
        if (!redirect)
            return makeUnexpected(redirect.error());
        webKitAction = WTF::move(*redirect);
    } else if (type == "modifyHeaders"_s) {
        kind = Kind::ModifyHeaders;
        Ref headers = JSON::Object::create();
        headers->setString("type"_s, "modify-headers"_s);
        // Same {header, operation, value} entries as Chrome.
        if (auto request = action->getArray("requestHeaders"_s))
            headers->setArray("request-headers"_s, request.releaseNonNull());
        if (auto response = action->getArray("responseHeaders"_s))
            headers->setArray("response-headers"_s, response.releaseNonNull());
        if (headers->size() == 1)
            return makeUnexpected("modifyHeaders needs requestHeaders or responseHeaders."_s);
        headers->setInteger("priority"_s, static_cast<int>(*priority));
        webKitAction = WTF::move(headers);
    } else
        return makeUnexpected(makeString("Native DNR action is not implemented: "_s, type));

    if (auto valid = checkKeys(*condition, { "urlFilter"_s, "regexFilter"_s, "isUrlFilterCaseSensitive"_s, "resourceTypes"_s, "excludedResourceTypes"_s,
        "domainType"_s, "initiatorDomains"_s, "excludedInitiatorDomains"_s, "domains"_s, "excludedDomains"_s,
        "requestDomains"_s, "excludedRequestDomains"_s, "requestMethods"_s }, "condition"_s); !valid)
        return makeUnexpected(valid.error());
    // Summit has no per-tab rules and no response-header matching.
    auto filter = WebExtensionDeclarativeNetRequestURLFilter::parse(*condition);
    if (!filter)
        return makeUnexpected(filter.error());
    auto types = resourceTypes(*condition);
    if (!types)
        return makeUnexpected(types.error());

    Ref trigger = JSON::Object::create();
    trigger->setString("url-filter"_s, filter->patterns[0]);
    if (filter->patterns.size() > 1) {
        Ref alternatives = JSON::Array::create();
        for (size_t index = 1; index < filter->patterns.size(); ++index)
            alternatives->pushString(filter->patterns[index]);
        trigger->setArray("url-filter-alternatives"_s, WTF::move(alternatives));
    }
    trigger->setBoolean("url-filter-is-case-sensitive"_s, filter->caseSensitive);
    trigger->setArray("resource-type"_s, WTF::move(*types));

    if (auto domainType = condition->getString("domainType"_s); !domainType.isNull()) {
        Ref loadType = JSON::Array::create();
        if (domainType == "firstParty"_s)
            loadType->pushString("first-party"_s);
        else if (domainType == "thirdParty"_s)
            loadType->pushString("third-party"_s);
        else
            return makeUnexpected("domainType must be firstParty or thirdParty."_s);
        trigger->setArray("load-type"_s, WTF::move(loadType));
    }

    // Request and initiator domains become WebCore URL-condition groups: all
    // groups must hold, and an exclusion never suppresses unrelated rules.
    Ref groups = JSON::Array::create();
    auto addGroup = [&](ASCIILiteral key, ASCIILiteral groupType) -> std::expected<void, String> {
        if (!condition->getValue(key))
            return { };
        auto patterns = domainPatterns(*condition, key);
        if (!patterns)
            return makeUnexpected(patterns.error());
        Ref group = JSON::Object::create();
        group->setString("type"_s, groupType);
        group->setArray("urls"_s, WTF::move(*patterns));
        groups->pushObject(WTF::move(group));
        return { };
    };
    if (condition->getValue("initiatorDomains"_s) && condition->getValue("domains"_s))
        return makeUnexpected("domains and initiatorDomains cannot be combined."_s);
    if (condition->getValue("excludedInitiatorDomains"_s) && condition->getValue("excludedDomains"_s))
        return makeUnexpected("excludedDomains and excludedInitiatorDomains cannot be combined."_s);
    // `domains`/`excludedDomains` are Chrome's deprecated names for the initiator keys.
    for (auto [key, groupType] : { std::pair { "requestDomains"_s, "if-request-url"_s }, std::pair { "excludedRequestDomains"_s, "unless-request-url"_s },
        std::pair { "initiatorDomains"_s, "if-frame-url"_s }, std::pair { "domains"_s, "if-frame-url"_s },
        std::pair { "excludedInitiatorDomains"_s, "unless-frame-url"_s }, std::pair { "excludedDomains"_s, "unless-frame-url"_s } }) {
        if (auto added = addGroup(key, groupType); !added)
            return makeUnexpected(added.error());
    }
    if (groups->length())
        trigger->setArray("url-conditions"_s, WTF::move(groups));

    Vector<Ref<JSON::Object>> triggers;
    if (kind == Kind::AllowAllRequests) {
        // Whole documents: stop this extension's later rules for every load
        // whose top (main_frame) or frame (sub_frame) URL matches the filter.
        auto names = condition->getArray("resourceTypes"_s);
        if (!names || !names->length())
            return makeUnexpected("allowAllRequests needs resourceTypes main_frame and/or sub_frame."_s);
        for (auto& value : *names) {
            auto name = value->asString();
            if (name != "main_frame"_s && name != "sub_frame"_s)
                return makeUnexpected("allowAllRequests only applies to main_frame and sub_frame."_s);
            Ref documentTrigger = JSON::Object::create();
            documentTrigger->setString("url-filter"_s, ".*"_s);
            Ref urls = JSON::Array::create();
            for (auto& pattern : filter->patterns)
                urls->pushString(pattern);
            Ref group = JSON::Object::create();
            group->setString("type"_s, name == "main_frame"_s ? "if-top-url"_s : "if-frame-url"_s);
            group->setArray("urls"_s, WTF::move(urls));
            group->setBoolean("case-sensitive"_s, filter->caseSensitive);
            Ref documentGroups = JSON::Array::create();
            documentGroups->pushObject(WTF::move(group));
            documentTrigger->setArray("url-conditions"_s, WTF::move(documentGroups));
            if (name == "sub_frame"_s) {
                Ref context = JSON::Array::create();
                context->pushString("child-frame"_s);
                documentTrigger->setArray("load-context"_s, WTF::move(context));
            }
            triggers.append(WTF::move(documentTrigger));
        }
    } else if (auto methods = condition->getArray("requestMethods"_s)) {
        // WebCore matches one method per trigger.
        if (!methods->length())
            return makeUnexpected("requestMethods must be a non-empty array."_s);
        for (auto& value : *methods) {
            auto method = value->asString();
            if (method.isEmpty())
                return makeUnexpected("requestMethods must contain method names."_s);
            Ref copy = JSON::Object::create();
            for (auto& entry : trigger.get())
                copy->setValue(entry.key, entry.value.copyRef());
            copy->setString("request-method"_s, method.convertToASCIILowercase());
            triggers.append(WTF::move(copy));
        }
    } else
        triggers.append(WTF::move(trigger));

    return Rule { *identifier, *priority, kind, declaration, WTF::move(triggers), WTF::move(webKitAction) };
}
} // namespace

std::expected<WebExtensionDeclarativeNetRequestTranslationHaiku, String> translateDeclarativeNetRequestRulesHaiku(
    const Vector<WebExtensionDeclarativeNetRequestRulesetHaiku>& rulesets, WebExtensionDeclarativeNetRequestLimitsHaiku limits)
{
    WebExtensionDeclarativeNetRequestTranslationHaiku result;
    Vector<Rule> rules;
    for (auto& ruleset : rulesets) {
        if (ruleset.identifier.isEmpty() || ruleset.identifier.contains('\0') || result.identifiers.contains(ruleset.identifier))
            return makeUnexpected("Ruleset identifiers must be non-empty and unique."_s);
        HashSet<double> identifiers;
        for (auto& value : ruleset.rules.get()) {
            if (rules.size() >= limits.maximumRules)
                return makeUnexpected("Native DNR rule count limit exceeded."_s);
            auto object = value->asObject();
            if (!object)
                return makeUnexpected(makeString("Ruleset "_s, ruleset.identifier, " contains a non-object rule."_s));
            auto parsed = parseRule(*object, rules.size());
            if (!parsed) {
                // Chrome and Safari load a static ruleset without the rules they
                // cannot represent. Rejecting the whole translation left uBlock
                // Origin Lite, AdGuard and Ghostery with no network blocking.
                if (ruleset.skipUnsupportedRules) {
                    ++result.skippedRuleCount;
                    continue;
                }
                return makeUnexpected(makeString("Ruleset "_s, ruleset.identifier, ": "_s, parsed.error()));
            }
            if (!identifiers.add(parsed->identifier).isNewEntry)
                return makeUnexpected(makeString("Duplicate rule id in ruleset "_s, ruleset.identifier));
            rules.append(WTF::move(*parsed));
        }
        result.identifiers.add(ruleset.identifier, WTF::move(identifiers));
    }
    std::ranges::sort(rules, [](const auto& a, const auto& b) {
        if (a.priority != b.priority)
            return a.priority > b.priority;
        if (rank(a.kind) != rank(b.kind))
            return rank(a.kind) < rank(b.kind);
        return a.declaration < b.declaration;
    });
    Ref output = JSON::Array::create();
    size_t outputLength = 2;
    auto append = [&](const JSON::Object& trigger, Ref<JSON::Object>&& action) -> bool {
        Ref item = JSON::Object::create();
        Ref triggerCopy = JSON::Object::create();
        for (auto& entry : trigger)
            triggerCopy->setValue(entry.key, entry.value.copyRef());
        item->setObject("trigger"_s, WTF::move(triggerCopy));
        item->setObject("action"_s, WTF::move(action));
        auto length = item->toJSONString().length();
        if (outputLength > limits.maximumOutputLength || length + 1 > limits.maximumOutputLength - outputLength)
            return false;
        outputLength += length + 1;
        output->pushObject(WTF::move(item));
        return true;
    };
    auto simpleAction = [](ASCIILiteral type) {
        Ref action = JSON::Object::create();
        action->setString("type"_s, type);
        return action;
    };
    auto copyAction = [](const JSON::Object& action) {
        Ref copy = JSON::Object::create();
        for (auto& entry : action)
            copy->setValue(entry.key, entry.value.copyRef());
        return copy;
    };
    for (auto& rule : rules) {
        for (auto& trigger : rule.triggers) {
            bool appended = true;
            switch (rule.kind) {
            case Kind::Block:
                appended = append(trigger, simpleAction("block"_s));
                break;
            case Kind::UpgradeScheme:
                appended = append(trigger, simpleAction("make-https"_s));
                break;
            case Kind::Redirect:
            case Kind::ModifyHeaders:
                appended = append(trigger, copyAction(*rule.action));
                break;
            case Kind::Allow:
            case Kind::AllowAllRequests:
                break;
            }
            if (!appended)
                return makeUnexpected("Native DNR translation size limit exceeded."_s);
            // The first matching terminal rule wins within this extension. The
            // stop action carries exactly the same conditions as its owning rule.
            // modifyHeaders is not terminal: lower-priority block rules still apply.
            // Neither is a redirect here: WebCore applies it only where the
            // extension has host access and then ignores later blocks itself.
            // Stopping after a redirect it refused let the request through with
            // no rule at all (uBlock Origin Lite's surrogate redirects rank above
            // its blocks, so ad scripts loaded). Chrome skips such a rule and the
            // next one applies.
            if (rule.kind != Kind::ModifyHeaders && rule.kind != Kind::Redirect
                && !append(trigger, simpleAction("ignore-following-rules"_s)))
                return makeUnexpected("Native DNR translation size limit exceeded."_s);
        }
    }
    result.ruleCount = rules.size();
    result.json = output->toJSONString();
    if (result.json.length() > limits.maximumOutputLength)
        return makeUnexpected("Native DNR translation size limit exceeded."_s);
    return result;
}
} // namespace WebKit
#endif
