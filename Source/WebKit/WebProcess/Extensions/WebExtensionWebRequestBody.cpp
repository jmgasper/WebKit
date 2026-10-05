/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionWebRequestBody.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionString.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/StrongInlines.h>
#include <wtf/ASCIICType.h>
#include <wtf/JSONValues.h>
#include <wtf/URLParser.h>

namespace WebKit {

JSObjectRef webExtensionWebRequestBody(JSContextRef context, std::span<const WebCore::FormDataElement> elements, const String& contentType, JSValueRef& exception)
{
    if (!context || elements.empty())
        return nullptr;
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    auto body = JSON::Object::create();
    bool allBytes = true;
    for (auto& element : elements) {
        if (std::holds_alternative<WebCore::FormDataElement::EncodedBlobData>(element.data)
            || std::holds_alternative<WebCore::FormDataElement::PendingStreamData>(element.data)) {
            body->setString("error"_s, "Request body contains an unresolved blob or stream."_s);
            return JSValueToObject(context, JSValueMakeFromJSONString(context, toJSString(body->toJSONString()).get()), &exception);
        }
        allBytes &= std::holds_alternative<Vector<uint8_t>>(element.data);
    }
    auto mimeType = contentType.left(contentType.find(';')).trim(isASCIIWhitespace);
    if (allBytes && equalLettersIgnoringASCIICase(mimeType, "application/x-www-form-urlencoded"_s)) {
        // The transport can split a key or UTF-8 sequence between elements.
        Vector<uint8_t> bytes;
        for (auto& element : elements)
            bytes.appendVector(std::get<Vector<uint8_t>>(element.data));
        auto fields = JSON::Object::create();
        for (auto& pair : WTF::URLParser::parseURLEncodedForm(String::fromUTF8ReplacingInvalidSequences(bytes.span()))) {
            RefPtr values = fields->getArray(pair.key);
            if (!values) {
                values = JSON::Array::create();
                fields->setArray(pair.key, Ref { *values });
            }
            values->pushString(pair.value);
        }
        body->setObject("formData"_s, WTF::move(fields));
        return JSValueToObject(context, JSValueMakeFromJSONString(context, toJSString(body->toJSONString()).get()), &exception);
    }
    auto raw = JSON::Array::create();
    for (auto& element : elements) {
        auto entry = JSON::Object::create();
        if (auto* file = std::get_if<WebCore::FormDataElement::EncodedFileData>(&element.data))
            entry->setString("file"_s, file->filename);
        else
            entry->setValue("bytes"_s, JSON::Value::null());
        raw->pushObject(WTF::move(entry));
    }
    body->setArray("raw"_s, WTF::move(raw));
    auto object = JSValueToObject(context, JSValueMakeFromJSONString(context, toJSString(body->toJSONString()).get()), &exception);
    if (exception || !object)
        return nullptr;
    JSC::Strong<JSC::Unknown> protectedBody(vm, toJS(globalObject, object));
    auto array = JSValueToObject(context, JSObjectGetProperty(context, object, toJSString("raw"_s).get(), &exception), &exception);
    if (exception)
        return nullptr;
    for (size_t i = 0; i < elements.size(); ++i) {
        auto* bytes = std::get_if<Vector<uint8_t>>(&elements[i].data);
        if (!bytes)
            continue;
        // A private copy for each callback prevents listeners from modifying the
        // upload or a later listener's ArrayBuffer. JSC owns the allocation.
        void* copy = fastMalloc(std::max<size_t>(bytes->size(), 1));
        if (!bytes->isEmpty())
            memcpy(copy, bytes->span().data(), bytes->size());
        auto buffer = JSObjectMakeArrayBufferWithBytesNoCopy(context, copy, bytes->size(), [](void* data, void*) { fastFree(data); }, nullptr, &exception);
        if (exception || !buffer)
            return nullptr;
        JSC::Strong<JSC::Unknown> protectedBuffer(vm, toJS(globalObject, buffer));
        auto entry = JSValueToObject(context, JSObjectGetPropertyAtIndex(context, array, i, &exception), &exception);
        if (exception)
            return nullptr;
        // JSON created an own writable bytes property, so prototype setters
        // cannot intercept this assignment.
        JSObjectSetProperty(context, entry, toJSString("bytes"_s).get(), buffer, kJSPropertyAttributeNone, &exception);
        if (exception)
            return nullptr;
    }
    return object;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
