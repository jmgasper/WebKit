/*
 * Copyright (C) 2022-2026 Apple Inc. All rights reserved.
 * Copyright (C) 2025 Igalia, S.L. All rights reserved.
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
#include "WebExtensionAPITest.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "MessageSenderInlines.h"
#include "JSWebExtensionPromise.h"
#include "WebExtensionAPINamespace.h"
#include "WebExtensionAPIWebPageNamespace.h"
#include "WebExtensionControllerMessages.h"
#include "WebExtensionControllerProxy.h"
#include "WebExtensionEventListenerType.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebProcess.h"
#include <JavaScriptCore/APICast.h>
#include <JavaScriptCore/ScriptCallStack.h>
#include <JavaScriptCore/ScriptCallStackFactory.h>
#include <WebCore/DocumentPage.h>
#include <WebCore/LocalFrameInlines.h>
#include <wtf/WorkQueue.h>

namespace WebKit {

std::pair<String, unsigned> WebExtensionAPITest::scriptLocation(JSContextRef context)
{
    auto callStack = Inspector::createScriptCallStack(toJS(context));
    if (const Inspector::ScriptCallFrame* frame = callStack->firstNonNativeCallFrame()) {
        auto sourceURL = frame->sourceURL();
        if (sourceURL.isEmpty())
            sourceURL = "global code"_s;
        return { sourceURL, frame->lineNumber() };
    }

    return { "unknown"_s, 0 };
}

template<size_t ArgumentCount>
JSValueRef WebExtensionAPITest::invokeMethod(JSContextRef context, JSValueRef value, const String& method, std::array<JSValueRef, ArgumentCount>&& arguments, JSValueRef* exception)
{
    if (!context || !value)
        return JSValueMakeUndefined(context);

    if (!JSValueIsObject(context, value))
        return JSValueMakeUndefined(context);

    JSObjectRef thisObject = JSValueToObject(context, value, exception);
    if (!thisObject || (exception && *exception))
        return JSValueMakeUndefined(context);

    // This is a safer cpp false positive (rdar://163760990).
    SUPPRESS_UNCOUNTED_ARG JSValueRef function = JSObjectGetProperty(context, thisObject, toJSString(method).get(), exception);
    if (!function || (exception && *exception))
        return JSValueMakeUndefined(context);

    JSObjectRef funcObject = JSValueToObject(context, function, exception);
    if (!funcObject || (exception && *exception))
        return JSValueMakeUndefined(context);

    return JSObjectCallAsFunction(context, funcObject, thisObject, ArgumentCount, arguments.data(), exception);
}

void WebExtensionAPITest::notifyFail(JSContextRef context, const String& message)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    WebProcess::singleton().send(Messages::WebExtensionController::TestFinished(nullString(), false, message, location.first, location.second), webExtensionControllerProxy->identifier());
}

void WebExtensionAPITest::notifyPass(JSContextRef context, const String& message)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    WebProcess::singleton().send(Messages::WebExtensionController::TestFinished(nullString(), true, message, location.first, location.second), webExtensionControllerProxy->identifier());
}

void WebExtensionAPITest::sendMessage(JSContextRef context, const String& message, JSValueRef argument)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    WebProcess::singleton().send(Messages::WebExtensionController::TestSentMessage(message, toSortedJSONString(context, argument), location.first, location.second), webExtensionControllerProxy->identifier());
}

WebExtensionAPIEvent& WebExtensionAPITest::onMessage()
{
    if (!m_onMessage)
        m_onMessage = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TestOnMessage);

    return *m_onMessage;
}

WebExtensionAPIEvent& WebExtensionAPITest::onTestStarted()
{
    if (!m_onTestStarted)
        m_onTestStarted = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TestOnTestStarted);

    return *m_onTestStarted;
}

WebExtensionAPIEvent& WebExtensionAPITest::onTestFinished()
{
    if (!m_onTestFinished)
        m_onTestFinished = WebExtensionAPIEvent::create(*this, WebExtensionEventListenerType::TestOnTestFinished);

    return *m_onTestFinished;
}

JSValueRef WebExtensionAPITest::runWithUserGesture(WebFrame& frame, JSContextRef context, JSValueRef function)
{
    RefPtr coreFrame = frame.coreLocalFrame();
    WebCore::UserGestureIndicator gestureIndicator(WebCore::IsProcessingUserGesture::Yes, protect(coreFrame ? coreFrame->document() : nullptr));

    return callObjectWithArguments<0>(function, context, { });
}

bool WebExtensionAPITest::isProcessingUserGesture()
{
    return WebCore::UserGestureIndicator::processingUserGesture();
}

String WebExtensionAPITest::debugString(JSContextRef contextRef, JSValueRef valueRef)
{
    if (isRegularExpression(contextRef, valueRef) || isFunction(contextRef, valueRef))
        return toString(contextRef, valueRef);
    auto sortedJSON = toSortedJSONString(contextRef, valueRef);
    if (!sortedJSON.isEmpty())
        return sortedJSON;
    return "undefined"_s;
}

void WebExtensionAPITest::log(JSContextRef context, JSValueRef value)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    WebProcess::singleton().send(Messages::WebExtensionController::TestLogMessage(debugString(context, value), location.first, location.second), webExtensionControllerProxy->identifier());
}

void WebExtensionAPITest::fail(JSContextRef context, const String& message)
{
    String empty = nullString();
    assertTrue(context, false, message, empty);
}

void WebExtensionAPITest::succeed(JSContextRef context, const String& message)
{
    String empty = nullString();
    assertTrue(context, true, message, empty);
}

void WebExtensionAPITest::assertTrue(JSContextRef context, bool actualValue, const String& message, String& outExceptionString)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    recordAssertionIfNeeded(actualValue, message, location, outExceptionString);

    WebProcess::singleton().send(Messages::WebExtensionController::TestResult(actualValue, message, location.first, location.second), webExtensionControllerProxy->identifier());
}

void WebExtensionAPITest::assertFalse(JSContextRef context, bool actualValue, const String& message, String& outExceptionString)
{
    assertTrue(context, !actualValue, message, outExceptionString);
}

void WebExtensionAPITest::assertDeepEq(JSContextRef context, JSValueRef actualValue, JSValueRef expectedValue, const String& message, String& outExceptionString)
{
    String expectedJSONValue = debugString(context, expectedValue);
    String actualJSONValue = debugString(context, actualValue);

    // FIXME: Comparing JSON is a quick attempt that works, but can still fail due to any non-JSON values.
    // See Firefox's implementation: https://searchfox.org/mozilla-central/source/toolkit/components/extensions/child/ext-test.js#78
    bool strictEqual = JSValueIsStrictEqual(context, expectedValue, actualValue);
    bool deepEqual = strictEqual || expectedJSONValue == actualJSONValue;

    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    recordAssertionIfNeeded(deepEqual, message, location, outExceptionString);

    WebProcess::singleton().send(Messages::WebExtensionController::TestEqual(deepEqual, expectedJSONValue, actualJSONValue, message, location.first, location.second), webExtensionControllerProxy->identifier());
}

String WebExtensionAPITest::combineMessages(const String& messageOne, const String& messageTwo)
{
    if (!messageOne.isEmpty() && !messageTwo.isEmpty()) {
        Vector<String> stringList = { messageOne, messageTwo };
        return makeStringByJoining(stringList, "\n"_s);
    }

    if (!messageOne.isEmpty() && messageTwo.isEmpty())
        return messageOne;
    return messageTwo;
}

void WebExtensionAPITest::assertEquals(JSContextRef context, bool result, const String& expectedString, const String& actualString, const String& message, String& outExceptionString)
{
    auto location = scriptLocation(context);

    RefPtr page = toWebPage(context);
    if (!page)
        return;

    RefPtr webExtensionControllerProxy = page->webExtensionControllerProxy();
    if (!webExtensionControllerProxy)
        return;

    recordAssertionIfNeeded(result, message, location, outExceptionString);

    WebProcess::singleton().send(Messages::WebExtensionController::TestEqual(result, expectedString, actualString, message, location.first, location.second), webExtensionControllerProxy->identifier());
}

void WebExtensionAPITest::assertEq(JSContextRef context, JSValueRef actualValue, JSValueRef expectedValue, const String& message, String& outExceptionString)
{
    String expectedJSONValue = debugString(context, expectedValue);
    String actualJSONValue = debugString(context, actualValue);

    bool strictEqual = JSValueIsStrictEqual(context, expectedValue, actualValue);
    if (!strictEqual && expectedJSONValue == actualJSONValue)
        actualJSONValue = makeString(actualJSONValue, " (different)"_s);

    assertEquals(context, strictEqual, expectedJSONValue, actualJSONValue, message, outExceptionString);
}


JSValueRef WebExtensionAPITest::assertRejects(JSContextRef context, JSValueRef promise, JSValueRef expectedError, const String& message)
{
    return transformWebExtensionPromise(context, promise, expectedError,
        [this, protectedThis = Ref { *this }, message, hasExpectedError = !!expectedError](JSContextRef context, bool fulfilled, JSValueRef value, JSValueRef expectedError, JSValueRef& exception) -> JSValueRef {
            String falseException;
            if (fulfilled) {
                assertEquals(context, false, hasExpectedError ? debugString(context, expectedError) : "(any error)"_s, debugString(context, value), combineMessages(message, "Promise did not reject with an error"_s), falseException);
                exception = JSValueMakeUndefined(context);
                return JSValueMakeUndefined(context);
            }

            Protected<JSValueRef> errorMessage(JSContextGetGlobalContext(context), webExtensionPromiseErrorMessage(context, value, exception));
            if (exception)
                return JSValueMakeUndefined(context);
            if (JSValueIsUndefined(context, expectedError)) {
                assertEquals(context, true, "(any error)"_s, debugString(context, errorMessage.get()), combineMessages(message, "Promise rejected with an error"_s), falseException);
                return JSValueMakeUndefined(context);
            }
            if (isRegularExpression(context, expectedError)) {
                JSValueRef result = invokeMethod<1>(context, expectedError, "test"_s, { errorMessage.get() }, &exception);
                if (exception)
                    return JSValueMakeUndefined(context);
                assertEquals(context, JSValueToBoolean(context, result), debugString(context, expectedError), debugString(context, errorMessage.get()), combineMessages(message, "Promise rejected with an error that didn't match the regular expression"_s), falseException);
                return JSValueMakeUndefined(context);
            }
            bool equal = JSValueIsEqual(context, expectedError, errorMessage.get(), &exception);
            if (exception)
                return JSValueMakeUndefined(context);
            assertEquals(context, equal, debugString(context, expectedError), debugString(context, errorMessage.get()), combineMessages(message, "Promise rejected with an error that didn't equal"_s), falseException);
            return JSValueMakeUndefined(context);
        });
}

JSValueRef WebExtensionAPITest::assertResolves(JSContextRef context, JSValueRef promise, const String& message)
{
    if (!isThenable(context, promise))
        return JSValueMakeNull(context);
    return transformWebExtensionPromise(context, promise, nullptr,
        [this, protectedThis = Ref { *this }, message](JSContextRef context, bool fulfilled, JSValueRef value, JSValueRef, JSValueRef& exception) -> JSValueRef {
            if (fulfilled) {
                succeed(context, "Promise resolved without an error"_s);
                return value;
            }
            Protected<JSValueRef> errorMessage(JSContextGetGlobalContext(context), webExtensionPromiseErrorMessage(context, value, exception));
            if (exception)
                return JSValueMakeUndefined(context);
            fail(context, combineMessages(message, makeString("Promise rejected with an error: "_s, debugString(context, errorMessage.get()))));
            return JSValueMakeUndefined(context);
        });
}

void WebExtensionAPITest::assertThrows(JSContextRef context, JSValueRef function, JSValueRef expectedError, const String& message, String& outExceptionString)
{
    JSValueRef exceptionValue = nullptr;
    callObjectWithArguments<0>(function, context, { }, &exceptionValue, false);

    if (!exceptionValue) {
        assertEquals(context, false, expectedError ? debugString(context, expectedError) : "(any exception)"_s, "(no exception)"_s, combineMessages(message, "Function did not throw an exception"_s), outExceptionString);
        return;
    }

    JSValueRef exceptionMessageValue = exceptionValue;
    if (JSValueIsObject(context, exceptionValue)) {
        JSObjectRef object = JSValueToObject(context, exceptionValue, nullptr);

        // This is a safer cpp false positive (rdar://163760990).
        SUPPRESS_UNCOUNTED_ARG if (JSObjectHasProperty(context, object, toJSString("message"_s).get()))
            SUPPRESS_UNCOUNTED_ARG exceptionMessageValue = JSObjectGetProperty(context, object, toJSString("message"_s).get(), 0);
    }

    // Clear the exception since it was caught.
    exceptionValue = nullptr;

    if (JSValueIsUndefined(context, expectedError) || !expectedError) {
        assertEquals(context, true, "(any exception)"_s, debugString(context, exceptionMessageValue), combineMessages(message, "Function threw an exception"_s), outExceptionString);
        return;
    }

    if (isRegularExpression(context, expectedError)) {
        JSValueRef testResult = invokeMethod<1>(context, expectedError, "test"_s, { exceptionMessageValue });
        assertEquals(context, JSValueToBoolean(context, testResult), debugString(context, expectedError), debugString(context, exceptionMessageValue), combineMessages(message, "Function threw an exception that didn't match the regular expression"_s), outExceptionString);
        return;
    }

    assertEquals(context, JSValueIsEqual(context, expectedError, exceptionMessageValue, nullptr), debugString(context, expectedError), debugString(context, exceptionMessageValue), combineMessages(message, "Function threw an exception that didn't equal"_s), outExceptionString);
}

JSValueRef WebExtensionAPITest::assertSafe(JSContextRef context, JSValueRef function, const String& message)
{
    JSValueRef exceptionValue = nullptr;
    JSValueRef result = callObjectWithArguments<0>(function, context, { }, &exceptionValue, false);

    if (!exceptionValue) {
        succeed(context, "Function did not throw an exception"_s);
        return result;
    }

    JSValueRef exceptionMessageValue = exceptionValue;
    if (JSValueIsObject(context, exceptionValue)) {
        JSObjectRef object = JSValueToObject(context, exceptionValue, nullptr);

        // This is a safer cpp false positive (rdar://163760990).
        SUPPRESS_UNCOUNTED_ARG if (JSObjectHasProperty(context, object, toJSString("message"_s).get()))
            SUPPRESS_UNCOUNTED_ARG exceptionMessageValue = JSObjectGetProperty(context, object, toJSString("message"_s).get(), 0);
    }

    // Clear the exception since it was caught.
    exceptionValue = nullptr;

    fail(context, combineMessages(message, makeString("Function threw an exception: "_s, debugString(context, exceptionMessageValue))));

    return JSValueMakeUndefined(context);
}

JSValueRef WebExtensionAPITest::assertSafeResolve(JSContextRef context, JSValueRef function, const String& message)
{
    JSValueRef result = assertSafe(context, function, message);
    if (!isThenable(context, function))
        return result;

    return assertResolves(context, result, message);
}

static JSValueRef rejectedTestPromise(JSContextRef context, JSValueRef reason)
{
    return transformWebExtensionPromise(context, nullptr, reason,
        [](JSContextRef context, bool, JSValueRef, JSValueRef reason, JSValueRef& exception) -> JSValueRef {
            exception = reason;
            return JSValueMakeUndefined(context);
        });
}

JSValueRef WebExtensionAPITest::addTest(JSContextRef context, JSValueRef testFunction, String callingAPIName)
{
    auto reject = [&](const String& message) {
        auto reason = toJSString(toErrorString(callingAPIName, nullString(), message));
        return rejectedTestPromise(context, JSValueMakeString(context, reason.get()));
    };
    if (!isFunction(context, testFunction))
        return reject("Error creating a new test."_s);

    JSValueRef exception = nullptr;
    auto functionObject = JSValueToObject(context, testFunction, &exception);
    if (exception)
        return rejectedTestPromise(context, exception);
    auto rawName = JSObjectGetProperty(context, functionObject, toJSString("name"_s).get(), &exception);
    if (exception)
        return rejectedTestPromise(context, exception);
    Protected<JSValueRef> nameValue(JSContextGetGlobalContext(context), rawName);
    JSRetainPtr nameString(Adopt, JSValueToStringCopy(context, nameValue.get(), &exception));
    if (exception)
        return rejectedTestPromise(context, exception);
    auto testName = toString(nameString.get());
    if (testName.isEmpty())
        return reject("The supplied test function must be named."_s);

    RefPtr page = toWebPage(context);
    if (!page)
        return reject("Error creating a new test."_s);
    RefPtr controller = page->webExtensionControllerProxy();
    if (!controller)
        return reject("Error creating a new test."_s);

    JSObjectRef resolve = nullptr;
    JSObjectRef rejectCallback = nullptr;
    JSObjectRef result = JSObjectMakeDeferredPromise(context, &resolve, &rejectCallback, &exception);
    if (exception)
        return rejectedTestPromise(context, exception);
    if (!result)
        return nullptr;
    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSValueRef> protectedResult(globalContext, result);
    Protected<JSValueRef> protectedResolve(globalContext, resolve);
    Protected<JSValueRef> protectedReject(globalContext, rejectCallback);
    auto location = scriptLocation(context);
    auto identifier = controller->identifier();
    m_testQueue.append({ testName, location, identifier, Protected(globalContext, testFunction), WTF::move(protectedResolve), WTF::move(protectedReject) });
    WebProcess::singleton().send(Messages::WebExtensionController::TestAdded(testName, location.first, location.second), identifier);

    if (!m_runningTest) {
        m_runningTest = true;
        WorkQueue::mainSingleton().dispatch([this, protectedThis = Ref { *this }] { startNextTest(); });
    }
    return result;
}

void WebExtensionAPITest::startNextTest()
{
    if (m_testQueue.isEmpty()) {
        m_runningTest = false;
        return;
    }
    auto test = m_testQueue.takeFirst();
    auto context = test.testFunction.context().get();
    m_hitAssertion = false;
    m_assertionMessage = nullString();
    WebProcess::singleton().send(Messages::WebExtensionController::TestStarted(test.testName, test.location.first, test.location.second), test.webExtensionControllerIdentifier);

    JSValueRef exception = nullptr;
    auto rawValue = callObjectWithArguments<0>(test.testFunction.get(), context, { }, &exception);
    if (!rawValue && !exception) {
        WebProcess::singleton().send(Messages::WebExtensionController::TestFinished(test.testName, false, "Test execution context is unavailable."_s, test.location.first, test.location.second), test.webExtensionControllerIdentifier);
        if (m_testQueue.isEmpty())
            m_runningTest = false;
        else
            WorkQueue::mainSingleton().dispatch([this, protectedThis = Ref { *this }] { startNextTest(); });
        return;
    }
    Protected<JSValueRef> value(context, rawValue ? rawValue : JSValueMakeUndefined(context));
    Protected<JSValueRef> thrown(context, exception ? exception : JSValueMakeUndefined(context));
    Protected<JSValueRef> execution(context, exception ? rejectedTestPromise(context, thrown.get()) : value.get());
    JSValueRef callbacks[] { test.resolveCallback.get(), test.rejectCallback.get() };
    Protected<JSValueRef> callbackValues(context, JSObjectMakeArray(context, 2, callbacks, nullptr));

    // Trace the running test's completion functions through the promise graph;
    // only queued tests need the Protected values held by m_testQueue.
    transformWebExtensionPromise(context, execution.get(), callbackValues.get(),
        [this, protectedThis = Ref { *this }, testName = test.testName, location = test.location, identifier = test.webExtensionControllerIdentifier]
        (JSContextRef context, bool fulfilled, JSValueRef value, JSValueRef callbacks, JSValueRef&) -> JSValueRef {
            bool success = fulfilled && !m_hitAssertion;
            String message;
            if (success)
                message = "Promise resolved without an error."_s;
            else {
                String errorMessage;
                if (!fulfilled) {
                    JSValueRef exception = nullptr;
                    Protected<JSValueRef> reason(JSContextGetGlobalContext(context), webExtensionPromiseErrorMessage(context, value, exception));
                    errorMessage = debugString(context, exception ? exception : reason.get());
                } else if (!m_assertionMessage.isNull())
                    errorMessage = m_assertionMessage;
                message = errorMessage.isNull() ? "Promise rejected without an error"_s : combineMessages("Promise rejected with an error: "_s, errorMessage);
            }
            WebProcess::singleton().send(Messages::WebExtensionController::TestFinished(testName, success, message, location.first, location.second), identifier);
            auto callbackArray = JSValueToObject(context, callbacks, nullptr);
            auto callback = JSObjectGetPropertyAtIndex(context, callbackArray, success ? 0 : 1, nullptr);
            if (success)
                callObjectWithArguments<1>(callback, context, { value });
            else
                callObjectWithArguments<0>(callback, context, { });
            m_hitAssertion = false;
            m_assertionMessage = nullString();
            if (m_testQueue.isEmpty())
                m_runningTest = false;
            else
                WorkQueue::mainSingleton().dispatch([this, protectedThis = Ref { *this }] { startNextTest(); });
            return JSValueMakeUndefined(context);
        });
}

JSValueRef WebExtensionAPITest::addTest(JSContextRef context, JSValueRef testFunction)
{
    return addTest(context, testFunction, "test.addTest()"_s);
}

JSValueRef WebExtensionAPITest::runTests(JSContextRef context, Vector<Protected<JSValueRef>> testFunctions)
{
    JSObjectRef testResultPromises = JSObjectMakeArray(context, 0, nullptr, nullptr);

    for (Protected<JSValueRef> testFunction : testFunctions)
        invokeMethod<1>(context, testResultPromises, "push"_s, { addTest(context, testFunction.get(), "test.runTests()"_s) });

    // This is a safer cpp false positive (rdar://163760990).
    SUPPRESS_UNCOUNTED_ARG return invokeMethod<1>(context, JSObjectGetProperty(context, JSContextGetGlobalObject(context), toJSString("Promise"_s).get(), nullptr), "all"_s, { testResultPromises });
}

void WebExtensionAPITest::recordAssertionIfNeeded(bool result, const String& message, std::pair<String, unsigned> location, String& outExceptionString)
{
    if (!m_runningTest || (m_runningTest && result))
        return;

    m_hitAssertion = true;
    m_assertionMessage = message;

    Vector<String> locationVector = { location.first, String::number(location.second) };
    auto locationString = makeStringByJoining(locationVector, ":"_s);
    outExceptionString = message.isNull()
        ? makeString("Assertion Failed: "_s, locationString)
        : makeString("Assertion Failed: "_s, message, ". "_s, locationString);
}

void WebExtensionContextProxy::dispatchTestMessageEvent(const String& message, const String& argumentJSON, WebExtensionContentWorldType contentWorldType)
{
    if (contentWorldType == WebExtensionContentWorldType::WebPage) {
        enumerateFramesAndWebPageNamespaceObjects([&](auto&, auto& namespaceObject) {
            namespaceObject.test().onMessage().invokeListenersWithJSONArgument(message, argumentJSON);
        });

        return;
    }

    enumerateFramesAndNamespaceObjects([&](auto&, auto& namespaceObject) {
        namespaceObject.test().onMessage().invokeListenersWithJSONArgument(message, argumentJSON);
    }, toDOMWrapperWorld(contentWorldType));
}

void WebExtensionContextProxy::dispatchTestStartedEvent(const String& argumentJSON, WebExtensionContentWorldType contentWorldType)
{
    if (contentWorldType == WebExtensionContentWorldType::WebPage) {
        enumerateFramesAndWebPageNamespaceObjects([&](auto&, auto& namespaceObject) {
            namespaceObject.test().onTestStarted().invokeListenersWithJSONArgument(argumentJSON);
        });

        return;
    }

    enumerateFramesAndNamespaceObjects([&](auto&, auto& namespaceObject) {
        namespaceObject.test().onTestStarted().invokeListenersWithJSONArgument(argumentJSON);
    }, toDOMWrapperWorld(contentWorldType));
}

void WebExtensionContextProxy::dispatchTestFinishedEvent(const String& argumentJSON, WebExtensionContentWorldType contentWorldType)
{
    if (contentWorldType == WebExtensionContentWorldType::WebPage) {
        enumerateFramesAndWebPageNamespaceObjects([&](auto&, auto& namespaceObject) {
            namespaceObject.test().onTestFinished().invokeListenersWithJSONArgument(argumentJSON);
        });

        return;
    }

    enumerateFramesAndNamespaceObjects([&](auto&, auto& namespaceObject) {
        namespaceObject.test().onTestFinished().invokeListenersWithJSONArgument(argumentJSON);
    }, toDOMWrapperWorld(contentWorldType));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
