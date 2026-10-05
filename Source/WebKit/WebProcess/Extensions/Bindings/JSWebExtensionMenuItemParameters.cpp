/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
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
#include "JSWebExtensionMenuItemParameters.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "JSWebExtensionString.h"
#include "Protected.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/ArrayConstructor.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/PropertyNameArray.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
namespace WebKit {
static std::expected<bool, String> menuIsArray(JSContextRef context, JSValueRef value)
{
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    bool result = JSC::isArray(globalObject, toJS(globalObject, value));
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, std::unexpected("menu property type could not be read"_s));
        return std::unexpected("menu property type could not be read"_s);
    }
    return result;
}

static std::expected<Vector<String>, String> menuPropertyNames(JSContextRef context, JSObjectRef object)
{
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    JSC::PropertyNameArrayBuilder names(vm, JSC::PropertyNameMode::Strings, JSC::PrivateSymbolMode::Exclude);
    // Keep dictionary enumeration semantics, including enumerable inherited
    // properties, while propagating a proxy or prototype enumeration failure.
    toJS(object)->getPropertyNames(globalObject, names, JSC::DontEnumPropertiesMode::Exclude);
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, std::unexpected("menu properties could not be read"_s));
        return std::unexpected("menu properties could not be read"_s);
    }
    Vector<String> result;
    for (auto& name : names)
        result.append(name.string());
    return result;
}

static bool isMenuProperty(const String& key)
{
    static constexpr std::array keys {
        "checked"_s, "command"_s, "contexts"_s, "documentUrlPatterns"_s, "enabled"_s, "icons"_s,
        "id"_s, "parentId"_s, "targetUrlPatterns"_s, "title"_s, "type"_s, "visible"_s,
    };
    return std::ranges::find(keys, key) != keys.end();
}

static RefPtr<JSON::Value> menuPropertyValue(JSContextRef context, const String& key, JSValueRef value)
{
    if (key == "title"_s || key == "command"_s || key == "type"_s || key == "id"_s || key == "parentId"_s) {
        if (JSValueIsString(context, value)) {
            JSRetainPtr string(Adopt, JSValueToStringCopy(context, value, nullptr));
            return JSON::Value::create(toString(string.get()));
        }
        if ((key == "id"_s || key == "parentId"_s) && JSValueIsNumber(context, value)) {
            double number = JSValueToNumber(context, value, nullptr);
            if (std::isfinite(number))
                return JSON::Value::create(number);
        }
    } else if (key == "checked"_s || key == "enabled"_s || key == "visible"_s) {
        if (JSValueIsBoolean(context, value))
            return JSON::Value::create(JSValueToBoolean(context, value));
    } else if (key == "contexts"_s || key == "documentUrlPatterns"_s || key == "targetUrlPatterns"_s) {
        auto isArray = menuIsArray(context, value);
        if (!isArray || !*isArray)
            return nullptr;
        auto object = JSValueToObject(context, value, nullptr);
        JSValueRef exception = nullptr;
        auto lengthValue = JSObjectGetProperty(context, object, toJSString("length"_s).get(), &exception);
        if (exception || !JSValueIsNumber(context, lengthValue))
            return nullptr;
        double length = JSValueToNumber(context, lengthValue, nullptr);
        if (!std::isfinite(length) || length < 0 || std::trunc(length) != length || length > std::numeric_limits<uint32_t>::max())
            return nullptr;
        Ref result = JSON::Array::create();
        for (uint64_t index = 0; index < static_cast<uint64_t>(length); ++index) {
            auto entry = JSObjectGetPropertyAtIndex(context, object, static_cast<uint32_t>(index), &exception);
            if (exception || !JSValueIsString(context, entry))
                return nullptr;
            JSRetainPtr string(Adopt, JSValueToStringCopy(context, entry, nullptr));
            result->pushString(toString(string.get()));
        }
        return result;
    } else if (key == "icons"_s) {
        if (JSValueIsNull(context, value))
            return JSON::Value::null();
        if (JSValueIsString(context, value)) {
            JSRetainPtr string(Adopt, JSValueToStringCopy(context, value, nullptr));
            return JSON::Value::create(toString(string.get()));
        }
        auto isArray = menuIsArray(context, value);
        if (!JSValueIsObject(context, value) || !isArray || *isArray)
            return nullptr;
        auto object = JSValueToObject(context, value, nullptr);
        if (JSObjectIsFunction(context, object))
            return nullptr;
        auto names = menuPropertyNames(context, object);
        if (!names)
            return nullptr;
        Ref result = JSON::Object::create();
        for (auto& name : *names) {
            JSValueRef exception = nullptr;
            auto path = JSObjectGetProperty(context, object, toJSString(name).get(), &exception);
            if (exception || !JSValueIsString(context, path))
                return nullptr;
            JSRetainPtr string(Adopt, JSValueToStringCopy(context, path, nullptr));
            result->setString(name, toString(string.get()));
        }
        return result;
    }
    return nullptr;
}

