/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionAPILocalization.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionString.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionLocalization.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/PropertyNameArray.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <wtf/Language.h>

namespace WebKit {

static std::expected<bool, String> localizationEscapeOption(JSContextRef context, JSValueRef options)
{
    if (!options || JSValueIsUndefined(context, options) || JSValueIsNull(context, options))
        return false;
    if (!JSValueIsObject(context, options) || JSValueIsArray(context, options) || isFunction(context, options))
        return makeUnexpected("options must be an object"_s);
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* object = toJS(JSValueToObject(context, options, nullptr));
    JSC::PropertyNameArrayBuilder names(vm, JSC::PropertyNameMode::Strings, JSC::PrivateSymbolMode::Exclude);
    object->methodTable()->getOwnPropertyNames(object, globalObject, names, JSC::DontEnumPropertiesMode::Include);
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, makeUnexpected("options properties could not be read"_s));
        return makeUnexpected("options properties could not be read"_s);
    }
    bool escape = false;
    for (auto& name : names) {
        if (name.string() != "escapeLt"_s)
            return makeUnexpected("options contains an unknown property"_s);
        auto value = object->get(globalObject, name);
        if (scope.exception()) {
            TRY_CLEAR_EXCEPTION(scope, makeUnexpected("options.escapeLt could not be read"_s));
            return makeUnexpected("options.escapeLt could not be read"_s);
        }
        if (value.isUndefinedOrNull())
            continue;
        if (!value.isBoolean())
            return makeUnexpected("options.escapeLt must be a boolean"_s);
        escape = value.asBoolean();
    }
    return escape;
}

JSValueRef WebExtensionAPILocalization::getMessage(JSContextRef context, const String& name, JSValueRef substitutions, JSValueRef options, String& exceptionString)
{
    auto escapeOption = localizationEscapeOption(context, options);
    if (!escapeOption) {
        exceptionString = escapeOption.error();
        return JSValueMakeUndefined(context);
    }
    RefPtr localization = extensionContext().localization();
    if (!localization)
        return toJSValueRef(context, emptyString());
    Vector<String> values;
    JSValueRef exception = nullptr;
    if (substitutions && JSValueIsArray(context, substitutions)) {
        auto array = JSValueToObject(context, substitutions, &exception);
        auto lengthValue = JSObjectGetProperty(context, array, toJSString("length"_s).get(), &exception);
        auto length = JSValueToNumber(context, lengthValue, &exception);
        if (exception || length > 9)
            return JSValueMakeUndefined(context);
        for (unsigned i = 0; i < length; ++i) {
            auto value = JSObjectGetPropertyAtIndex(context, array, i, &exception);
            if (exception)
                return JSValueMakeUndefined(context);
            auto string = adopt(JSValueToStringCopy(context, value, &exception));
            // Match Chromium's conversion of array elements: a failed ToString
            // is ignored, whereas a throwing index getter aborts the lookup.
            if (!exception && string)
                values.append(toString(string.get()));
            exception = nullptr;
        }
    } else if (substitutions && JSValueIsString(context, substitutions)) {
        auto string = adopt(JSValueToStringCopy(context, substitutions, &exception));
        if (exception || !string)
            return JSValueMakeUndefined(context);
        values.append(toString(string.get()));
    }
    return toJSValueRef(context, localization->localizedStringForKey(name, WTF::move(values), *escapeOption));
}

String WebExtensionAPILocalization::getUILanguage()
{
    return defaultLanguage(ShouldMinimizeLanguages::No);
}

void WebExtensionAPILocalization::getAcceptLanguages(Ref<WebExtensionCallbackHandler>&& callback)
{
    // Use the same list as CurlRequest's Accept-Language header.
    callback->call(fromArray(callback->globalContext(), userPreferredLanguages()));
}

void WebExtensionAPILocalization::getPreferredSystemLanguages(Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->call(fromArray(callback->globalContext(), userPreferredLanguages(ShouldMinimizeLanguages::No)));
}

void WebExtensionAPILocalization::getSystemUILanguage(Ref<WebExtensionCallbackHandler>&& callback)
{
    callback->call(toJSValueRef(callback->globalContext(), defaultLanguage(ShouldMinimizeLanguages::No)));
}

} // namespace WebKit
#endif
