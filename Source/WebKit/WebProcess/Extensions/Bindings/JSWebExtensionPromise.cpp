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
#include "JSWebExtensionPromise.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/ExceptionHelpers.h>
#include <JavaScriptCore/JSCJSValueInlines.h>
#include <JavaScriptCore/JSLock.h>
#include <JavaScriptCore/JSRetainPtr.h>
#include <JavaScriptCore/JSNativeStdFunction.h>
#include <JavaScriptCore/JSPromise.h>
#include <JavaScriptCore/StrongInlines.h>
#include <JavaScriptCore/ThrowScope.h>
#include <wtf/SharedTask.h>

namespace WebKit {

JSValueRef webExtensionPromiseErrorMessage(JSContextRef context, JSValueRef error, JSValueRef& exception)
{
    if (!JSValueIsObject(context, error))
        return error;
    JSObjectRef object = JSValueToObject(context, error, &exception);
    if (exception)
        return JSValueMakeUndefined(context);
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    JSC::Strong<JSC::Unknown> protectedError(vm, toJS(globalObject, error));
    JSRetainPtr keyName(Adopt, JSStringCreateWithUTF8CString("message"));
    JSC::Strong<JSC::Unknown> key(vm, toJS(globalObject, JSValueMakeString(context, keyName.get())));
    bool hasMessage = JSObjectHasPropertyForKey(context, object, toRef(globalObject, key.get()), &exception);
    if (exception)
        return JSValueMakeUndefined(context);
    return hasMessage ? JSObjectGetPropertyForKey(context, object, toRef(globalObject, key.get()), &exception) : error;
}

JSObjectRef transformWebExtensionPromise(JSContextRef context, JSValueRef input, JSValueRef capturedValue, Function<WebExtensionPromiseCompletion>&& completion)
{
    ASSERT(context);
    auto* globalObject = toJS(context);
    auto& vm = globalObject->vm();
    JSC::JSLockHolder lock(vm);
    JSC::Strong<JSC::Unknown> protectedInput(vm, input ? toJS(globalObject, input) : JSC::jsUndefined());
    JSC::Strong<JSC::Unknown> protectedCapture(vm, capturedValue ? toJS(globalObject, capturedValue) : JSC::jsUndefined());
    JSC::Strong<JSC::JSPromise> source(vm, JSC::JSPromise::create(vm, globalObject->promiseStructure()));
    JSC::Strong<JSC::JSPromise> result(vm, JSC::JSPromise::create(vm, globalObject->promiseStructure()));
    auto sharedCompletion = createSharedTask<WebExtensionPromiseCompletion>(
        [completion = WTF::move(completion)](JSContextRef context, bool fulfilled, JSValueRef value, JSValueRef capture, JSValueRef& exception) mutable {
            // Promise settlement invokes exactly one handler. Release captured
            // native objects after it runs, without waiting for a future GC.
            auto callback = std::exchange(completion, { });
            return callback(context, fulfilled, value, capture, exception);
        });
    auto makeHandler = [&](bool fulfilled) {
        auto capture = protectedCapture.get();
        return JSC::JSNativeStdFunction::create(vm, globalObject, 1, String { },
            [sharedCompletion, fulfilled, capture](JSC::JSGlobalObject* globalObject, JSC::CallFrame* callFrame) {
                auto& vm = globalObject->vm();
                auto scope = DECLARE_THROW_SCOPE(vm);
                auto context = toRef(globalObject);
                JSValueRef exception = nullptr;
                auto value = sharedCompletion->run(context, fulfilled, toRef(globalObject, callFrame->argument(0)), toRef(globalObject, capture), exception);
                if (exception) {
                    JSC::throwException(globalObject, scope, toJS(globalObject, exception));
                    return JSC::JSValue::encode(JSC::jsUndefined());
                }
                return JSC::JSValue::encode(value ? toJS(globalObject, value) : JSC::jsUndefined());
            }, capture);
    };
    JSC::Strong<JSC::JSNativeStdFunction> onFulfilled(vm, makeHandler(true));
    JSC::Strong<JSC::JSNativeStdFunction> onRejected(vm, makeHandler(false));
    source->performPromiseThenExported(vm, globalObject, onFulfilled.get(), onRejected.get(), result.get());
    source->resolve(globalObject, vm, protectedInput.get());
    return toRef(result.get());
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
