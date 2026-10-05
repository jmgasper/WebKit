/*
 * Copyright (C) 2022-2024 Apple Inc. All rights reserved.
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
#include "WebExtensionController.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <wtf/JSONValues.h>
#include <wtf/text/CString.h>

namespace WebKit {

static Ref<JSON::Object> extensionTestReport(ASCIILiteral type, const String& sourceURL, unsigned lineNumber)
{
    auto report = JSON::Object::create();
    report->setString("type"_s, type);
    report->setString("sourceURL"_s, sourceURL);
    report->setInteger("lineNumber"_s, lineNumber);
    return report;
}

static void logExtensionTestReport(const JSON::Object& report)
{
    // Test IPC is guarded by inTestingMode. Keep reports available in release
    // builds too, and JSON-escape extension text so each report occupies one line.
    WTFLogAlways("WebExtension test: %s", report.toJSONString().utf8().legacyCStringPointer());
}

void WebExtensionController::testResult(bool result, String message, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("assertion"_s, sourceURL, lineNumber);
    report->setBoolean("result"_s, result);
    report->setString("message"_s, message.isEmpty() ? "(no message)"_s : message);
    logExtensionTestReport(report);
}

void WebExtensionController::testEqual(bool result, String expectedValue, String actualValue, String message, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("equality"_s, sourceURL, lineNumber);
    report->setBoolean("result"_s, result);
    report->setString("expected"_s, expectedValue);
    report->setString("actual"_s, actualValue);
    report->setString("message"_s, message.isEmpty() ? "Expected equality of these values"_s : message);
    logExtensionTestReport(report);
}

void WebExtensionController::testLogMessage(String message, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("log"_s, sourceURL, lineNumber);
    report->setString("message"_s, message.isEmpty() ? "(no message)"_s : message);
    logExtensionTestReport(report);
}

void WebExtensionController::testSentMessage(String message, String argument, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("message"_s, sourceURL, lineNumber);
    report->setString("message"_s, message);
    report->setString("argumentJSON"_s, argument);
    logExtensionTestReport(report);
}

void WebExtensionController::testAdded(String testName, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("added"_s, sourceURL, lineNumber);
    report->setString("name"_s, testName);
    logExtensionTestReport(report);
}

void WebExtensionController::testStarted(String testName, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("started"_s, sourceURL, lineNumber);
    report->setString("name"_s, testName);
    logExtensionTestReport(report);
}

void WebExtensionController::testFinished(String testName, bool result, String message, String sourceURL, unsigned lineNumber)
{
    auto report = extensionTestReport("finished"_s, sourceURL, lineNumber);
    report->setBoolean("result"_s, result);
    report->setString("name"_s, testName.isEmpty() ? "(no test name)"_s : testName);
    report->setString("message"_s, message.isEmpty() ? "(no message)"_s : message);
    logExtensionTestReport(report);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
