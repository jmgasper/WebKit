/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPIDeclarativeNetRequest.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"
#if ENABLE(CONTENT_EXTENSIONS)
#include "haiku/WebExtensionDeclarativeNetRequestURLFilter.h"
#endif
#include <cmath>
#include <wtf/text/MakeString.h>

// C++ port of Cocoa/WebExtensionAPIDeclarativeNetRequestCocoa.mm. Only argument
// types are checked here. Rule contents are validated by the UI process against
// the native content-rule translator, and an unsupported rule rejects the promise.

namespace WebKit {
// Documentation: https://developer.mozilla.org/docs/Mozilla/Add-ons/WebExtensions/API/declarativeNetRequest

static constexpr auto addRulesKey = "addRules"_s;
static constexpr auto removeRuleIdsKey = "removeRuleIds"_s;
static constexpr auto ruleIdsKey = "ruleIds"_s;
static constexpr auto enableRulesetIdsKey = "enableRulesetIds"_s;
static constexpr auto disableRulesetIdsKey = "disableRulesetIds"_s;
static constexpr auto regexKey = "regex"_s;
static constexpr auto isCaseSensitiveKey = "isCaseSensitive"_s;
static constexpr auto requireCapturingKey = "requireCapturing"_s;

static RefPtr<JSON::Value> member(const JSON::Object& object, const String& key)
{
    RefPtr value = object.getValue(key);
    return value && !value->isNull() ? value : nullptr;
}

static std::expected<Vector<String>, String> stringArray(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return Vector<String> { };
    RefPtr array = value->asArray();
    if (!array)
        return makeUnexpected(toErrorString(nullString(), key, "it must be an array of strings"_s));
    Vector<String> result;
    for (Ref item : *array) {
        auto string = item->asString();
        if (string.isNull())
            return makeUnexpected(toErrorString(nullString(), key, "it must be an array of strings"_s));
        result.append(WTF::move(string));
    }
    return result;
}

static std::expected<Vector<double>, String> ruleIdentifierArray(const JSON::Object& object, const String& key)
{
    RefPtr value = member(object, key);
    if (!value)
        return Vector<double> { };
    RefPtr array = value->asArray();
    if (!array)
        return makeUnexpected(toErrorString(nullString(), key, "it must be an array of integers"_s));
    Vector<double> result;
    for (Ref item : *array) {
        auto number = item->asDouble();
        if (!number || !std::isfinite(*number) || std::trunc(*number) != *number)
            return makeUnexpected(toErrorString(nullString(), key, "it must be an array of integers"_s));
        result.append(*number);
    }
    return result;
}

static void reply(WebExtensionCallbackHandler& callback, std::expected<void, WebExtensionError>&& result)
{
    if (!result) {
        callback.reportError(result.error());
        return;
    }
    callback.call();
}

void WebExtensionAPIDeclarativeNetRequest::updateEnabledRulesets(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    RefPtr object = options ? options->asObject() : nullptr;
    if (!object) {
        outExceptionString = toErrorString(nullString(), "options"_s, "it must be an object"_s);
        return;
    }
    auto enable = stringArray(*object, enableRulesetIdsKey);
    if (!enable) {
        outExceptionString = enable.error();
        return;
    }
    auto disable = stringArray(*object, disableRulesetIdsKey);
    if (!disable) {
        outExceptionString = disable.error();
        return;
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestUpdateEnabledRulesets(*enable, *disable), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        reply(callback, WTF::move(result));
    }, extensionContext().identifier());
}

void WebExtensionAPIDeclarativeNetRequest::getEnabledRulesets(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestGetEnabledRulesets(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](Vector<String>&& enabledRulesets) {
        callback->call(fromArray(callback->globalContext(), WTF::move(enabledRulesets)));
    }, extensionContext().identifier());
}

void WebExtensionAPIDeclarativeNetRequest::updateRules(bool session, RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    RefPtr object = options ? options->asObject() : nullptr;
    if (!object) {
        outExceptionString = toErrorString(nullString(), "options"_s, "it must be an object"_s);
        return;
    }

    String rulesToAddJSON;
    if (RefPtr value = member(*object, addRulesKey)) {
        RefPtr rules = value->asArray();
        if (!rules) {
            outExceptionString = toErrorString(nullString(), addRulesKey, "it must be an array of objects"_s);
            return;
        }
        for (Ref rule : *rules) {
            if (!rule->asObject()) {
                outExceptionString = toErrorString(nullString(), addRulesKey, "it must be an array of objects"_s);
                return;
            }
        }
        rulesToAddJSON = rules->toJSONString();
    }

    auto ruleIDsToRemove = ruleIdentifierArray(*object, removeRuleIdsKey);
    if (!ruleIDsToRemove) {
        outExceptionString = ruleIDsToRemove.error();
        return;
    }

    auto completion = [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        reply(callback, WTF::move(result));
    };
    if (session)
        WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestUpdateSessionRules(WTF::move(rulesToAddJSON), WTF::move(*ruleIDsToRemove)), WTF::move(completion), extensionContext().identifier());
    else
        WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestUpdateDynamicRules(WTF::move(rulesToAddJSON), WTF::move(*ruleIDsToRemove)), WTF::move(completion), extensionContext().identifier());
}

