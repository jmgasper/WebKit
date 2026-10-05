/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebExtensionCommandSettings.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionCommandShortcut.h"

namespace WebKit {

static constexpr auto commandOverridesStateKey = "CommandOverrides"_s;

bool isWebExtensionActionCommandIdentifier(const String& name)
{
    return name == "_execute_action"_s || name == "_execute_browser_action"_s
        || name == "_execute_page_action"_s || name == "_execute_sidebar_action"_s;
}

std::expected<WebExtensionCommandUpdate, String> parseWebExtensionCommandUpdate(const JSON::Value* details)
{
    RefPtr object = details ? details->asObject() : nullptr;
    if (!object || !object->getValue("name"_s))
        return makeUnexpected("details must contain a command name"_s);
    for (auto& key : object->keys()) {
        if ((key != "name"_s && key != "description"_s && key != "shortcut"_s) || object->getValue(key)->type() != JSON::Value::Type::String)
            return makeUnexpected("expected name, description and shortcut strings"_s);
    }
    WebExtensionCommandUpdate result { object->getString("name"_s), std::nullopt, std::nullopt };
    if (object->getValue("description"_s))
        result.description = object->getString("description"_s);
    if (object->getValue("shortcut"_s)) {
        auto parsed = parseWebExtensionCommandShortcut(object->getString("shortcut"_s), WebExtensionCommandShortcutSource::Update);
        if (!parsed)
            return makeUnexpected("invalid shortcut"_s);
        result.shortcut = webExtensionCommandShortcutString(parsed->activationKey, parsed->modifiers);
    }
    return result;
}

RefPtr<JSON::Object> webExtensionCommandOverride(const JSON::Object* state, const String& name)
{
    RefPtr overrides = state ? state->getObject(commandOverridesStateKey) : nullptr;
    if (!overrides)
        return nullptr;
    RefPtr saved = overrides->getObject(name);
    if (!saved && name == "_execute_action"_s)
        saved = overrides->getObject("_execute_browser_action"_s);
    return saved;
}

RefPtr<JSON::Object> webExtensionCommandStateWithUpdate(const JSON::Object* original, const String& name, const std::optional<WebExtensionCommandUpdate>& update)
{
    RefPtr<JSON::Object> state;
    if (original) {
        RefPtr copy = JSON::Value::parseJSON(original->toJSONString());
        state = copy ? copy->asObject() : nullptr;
    } else
        state = JSON::Object::create();
    if (!state)
        return nullptr;
    RefPtr overrides = state->getObject(commandOverridesStateKey);
    if (!overrides)
        overrides = JSON::Object::create();
    if (update) {
        RefPtr previous = webExtensionCommandOverride(state.get(), name);
        // Only persist known, valid fields. Malformed saved data cannot invent commands.
        auto saved = JSON::Object::create();
        if (previous) {
            if (RefPtr description = previous->getValue("description"_s); description && description->type() == JSON::Value::Type::String)
                saved->setString("description"_s, description->asString());
            if (RefPtr shortcut = previous->getValue("shortcut"_s); shortcut && shortcut->type() == JSON::Value::Type::String) {
                auto parsed = parseWebExtensionCommandShortcut(shortcut->asString(), WebExtensionCommandShortcutSource::Update);
                if (parsed)
                    saved->setString("shortcut"_s, webExtensionCommandShortcutString(parsed->activationKey, parsed->modifiers));
            }
        }
        if (update->description)
            saved->setString("description"_s, *update->description);
        if (update->shortcut)
            saved->setString("shortcut"_s, *update->shortcut);
        overrides->setObject(name, WTF::move(saved));
    } else
        overrides->remove(name);
    if (name == "_execute_action"_s)
        overrides->remove("_execute_browser_action"_s);
    state->setObject(commandOverridesStateKey, overrides.releaseNonNull());
    return state;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
