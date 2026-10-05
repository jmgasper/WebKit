/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionAPIOffscreen.h"

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && !PLATFORM(COCOA)

#include "MessageSenderInlines.h"
#include "WebExtensionContextMessages.h"
#include "WebExtensionContextProxy.h"
#include "WebExtensionOffscreenDocumentParameters.h"
#include "WebExtensionUtilities.h"
#include "WebProcess.h"

// chrome.offscreen for ports without the Objective-C bindings. Documentation:
// https://developer.chrome.com/docs/extensions/reference/api/offscreen
// Argument types are checked here; the UI process validates the URL again and
// owns the single hidden document.

namespace WebKit {

void WebExtensionAPIOffscreen::createDocument(RefPtr<JSON::Value> details, Ref<WebExtensionCallbackHandler>&& callback, String& outExceptionString)
{
    RefPtr object = details ? details->asObject() : nullptr;
    if (!object) {
        outExceptionString = toErrorString(nullString(), "parameters"_s, "it must be an object"_s);
        return;
    }

    WebExtensionOffscreenDocumentParameters parameters;
    RefPtr url = object->getValue("url"_s);
    if (!url || url->asString().isEmpty()) {
        outExceptionString = toErrorString(nullString(), "url"_s, "it must be a non-empty string"_s);
        return;
    }
    parameters.url = url->asString();

    RefPtr justification = object->getValue("justification"_s);
    if (!justification || justification->type() != JSON::Value::Type::String) {
        outExceptionString = toErrorString(nullString(), "justification"_s, "it must be a string"_s);
        return;
    }
    parameters.justification = justification->asString();

    RefPtr reasons = object->getArray("reasons"_s);
    if (!reasons || !reasons->length()) {
        outExceptionString = toErrorString(nullString(), "reasons"_s, "it must be a non-empty array of strings"_s);
        return;
    }
    for (Ref reason : *reasons) {
        if (reason->type() != JSON::Value::Type::String) {
            outExceptionString = toErrorString(nullString(), "reasons"_s, "it must be a non-empty array of strings"_s);
            return;
        }
        parameters.reasons.append(reason->asString());
    }

    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::OffscreenCreateDocument(parameters), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPIOffscreen::closeDocument(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::OffscreenCloseDocument(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<void, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        callback->call();
    }, extensionContext().identifier());
}

void WebExtensionAPIOffscreen::hasDocument(Ref<WebExtensionCallbackHandler>&& callback)
{
    WebProcess::singleton().sendWithAsyncReply(Messages::WebExtensionContext::OffscreenHasDocument(), [protectedThis = Ref { *this }, callback = WTF::move(callback)](std::expected<bool, WebExtensionError>&& result) {
        if (!result) {
            callback->reportError(result.error());
            return;
        }
        callback->call(JSValueMakeBoolean(callback->globalContext(), result.value()));
    }, extensionContext().identifier());
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && !PLATFORM(COCOA)
