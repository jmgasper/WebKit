/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionWindowQueryParser.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
namespace WebKit {
std::expected<WebExtensionWindowQuery, String> parseWebExtensionWindowQuery(const JSON::Value* value, WebExtensionWindowQueryKind kind)
{
    WebExtensionWindowQuery result;
    if (!value)
        return result;
    RefPtr object = value->asObject();
    if (!object)
        return std::unexpected("window options must be an object"_s);
    for (auto& key : object->keys()) {
        Ref entry = object->getValue(key).releaseNonNull();
        if (key == "populate"_s && kind == WebExtensionWindowQueryKind::Get) {
            auto populate = entry->asBoolean();
            if (!populate)
                return std::unexpected("populate must be a boolean"_s);
            result.populate = *populate ? WebExtensionWindow::PopulateTabs::Yes : WebExtensionWindow::PopulateTabs::No;
        } else if (key == "windowTypes"_s) {
            RefPtr types = entry->asArray();
            if (!types || !types->length())
                return std::unexpected("windowTypes must be a non-empty array"_s);
            result.types = { };
            for (auto& type : *types) {
                auto name = type->asString();
                if (name == "normal"_s)
                    result.types.add(WebExtensionWindowTypeFilter::Normal);
                else if (name == "popup"_s)
                    result.types.add(WebExtensionWindowTypeFilter::Popup);
                else
                    return std::unexpected("windowTypes must contain only 'normal' or 'popup'"_s);
            }
        } else
            return std::unexpected("unsupported window option"_s);
    }
    return result;
}
}
#endif
