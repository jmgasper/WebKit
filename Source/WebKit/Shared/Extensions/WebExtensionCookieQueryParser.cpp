/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionCookieQueryParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionCookieStoreIdentifier.h"
namespace WebKit {
std::expected<WebExtensionCookieQuery, String> parseWebExtensionCookieQuery(const JSON::Value* value, WebExtensionCookieQueryKind kind)
{
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return std::unexpected("cookie details must be an object"_s);
    WebExtensionCookieQuery result;
    for (auto& key : object->keys()) {
        Ref field = object->getValue(key).releaseNonNull();
        if (kind == WebExtensionCookieQueryKind::GetAll && (key == "secure"_s || key == "session"_s)) {
            auto flag = field->asBoolean();
            if (!flag)
                return std::unexpected("cookie filter flags must be booleans"_s);
            if (key == "secure"_s)
                result.filter.secure = *flag;
            else
                result.filter.session = *flag;
            continue;
        }
        bool common = key == "name"_s || key == "url"_s || key == "storeId"_s;
        if (!common && (kind != WebExtensionCookieQueryKind::GetAll || (key != "domain"_s && key != "path"_s)))
            return std::unexpected("unsupported cookie query field"_s);
        if (field->type() != JSON::Value::Type::String)
            return std::unexpected("cookie query text must be a string"_s);
        auto text = field->asString();
        if (text.contains('\0'))
            return std::unexpected("cookie query text must not contain null characters"_s);
        if (key == "name"_s)
            result.filter.name = text;
        else if (key == "url"_s) {
            result.url = URL { text };
            if (!result.url.isValid() || !result.url.protocolIsInHTTPFamily() || result.url.host().isEmpty())
                return std::unexpected("cookie URL must be a valid HTTP or HTTPS URL"_s);
        } else if (key == "storeId"_s) {
            result.session = parseWebExtensionCookieStoreIdentifier(text);
            if (!result.session)
                return std::unexpected("invalid cookie store identifier"_s);
        } else if (key == "path"_s)
            result.filter.path = text;
        else {
            if (text.startsWith('.'))
                text = text.substring(1);
            if (text.contains(':') && text.startsWith('[') && text.endsWith(']'))
                text = text.substring(1, text.length() - 2);
            result.filter.domain = text;
        }
    }
    if (kind == WebExtensionCookieQueryKind::Get && (!result.filter.name || !result.url.isValid()))
        return std::unexpected("cookie get requires name and url"_s);
    return result;
}
}
#endif