std::expected<WebExtensionMenuItemInput, String> parseWebExtensionMenuItemInput(JSContextRef context, JSValueRef value, WebExtensionMenuItemOperation operation, bool manifestVersion3, const URL& baseURL)
{
    if (!value || !JSValueIsObject(context, value))
        return std::unexpected("menu properties must be a dictionary"_s);
    auto isArray = menuIsArray(context, value);
    if (!isArray || *isArray)
        return std::unexpected("menu properties must be a dictionary"_s);
    auto object = JSValueToObject(context, value, nullptr);
    if (JSObjectIsFunction(context, object))
        return std::unexpected("menu properties must be a dictionary"_s);
    Protected<JSObjectRef> protectedObject(JSContextGetGlobalContext(context), object);
    WebExtensionMenuItemInput result;
    Ref properties = JSON::Object::create();
    auto names = menuPropertyNames(context, object);
    if (!names)
        return std::unexpected(names.error());
    for (auto& key : *names) {
        JSValueRef exception = nullptr;
        auto field = JSObjectGetProperty(context, object, toJSString(key).get(), &exception);
        if (exception)
            return std::unexpected("a menu property getter threw an exception"_s);
        if (JSValueIsUndefined(context, field))
            continue;
        Protected<JSValueRef> protectedField(JSContextGetGlobalContext(context), field);
        if (key == "onclick"_s) {
            result.clickHandlerSpecified = true;
            if (JSValueIsNull(context, field))
                continue;
            if (!JSValueIsObject(context, field) || !JSObjectIsFunction(context, JSValueToObject(context, field, nullptr)))
                return std::unexpected("onclick must be a function or null"_s);
            result.clickHandler = Protected<JSObjectRef>(JSContextGetGlobalContext(context), JSValueToObject(context, field, nullptr));
            continue;
        }
        // Like Safari, ignore properties menus do not have, such as Firefox's
        // viewTypes or a method a page added to Object.prototype (ClearURLs).
        if (!isMenuProperty(key))
            continue;
        RefPtr converted = menuPropertyValue(context, key, field);
        if (!converted)
            return std::unexpected("unsupported menu property or invalid JavaScript value"_s);
        if (key == "id"_s)
            result.explicitIdentifier = true;
        properties->setValue(key, converted.releaseNonNull());
    }
    auto parameters = parseWebExtensionMenuItemProperties(properties.ptr(), operation, manifestVersion3, baseURL);
    if (!parameters)
        return std::unexpected(parameters.error());
    result.parameters = WTF::move(*parameters);
    return result;
}

std::expected<String, String> parseWebExtensionMenuIdentifier(JSContextRef context, JSValueRef value)
{
    if (value && JSValueIsString(context, value)) {
        JSRetainPtr string(Adopt, JSValueToStringCopy(context, value, nullptr));
        return encodeWebExtensionMenuIdentifier(JSON::Value::create(toString(string.get())).ptr());
    }
    if (value && JSValueIsNumber(context, value)) {
        double number = JSValueToNumber(context, value, nullptr);
        if (std::isfinite(number))
            return encodeWebExtensionMenuIdentifier(JSON::Value::create(number).ptr());
    }
    return std::unexpected("menu identifiers must be nonempty strings or safe integers"_s);
}

