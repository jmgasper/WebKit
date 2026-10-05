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
#include "JSWebExtensionTabParameters.h"

#if ENABLE(WK_WEB_EXTENSIONS)

#include "JSWebExtensionString.h"
#include "Protected.h"
#include "WebExtensionTabParameters.h"

namespace WebKit {

JSValueRef toWebAPI(JSContextRef context, const WebExtensionTabParameters& parameters)
{
    auto globalContext = JSContextGetGlobalContext(context);
    Protected<JSObjectRef> result(globalContext, JSObjectMake(context, nullptr, nullptr));
    auto set = [&](ASCIILiteral key, JSValueRef value) {
        JSObjectSetProperty(context, result.get(), toJSString(key).get(), value, kJSPropertyAttributeNone, nullptr);
    };
    auto stringValue = [&](const String& value) {
        return JSValueMakeString(context, toJSString(value).get());
    };

    if (parameters.identifier)
        set("id"_s, JSValueMakeNumber(context, toWebAPI(parameters.identifier.value())));
    if (parameters.url)
        set("url"_s, stringValue(parameters.url->string()));
    if (parameters.title)
        set("title"_s, stringValue(parameters.title.value()));
    if (parameters.windowIdentifier)
        set("windowId"_s, JSValueMakeNumber(context, toWebAPI(parameters.windowIdentifier.value())));
    if (parameters.index)
        set("index"_s, JSValueMakeNumber(context, parameters.index.value() != notFound ? static_cast<double>(parameters.index.value()) : std::numeric_limits<double>::quiet_NaN()));
#if PLATFORM(COCOA)
    if (parameters.size) {
        set("width"_s, JSValueMakeNumber(context, parameters.size->width()));
        set("height"_s, JSValueMakeNumber(context, parameters.size->height()));
    }
#endif
    if (parameters.parentTabIdentifier)
        set("openerTabId"_s, JSValueMakeNumber(context, toWebAPI(parameters.parentTabIdentifier.value())));
    if (parameters.active)
        set("active"_s, JSValueMakeBoolean(context, parameters.active.value()));
    if (parameters.selected) {
        set("selected"_s, JSValueMakeBoolean(context, parameters.selected.value()));
        set("highlighted"_s, JSValueMakeBoolean(context, parameters.selected.value()));
    }
    if (parameters.pinned)
        set("pinned"_s, JSValueMakeBoolean(context, parameters.pinned.value()));
    if (parameters.audible)
        set("audible"_s, JSValueMakeBoolean(context, parameters.audible.value()));
    if (parameters.discarded)
        set("discarded"_s, JSValueMakeBoolean(context, parameters.discarded.value()));
    if (parameters.muted) {
        Protected<JSObjectRef> info(globalContext, JSObjectMake(context, nullptr, nullptr));
        JSObjectSetProperty(context, info.get(), toJSString("muted"_s).get(), JSValueMakeBoolean(context, parameters.muted.value()), kJSPropertyAttributeNone, nullptr);
        set("mutedInfo"_s, info.get());
    }
    if (parameters.loading)
        set("status"_s, stringValue(parameters.loading.value() ? "loading"_s : "complete"_s));
    if (parameters.privateBrowsing)
        set("incognito"_s, JSValueMakeBoolean(context, parameters.privateBrowsing.value()));
#if PLATFORM(HAIKU)
    if (parameters.cookieStoreIdentifier && !parameters.cookieStoreIdentifier->isEmpty())
        set("cookieStoreId"_s, stringValue(*parameters.cookieStoreIdentifier));
#endif
    if (parameters.readerModeAvailable)
        set("isArticle"_s, JSValueMakeBoolean(context, parameters.readerModeAvailable.value()));
    if (parameters.showingReaderMode)
        set("isInReaderMode"_s, JSValueMakeBoolean(context, parameters.showingReaderMode.value()));

    return result.get();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
