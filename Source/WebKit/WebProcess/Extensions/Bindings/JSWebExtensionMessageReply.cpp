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
#include "JSWebExtensionMessageReply.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "Protected.h"
#include "WebExtensionConstants.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <wtf/Vector.h>

namespace WebKit {

Ref<WebExtensionMessageReply> createWebExtensionMessageReply(CompletionHandler<void(String&&)>&& completionHandler)
{
    return WebExtensionMessageReply::create([completionHandler = WTF::move(completionHandler)](JSContextRef context, JSValueRef reply) mutable {
        if (!context) {
            completionHandler({ });
            return;
        }

        JSRetainPtr json(Adopt, JSValueCreateJSONString(context, reply, 0, nullptr));
        if (!json || JSStringGetLength(json.get()) > webExtensionMaxMessageLength) {
            completionHandler(String { emptyString() });
            return;
        }

        Vector<char> buffer(JSStringGetMaximumUTF8CStringSize(json.get()));
        JSStringGetUTF8CString(json.get(), buffer.mutableSpan().data(), buffer.size());
        completionHandler(String::fromUTF8(buffer.span().data()));
    }, nullptr, nullptr);
}

JSObjectRef createWebExtensionMessageReplyFunction(JSContextRef context, Ref<WebExtensionMessageReply>&& callback)
{
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    auto* function = JSC::JSNativeStdFunction::create(vm, globalObject, 1, String(), [callback = WTF::move(callback)](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) {
        callback.get()(toRef(globalObject), toRef(globalObject, callFrame->argument(0)));
        return JSC::JSValue::encode(JSC::jsUndefined());
    });
    return toRef(function);
}

void invokeWebExtensionMessageReplyThenable(JSContextRef context, JSValueRef thenable, Ref<WebExtensionMessageReply>&& callback, JSValueRef& exception)
{
    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSValueRef> protectedThenable(globalContext, thenable);
    auto thisObject = JSValueToObject(context, thenable, &exception);
    if (exception)
        return;

    JSRetainPtr thenName(Adopt, JSStringCreateWithUTF8CString("then"));
    auto method = JSObjectGetProperty(context, thisObject, thenName.get(), &exception);
    if (exception)
        return;
    Protected<JSValueRef> protectedMethod(globalContext, method);
    auto thenFunction = JSValueToObject(context, method, &exception);
    if (exception)
        return;

    Protected<JSValueRef> resolveFunction(globalContext, createWebExtensionMessageReplyFunction(context, WTF::move(callback)));
    Protected<JSValueRef> rejectFunction(globalContext, JSObjectMakeFunctionWithCallback(context, nullptr, [](JSContextRef context, JSObjectRef, JSObjectRef, size_t, const JSValueRef[], JSValueRef*) {
        return JSValueMakeUndefined(context);
    }));
    std::array<JSValueRef, 2> arguments { resolveFunction.get(), rejectFunction.get() };
    JSObjectCallAsFunction(context, thenFunction, thisObject, arguments.size(), arguments.data(), &exception);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
