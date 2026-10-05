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

#include "config.h"
#include "JSWebExtensionStorageValues.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionString.h"
#include "JSWebExtensionWrapper.h"
#include "Protected.h"
#include "WebExtensionUtilities.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/TopExceptionScope.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSObjectInlines.h>
#include <JavaScriptCore/PropertyNameArray.h>
#include <JavaScriptCore/ProxyObject.h>
#include <JavaScriptCore/ArrayConstructor.h>
#include <cmath>
#include <limits>
#include <wtf/text/MakeString.h>

namespace WebKit {

static std::expected<bool, String> storageIsArray(JSContextRef context, JSValueRef value, const String& sourceKey)
{
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    // JSValueIsArray only recognizes direct arrays; the intrinsic handles proxies.
    bool result = JSC::isArray(globalObject, toJS(globalObject, value));
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, makeUnexpected(toErrorString(nullString(), sourceKey, "its type could not be read"_s)));
        return makeUnexpected(toErrorString(nullString(), sourceKey, "its type could not be read"_s));
    }
    return result;
}

static std::expected<Vector<String>, String> storageOwnPropertyNames(JSContextRef context, JSObjectRef object, const String& sourceKey)
{
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
    auto* jsObject = toJS(object);
    JSC::PropertyNameArrayBuilder names(vm, JSC::PropertyNameMode::Strings, JSC::PrivateSymbolMode::Exclude);
    jsObject->methodTable()->getOwnPropertyNames(jsObject, globalObject, names, JSC::DontEnumPropertiesMode::Exclude);
    if (scope.exception()) {
        TRY_CLEAR_EXCEPTION(scope, makeUnexpected(toErrorString(nullString(), sourceKey, "its properties could not be read"_s)));
        return makeUnexpected(toErrorString(nullString(), sourceKey, "its properties could not be read"_s));
    }

    Vector<String> result;
    for (auto& name : names)
        result.append(name.string());
    return result;
}

std::expected<WebExtensionStorageValues, String> serializeWebExtensionStorageValues(JSContextRef context, JSValueRef items, std::optional<size_t> perItemQuota, size_t maximumBytes)
{
    if (!context || !items || !JSValueIsObject(context, items) || isFunction(context, items))
        return makeUnexpected(toErrorString(nullString(), "items"_s, "an object is expected"_s));
    auto isArray = storageIsArray(context, items, "items"_s);
    if (!isArray)
        return makeUnexpected(isArray.error());
    if (*isArray)
        return makeUnexpected(toErrorString(nullString(), "items"_s, "an object is expected"_s));

    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> object(globalContext, JSValueToObject(context, items, nullptr));
    auto names = storageOwnPropertyNames(context, object.get(), "items"_s);
    if (!names)
        return makeUnexpected(names.error());

    WebExtensionStorageValues result { JSON::Object::create() };
    for (auto& key : *names) {
        auto sourceKey = makeString("items[`"_s, key, "`]"_s);
        JSValueRef exception = nullptr;
        Protected<JSValueRef> value(globalContext, JSObjectGetProperty(context, object.get(), toJSString(key).get(), &exception));
        if (exception || !value.get())
            return makeUnexpected(toErrorString(nullString(), sourceKey, "its value could not be read"_s));
        if (JSValueIsUndefined(context, value.get()))
            continue;

        auto serialized = serializeJSObject(context, value.get(), &exception);
        if (exception || serialized.isNull())
            return makeUnexpected(toErrorString(nullString(), sourceKey, "it is not JSON-serializable"_s));

        auto keySize = key.utf8().length();
        auto valueSize = serialized.utf8().length();
        if (perItemQuota && (keySize > *perItemQuota || valueSize > *perItemQuota - keySize))
            return makeUnexpected(toErrorString(nullString(), sourceKey, "it exceeded maximum size for a single item"_s));

        if (keySize > maximumBytes - result.sizeInBytes || valueSize > maximumBytes - result.sizeInBytes - keySize)
            return makeUnexpected(toErrorString(nullString(), nullString(), "it exceeded maximum data size allowed per call"_s));

        result.sizeInBytes += keySize + valueSize;
        result.serializedValues->setString(key, serialized);
    }

    return result;
}

