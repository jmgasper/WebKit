/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionWebRequestDecisionHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <cmath>
#include <wtf/text/MakeString.h>

namespace WebKit {
namespace {
constexpr size_t maximumReplyCharacters = 1024 * 1024;
constexpr size_t maximumHeaderCount = 1024;
constexpr size_t maximumHeaderBytes = 256 * 1024;

bool validHeaderName(const String& name)
{
    if (name.isEmpty())
        return false;
    constexpr auto punctuation = "!#$%&'*+-.^_`|~"_s;
    for (unsigned index = 0; index < name.length(); ++index) {
        auto character = name[index];
        if ((character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9') || StringView { punctuation }.contains(character))
            continue;
        return false;
    }
    return true;
}

std::expected<Vector<WebExtensionWebRequestHeaderHaiku>, String> parseHeaders(JSON::Value& value)
{
    RefPtr array = value.asArray();
    if (!array || array->length() > maximumHeaderCount)
        return makeUnexpected("A webRequest header replacement must be an array of at most 1024 entries."_s);
    Vector<WebExtensionWebRequestHeaderHaiku> result;
    size_t totalBytes = 0;
    for (auto& entry : *array) {
        RefPtr object = entry->asObject();
        if (!object)
            return makeUnexpected("Each webRequest header must be an object."_s);
        auto name = object->getString("name"_s);
        if (!validHeaderName(name))
            return makeUnexpected("A webRequest header has an invalid name."_s);
        RefPtr stringValue = object->getValue("value"_s);
        RefPtr binaryValue = object->getValue("binaryValue"_s);
        if (!!stringValue == !!binaryValue)
            return makeUnexpected("A webRequest header needs exactly one of value or binaryValue."_s);
        Vector<uint8_t> bytes;
        if (stringValue) {
            auto text = stringValue->asString();
            if (text.isNull())
                return makeUnexpected("A webRequest header value must be a string."_s);
            auto utf8 = text.utf8();
            if (utf8.length() > maximumHeaderBytes)
                return makeUnexpected("The webRequest header replacement exceeds its byte limit."_s);
            bytes.append(utf8.span());
        } else {
            RefPtr binary = binaryValue->asArray();
            if (!binary || binary->length() > maximumHeaderBytes)
                return makeUnexpected("A binary webRequest header value must be an array of bytes."_s);
            for (auto& element : *binary) {
                auto number = element->asDouble();
                if (!number || !std::isfinite(*number) || *number < 0 || *number > 255 || std::floor(*number) != *number)
                    return makeUnexpected("A binary webRequest header contains a non-byte value."_s);
                bytes.append(static_cast<uint8_t>(*number));
            }
        }
        for (auto byte : bytes) {
            if (!byte || byte == '\r' || byte == '\n')
                return makeUnexpected("A webRequest header value contains a prohibited line break or NUL."_s);
        }
        if (name.length() > maximumHeaderBytes - totalBytes)
            return makeUnexpected("The webRequest header replacement exceeds its byte limit."_s);
        totalBytes += name.length();
        if (bytes.size() > maximumHeaderBytes - totalBytes)
            return makeUnexpected("The webRequest header replacement exceeds its byte limit."_s);
        totalBytes += bytes.size();
        result.append({ WTF::move(name), WTF::move(bytes) });
    }
    return result;
}
}

std::expected<WebExtensionWebRequestDecisionHaiku, String> parseWebExtensionWebRequestDecisionHaiku(const String& input)
{
    if (input.length() > maximumReplyCharacters)
        return makeUnexpected("The webRequest blocking reply exceeds its size limit."_s);
    RefPtr parsed = JSON::Value::parseJSON(input);
    if (!parsed)
        return makeUnexpected("The webRequest blocking reply is not valid JSON."_s);
    WebExtensionWebRequestDecisionHaiku result;
    if (parsed->isNull())
        return result;
    RefPtr object = parsed->asObject();
    if (!object)
        return makeUnexpected("The webRequest blocking reply must be an object."_s);
    if (RefPtr value = object->getValue("cancel"_s)) {
        if (object->size() != 1 || !value->asBoolean().has_value())
            return makeUnexpected("The cancel field must be a boolean and cannot be combined with other reply fields."_s);
        result.cancel = *value->asBoolean();
    }
    if (RefPtr value = object->getValue("redirectUrl"_s)) {
        auto text = value->asString();
        URL url { text };
        if (text.isNull() || !url.isValid())
            return makeUnexpected("The webRequest redirect URL must be a valid absolute URL."_s);
        result.redirectURL = WTF::move(url);
    }
    RefPtr requestHeaders = object->getValue("requestHeaders"_s);
    RefPtr responseHeaders = object->getValue("responseHeaders"_s);
    if (requestHeaders && responseHeaders)
        return makeUnexpected("A webRequest reply cannot replace request and response headers together."_s);
    if (requestHeaders || responseHeaders) {
        auto headers = parseHeaders(requestHeaders ? *requestHeaders : *responseHeaders);
        if (!headers)
            return makeUnexpected(headers.error());
        if (requestHeaders)
            result.requestHeaders = WTF::move(*headers);
        else
            result.responseHeaders = WTF::move(*headers);
    }
    if (RefPtr value = object->getValue("authCredentials"_s)) {
        RefPtr credentials = value->asObject();
        if (!credentials)
            return makeUnexpected("The webRequest authentication credentials must be an object."_s);
        auto username = credentials->getString("username"_s);
        auto password = credentials->getString("password"_s);
        if (username.isNull() || password.isNull())
            return makeUnexpected("The webRequest authentication username and password must be strings."_s);
        result.authCredentials = WebExtensionWebRequestCredentialsHaiku { WTF::move(username), WTF::move(password) };
    }
    // Chromium ignores unknown properties. Known fields still undergo complete
    // validation even when their eventual event phase does not consume them.
    return result;
}
}
#endif