JSValueRef toWebAPIMenuIdentifier(JSContextRef context, const String& nativeKey)
{
    auto identifier = decodeWebExtensionMenuIdentifier(nativeKey);
    if (!identifier)
        return JSValueMakeNull(context);
    if (auto string = std::get_if<String>(&*identifier))
        return JSValueMakeString(context, toJSString(*string).get());
    return JSValueMakeNumber(context, std::get<int64_t>(*identifier));
}

JSValueRef toWebAPIMenuClickInfo(JSContextRef context, const WebExtensionMenuItemParameters& parameters, bool wasChecked, const WebExtensionMenuItemContextParameters& details)
{
    if (!parameters.type || !decodeWebExtensionMenuIdentifier(parameters.identifier) || (parameters.parentIdentifier && !decodeWebExtensionMenuIdentifier(*parameters.parentIdentifier)))
        return JSValueMakeNull(context);
    switch (*parameters.type) {
    case WebExtensionMenuItemType::Normal:
    case WebExtensionMenuItemType::Checkbox:
    case WebExtensionMenuItemType::Radio:
        break;
    case WebExtensionMenuItemType::Separator:
        return JSValueMakeNull(context);
    default:
        return JSValueMakeNull(context);
    }
    if (details.frameIdentifier && !isNone(details.frameIdentifier) && !isMainFrame(details.frameIdentifier) && details.frameIdentifier->toUInt64() > 9007199254740991ULL)
        return JSValueMakeNull(context);
    Ref result = JSON::Object::create();
    auto identifier = [&](ASCIILiteral key, const String& nativeKey) {
        // Both identifiers were validated above. Decode into native JSON values
        // so serialization preserves string/integer identity without running JS.
        auto value = decodeWebExtensionMenuIdentifier(nativeKey);
        if (auto* text = std::get_if<String>(&*value))
            result->setString(key, *text);
        else
            result->setDouble(key, static_cast<double>(std::get<int64_t>(*value)));
    };
    auto string = [&](ASCIILiteral key, const String& value) {
        result->setString(key, value);
    };
    identifier("menuItemId"_s, parameters.identifier);
    if (parameters.parentIdentifier)
        identifier("parentMenuItemId"_s, *parameters.parentIdentifier);
    if (isCheckedType(*parameters.type)) {
        result->setBoolean("checked"_s, parameters.checked.value_or(false));
        result->setBoolean("wasChecked"_s, wasChecked);
    }
    result->setBoolean("editable"_s, details.editable);
    if (!details.selectionString.isNull())
        string("selectionText"_s, details.selectionString);
    if (!details.sourceURL.isNull()) {
        string("srcUrl"_s, details.sourceURL.string());
        using Type = WebExtensionMenuItemContextType;
        if (details.types.contains(Type::Audio)) string("mediaType"_s, "audio"_s);
        else if (details.types.contains(Type::Image)) string("mediaType"_s, "image"_s);
        else if (details.types.contains(Type::Video)) string("mediaType"_s, "video"_s);
    }
    if (!details.linkURL.isNull())
        string("linkUrl"_s, details.linkURL.string());
    if (!details.linkText.isNull())
        string("linkText"_s, details.linkText);
    if (isValid(details.frameIdentifier) && !details.frameURL.isNull()) {
        result->setDouble("frameId"_s, toWebAPI(*details.frameIdentifier));
        string(isMainFrame(details.frameIdentifier) ? "pageUrl"_s : "frameUrl"_s, details.frameURL.string());
    }
    // JSC's native JSON parser creates ordinary own data properties. Assigning
    // through JSObjectSetProperty could invoke an extension's inherited setters.
    // This C API entry point does not call the replaceable global JSON.parse.
    return JSValueMakeFromJSONString(context, toJSString(result->toJSONString()).get());
}
}
#endif
