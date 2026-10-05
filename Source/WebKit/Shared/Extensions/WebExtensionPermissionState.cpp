/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionPermissionState.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <cmath>

namespace WebKit {

static constexpr auto stateKey = "PermissionGrants"_s;

std::expected<WebExtensionPermissionDetails, String> parseWebExtensionPermissionDetails(const JSON::Value* value)
{
    RefPtr object = value ? value->asObject() : nullptr;
    if (!object)
        return makeUnexpected("permissions must be an object"_s);
    for (auto& key : object->keys()) {
        if (key != "permissions"_s && key != "origins"_s)
            return makeUnexpected("unsupported permission detail field"_s);
    }
    WebExtensionPermissionDetails result;
    for (auto key : { "permissions"_s, "origins"_s }) {
        RefPtr member = object->getValue(key);
        if (!member)
            continue;
        RefPtr array = member->asArray();
        if (!array)
            return makeUnexpected("permissions and origins must be arrays of strings"_s);
        auto& target = key == "permissions"_s ? result.permissions : result.origins;
        for (auto& item : *array) {
            if (item->type() != JSON::Value::Type::String)
                return makeUnexpected("permissions and origins must contain only strings"_s);
            auto text = item->asString();
            if (text.isEmpty() || text.contains('\0'))
                return makeUnexpected("permission names and origins must be nonempty and contain no NUL"_s);
            target.add(WTF::move(text));
        }
    }
    return result;
}

std::expected<WebExtensionPermissionState, String> readWebExtensionPermissionState(const JSON::Object* original, WallTime now)
{
    WebExtensionPermissionState result;
    RefPtr value = original ? original->getValue(stateKey) : nullptr;
    if (!value)
        return result;
    RefPtr saved = value->asObject();
    if (!saved || saved->getDouble("version"_s) != 1)
        return makeUnexpected("unsupported or malformed saved permission state"_s);
    for (auto key : { "permissions"_s, "origins"_s }) {
        RefPtr grants = saved->getObject(key);
        if (!grants)
            return makeUnexpected("saved permission grants must be objects"_s);
        auto& target = key == "permissions"_s ? result.permissions : result.origins;
        for (auto& name : grants->keys()) {
            if (name.isEmpty() || name.contains('\0'))
                return makeUnexpected("invalid saved permission identifier"_s);
            Ref entry = grants->getValue(name).releaseNonNull();
            WallTime expiration = WallTime::infinity();
            if (entry->type() != JSON::Value::Type::Null) {
                auto seconds = entry->asDouble();
                if (!seconds || !std::isfinite(*seconds))
                    return makeUnexpected("invalid saved permission expiration"_s);
                expiration = WallTime::fromRawSeconds(*seconds);
            }
            if (expiration > now)
                target.set(name, expiration);
        }
    }
    return result;
}

std::expected<Ref<JSON::Object>, String> stateWithWebExtensionPermissions(const JSON::Object* original, const WebExtensionPermissionState& permissions)
{
    RefPtr<JSON::Object> state;
    if (original) {
        RefPtr parsed = JSON::Value::parseJSON(original->toJSONString());
        state = parsed ? parsed->asObject() : nullptr;
    } else
        state = JSON::Object::create();
    if (!state)
        return makeUnexpected("could not copy extension state"_s);
    auto saved = JSON::Object::create();
    saved->setInteger("version"_s, 1);
    for (auto key : { "permissions"_s, "origins"_s }) {
        const auto& source = key == "permissions"_s ? permissions.permissions : permissions.origins;
        auto grants = JSON::Object::create();
        for (auto& entry : source) {
            if (entry.key.isEmpty() || entry.key.contains('\0') || entry.value.isNaN()
                || entry.value == -WallTime::infinity())
                return makeUnexpected("invalid permission grant state"_s);
            if (entry.value == WallTime::infinity())
                grants->setValue(entry.key, JSON::Value::null());
            else
                grants->setDouble(entry.key, entry.value.secondsSinceEpoch().seconds());
        }
        saved->setObject(key, WTF::move(grants));
    }
    state->setObject(stateKey, WTF::move(saved));
    return state.releaseNonNull();
}

} // namespace WebKit

#endif