std::expected<WebExtensionStorageKeySelection, String> parseWebExtensionStorageKeys(JSContextRef context, JSValueRef items, WebExtensionStorageKeyMode mode)
{
    auto sourceKey = mode == WebExtensionStorageKeyMode::Get ? "items"_s : "keys"_s;
    auto invalidKeys = [&] {
        auto expected = mode == WebExtensionStorageKeyMode::Get ? "an object or a string or an array of strings or null"_s
            : mode == WebExtensionStorageKeyMode::GetBytesInUse ? "a string or an array of strings or null"_s : "a string or an array of strings"_s;
        auto provided = "a value of another type"_s;
        if (context && items) {
            if (JSValueIsNumber(context, items))
                provided = "a number"_s;
            else if (JSValueIsBoolean(context, items))
                provided = "a boolean"_s;
            else if (JSValueIsArray(context, items))
                provided = "an array of other values"_s;
            else if (JSValueIsObject(context, items))
                provided = "an object"_s;
        }
        return makeUnexpected(toErrorString(nullString(), sourceKey, makeString(expected, " is expected, but "_s, provided, " was provided"_s)));
    };
    if (!context)
        return invalidKeys();

    WebExtensionStorageKeySelection result;
    if (!items || JSValueIsNull(context, items) || JSValueIsUndefined(context, items)) {
        if (mode == WebExtensionStorageKeyMode::Remove)
            return invalidKeys();
        result.allKeys = true;
        return result;
    }

    if (JSValueIsString(context, items)) {
        result.keys.append(toString(JSRetainPtr(Adopt, JSValueToStringCopy(context, items, nullptr)).get()));
        return result;
    }
    if (!JSValueIsObject(context, items) || isFunction(context, items))
        return invalidKeys();

    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> object(globalContext, JSValueToObject(context, items, nullptr));
    auto isArray = storageIsArray(context, items, sourceKey);
    if (!isArray)
        return makeUnexpected(isArray.error());
    if (*isArray) {
        JSValueRef exception = nullptr;
        auto lengthValue = JSObjectGetProperty(context, object.get(), toJSString("length"_s).get(), &exception);
        if (exception || !lengthValue || !JSValueIsNumber(context, lengthValue))
            return invalidKeys();
        double length = JSValueToNumber(context, lengthValue, nullptr);
        if (!std::isfinite(length) || length < 0 || length > std::numeric_limits<uint32_t>::max() || std::trunc(length) != length)
            return invalidKeys();
        for (uint32_t index = 0; index < length; ++index) {
            auto value = JSObjectGetPropertyAtIndex(context, object.get(), index, &exception);
            if (exception || !value || !JSValueIsString(context, value))
                return invalidKeys();
            result.keys.append(toString(JSRetainPtr(Adopt, JSValueToStringCopy(context, value, nullptr)).get()));
        }
        // The backing store treats an empty key vector as all keys for byte counts.
        result.allKeys = mode == WebExtensionStorageKeyMode::GetBytesInUse && result.keys.isEmpty();
        return result;
    }

    if (mode != WebExtensionStorageKeyMode::Get)
        return invalidKeys();
    auto names = storageOwnPropertyNames(context, object.get(), sourceKey);
    if (!names)
        return makeUnexpected(names.error());
    result.keys = WTF::move(*names);
    for (auto& key : result.keys) {
        JSValueRef exception = nullptr;
        auto value = JSObjectGetProperty(context, object.get(), toJSString(key).get(), &exception);
        if (exception || !value)
            return makeUnexpected(toErrorString(nullString(), makeString("items[`"_s, key, "`]"_s), "its value could not be read"_s));
        result.defaultValues.set(key, Protected<JSValueRef>(globalContext, value));
    }
    return result;
}

std::expected<Protected<JSObjectRef>, String> deserializeWebExtensionStorageValues(JSContextRef context, const Vector<String>& serializedChunks, const HashMap<String, Protected<JSValueRef>>& defaultValues)
{
    auto invalidData = [] { return makeUnexpected(toErrorString(nullString(), nullString(), "the stored data could not be decoded"_s)); };
    if (!context)
        return invalidData();

    auto globalContext = JSContextGetGlobalContext(context);
    auto* globalObject = toJS(context);
    JSC::JSLockHolder lock(globalObject);
    auto& vm = globalObject->vm();
    Protected<JSObjectRef> result(globalContext, JSObjectMake(context, nullptr, nullptr));
    // Define own data properties, including __proto__, without invoking setters.
    auto putValue = [&](const String& key, JSValueRef value) {
        auto scope = DECLARE_TOP_EXCEPTION_SCOPE(vm);
        bool success = toJS(result.get())->putDirectMayBeIndex(globalObject, JSC::Identifier::fromString(vm, key), toJS(globalObject, value));
        if (scope.exception()) {
            TRY_CLEAR_EXCEPTION(scope, false);
            return false;
        }
        return success;
    };
    for (auto& entry : defaultValues) {
        if (!putValue(entry.key, entry.value.get()))
            return invalidData();
    }

    for (auto& chunk : serializedChunks) {
        auto parsed = JSON::Value::parseJSON(chunk);
        auto object = parsed ? parsed->asObject() : nullptr;
        if (!object)
            return invalidData();
        for (auto& entry : *object) {
            auto serialized = entry.value->asString();
            if (serialized.isNull())
                return invalidData();
            auto value = JSValueMakeFromJSONString(context, toJSString(serialized).get());
            if (!value)
                return invalidData();
            Protected<JSValueRef> protectedValue(globalContext, value);
            if (!putValue(entry.key, value))
                return invalidData();
        }
    }
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
