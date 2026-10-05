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
#include "WebExtensionNavigationParameters.h"

#if ENABLE(WK_WEB_EXTENSIONS)

namespace WebKit {

std::expected<WebExtensionTabIdentifier, String> parseWebExtensionNavigationTab(const JSON::Value* details)
{
    RefPtr object = details ? details->asObject() : nullptr;
    auto number = object ? object->getDouble("tabId"_s) : std::nullopt;
    if (!number)
        return makeUnexpected("The 'details' value must be an object with a numeric 'tabId'."_s);
    auto identifier = toWebExtensionTabIdentifier(*number);
    if (!isValid(identifier))
        return makeUnexpected("The 'tabId' value is not a tab identifier."_s);
    return *identifier;
}

std::expected<WebExtensionFrameIdentifier, String> parseWebExtensionNavigationFrame(const JSON::Value* details)
{
    RefPtr object = details ? details->asObject() : nullptr;
    auto number = object ? object->getDouble("frameId"_s) : std::nullopt;
    if (!number)
        return makeUnexpected("The 'details' value must be an object with a numeric 'frameId'."_s);
    auto identifier = toWebExtensionFrameIdentifier(*number);
    if (!isValid(identifier))
        return makeUnexpected("The 'frameId' value is not a frame identifier."_s);
    return *identifier;
}

Ref<JSON::Object> webExtensionNavigationFrameJSON(const WebExtensionFrameParameters& frame)
{
    auto result = JSON::Object::create();
    result->setBoolean("errorOccurred"_s, frame.errorOccurred);
    result->setDouble("parentFrameId"_s, toWebAPI(frame.parentFrameIdentifier));
    result->setString("url"_s, frame.url ? frame.url->string() : emptyString());
    if (frame.frameIdentifier)
        result->setDouble("frameId"_s, toWebAPI(*frame.frameIdentifier));
    if (frame.documentIdentifier)
        result->setString("documentId"_s, frame.documentIdentifier->toString());
    return result;
}

RefPtr<JSON::Object> webExtensionNavigationEventJSON(WebExtensionTabIdentifier tabID, const WebExtensionFrameParameters& frame, WallTime timestamp)
{
    if (!frame.url || !frame.frameIdentifier)
        return nullptr;
    auto result = JSON::Object::create();
    result->setString("url"_s, frame.url->string());
    result->setDouble("tabId"_s, toWebAPI(tabID));
    result->setDouble("frameId"_s, toWebAPI(*frame.frameIdentifier));
    result->setDouble("parentFrameId"_s, toWebAPI(frame.parentFrameIdentifier));
    result->setDouble("timeStamp"_s, floor(timestamp.approximate<WallTime>().secondsSinceEpoch().milliseconds()));
    if (frame.documentIdentifier)
        result->setString("documentId"_s, frame.documentIdentifier->toString());
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