void WebExtensionAPIDeclarativeNetRequest::getRules(bool session, RefPtr<JSON::Value> filter, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    Vector<double> ruleIDs;
    if (filter && !filter->isNull()) {
        RefPtr object = filter->asObject();
        if (!object) {
            outExceptionString = toErrorString(nullString(), "filter"_s, "it must be an object"_s);
            return;
        }
        auto identifiers = ruleIdentifierArray(*object, ruleIdsKey);
        if (!identifiers) {
            outExceptionString = identifiers.error();
            return;
        }
        ruleIDs = WTF::move(*identifiers);
    }

    auto completion = [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<String, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        callback->call(fromJSON(callback->globalContext(), JSON::Value::parseJSON(result.value())));
    };
    if (session)
        WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestGetSessionRules(WTF::move(ruleIDs)), WTF::move(completion), extensionContext().identifier());
    else
        WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestGetDynamicRules(WTF::move(ruleIDs)), WTF::move(completion), extensionContext().identifier());
}

void WebExtensionAPIDeclarativeNetRequest::updateDynamicRules(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    updateRules(false, WTF::move(options), WTF::move(callback), outExceptionString);
}

void WebExtensionAPIDeclarativeNetRequest::getDynamicRules(RefPtr<JSON::Value> filter, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    getRules(false, WTF::move(filter), WTF::move(callback), outExceptionString);
}

void WebExtensionAPIDeclarativeNetRequest::updateSessionRules(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    updateRules(true, WTF::move(options), WTF::move(callback), outExceptionString);
}

void WebExtensionAPIDeclarativeNetRequest::getSessionRules(RefPtr<JSON::Value> filter, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    getRules(true, WTF::move(filter), WTF::move(callback), outExceptionString);
}

void WebExtensionAPIDeclarativeNetRequest::getMatchedRules(RefPtr<JSON::Value>, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    Ref extensionContext = this->extensionContext();
    if (!extensionContext->hasPermission("declarativeNetRequestFeedback"_s) && !extensionContext->hasPermission("activeTab"_s)) {
        outExceptionString = toErrorString(nullString(), nullString(), "either the 'declarativeNetRequestFeedback' or 'activeTab' permission is required"_s);
        return;
    }
    // The native content-rule path does not report which rule matched a load.
    callback->reportError("declarativeNetRequest.getMatchedRules() is not supported on this platform."_s);
}

void WebExtensionAPIDeclarativeNetRequest::isRegexSupported(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    RefPtr object = options ? options->asObject() : nullptr;
    if (!object) {
        outExceptionString = toErrorString(nullString(), "regexOptions"_s, "it must be an object"_s);
        return;
    }
    RefPtr regexValue = member(*object, regexKey);
    auto regex = regexValue ? regexValue->asString() : String();
    if (regex.isNull()) {
        outExceptionString = toErrorString(nullString(), regexKey, "it must be a string"_s);
        return;
    }
    bool caseSensitive = true;
    if (RefPtr value = member(*object, isCaseSensitiveKey)) {
        auto boolean = value->asBoolean();
        if (!boolean) {
            outExceptionString = toErrorString(nullString(), isCaseSensitiveKey, "it must be a boolean"_s);
            return;
        }
        caseSensitive = *boolean;
    }
    bool requireCapturing = false;
    if (RefPtr value = member(*object, requireCapturingKey)) {
        auto boolean = value->asBoolean();
        if (!boolean) {
            outExceptionString = toErrorString(nullString(), requireCapturingKey, "it must be a boolean"_s);
            return;
        }
        requireCapturing = *boolean;
    }

    // Answer with the parser the rule loader uses, so a supported expression is
    // exactly one that a regexFilter rule can be installed with. Capturing
    // groups exist only for redirect substitutions, which are not implemented.
    bool supported = false;
#if ENABLE(CONTENT_EXTENSIONS)
    if (!requireCapturing) {
        Ref condition = JSON::Object::create();
        condition->setString("regexFilter"_s, regex);
        condition->setBoolean("isUrlFilterCaseSensitive"_s, caseSensitive);
        supported = !!WebExtensionDeclarativeNetRequestURLFilter::parse(condition.get());
    }
#else
    UNUSED_VARIABLE(caseSensitive);
    UNUSED_VARIABLE(requireCapturing);
#endif

    Ref result = JSON::Object::create();
    result->setBoolean("isSupported"_s, supported);
    if (!supported)
        result->setString("reason"_s, "syntaxError"_s);
    callback->call(fromJSON(callback->globalContext(), WTF::move(result)));
}

void WebExtensionAPIDeclarativeNetRequest::setExtensionActionOptions(RefPtr<JSON::Value> options, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    if (!options || !options->asObject()) {
        outExceptionString = toErrorString(nullString(), "extensionActionOptions"_s, "it must be an object"_s);
        return;
    }
    // Blocked-load counts are not reported by the native content-rule path, so
    // there is no badge count to show. Accept the options as Safari does for
    // what it cannot display: uBlock Origin Lite calls this at startup and a
    // rejection there surfaced as an unhandled error.
    callback->call();
}

void WebExtensionAPIDeclarativeNetRequest::getAvailableStaticRuleCount(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::DeclarativeNetRequestGetAvailableStaticRuleCount(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](double count) {
        callback->call(JSValueMakeNumber(callback->globalContext(), count));
    }, extensionContext().identifier());
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
