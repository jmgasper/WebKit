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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionEventListenerType.h"
#include <cmath>
#include <limits>
#include <wtf/EnumTraits.h>
#include <wtf/HashCountedSet.h>
#include <wtf/JSONValues.h>

namespace WebKit {

inline constexpr auto backgroundContentEventListenersKeyHaiku = "BackgroundContentEventListeners"_s;
inline constexpr auto backgroundContentEventListenersVersionKeyHaiku = "BackgroundContentEventListenersVersion"_s;
// Native privacy and downloads.onErased add events only on Haiku; older backgrounds must register them on their next load.
inline constexpr unsigned currentBackgroundContentListenerStateVersionHaiku = 6;

using BackgroundListenerCountsHaiku = HashCountedSet<WebExtensionEventListenerType>;

inline std::optional<BackgroundListenerCountsHaiku> readBackgroundListenersHaiku(const JSON::Object& state)
{
    if (state.getDouble(backgroundContentEventListenersVersionKeyHaiku) != currentBackgroundContentListenerStateVersionHaiku)
        return std::nullopt;
    RefPtr saved = state.getObject(backgroundContentEventListenersKeyHaiku);
    if (!saved)
        return std::nullopt;

    BackgroundListenerCountsHaiku result;
    constexpr auto names = WTF::enumNames<WebExtensionEventListenerType>();
    for (auto& entry : *saved) {
        auto count = entry.value->asDouble();
        if (!count || !std::isfinite(*count) || *count < 1 || *count > std::numeric_limits<unsigned>::max() || std::trunc(*count) != *count)
            return std::nullopt;

        bool found = false;
        for (size_t i = 1; i < names.size(); ++i) {
            if (names[i].isEmpty() || entry.key != names[i])
                continue;
            result.add(static_cast<WebExtensionEventListenerType>(i), static_cast<unsigned>(*count));
            found = true;
            break;
        }
        if (!found)
            return std::nullopt;
    }
    return result;
}

inline void clearBackgroundListenersHaiku(JSON::Object& state)
{
    state.remove(backgroundContentEventListenersKeyHaiku);
    state.remove(backgroundContentEventListenersVersionKeyHaiku);
}

inline bool saveBackgroundListenersHaiku(JSON::Object& state, const BackgroundListenerCountsHaiku& listeners)
{
    // Store enum names, not values: feature-gated events can shift numeric IDs
    // without changing the saved format version. Counts preserve removals of
    // individual registrations after restoring the listener set.
    Ref saved = JSON::Object::create();
    constexpr auto names = WTF::enumNames<WebExtensionEventListenerType>();
    for (size_t i = 1; i < names.size(); ++i) {
        if (names[i].isEmpty())
            continue;
        auto count = listeners.count(static_cast<WebExtensionEventListenerType>(i));
        if (count)
            saved->setDouble(names[i], count);
    }
    RefPtr previous = state.getObject(backgroundContentEventListenersKeyHaiku);
    if (state.getDouble(backgroundContentEventListenersVersionKeyHaiku) == currentBackgroundContentListenerStateVersionHaiku
        && previous && previous->toJSONString() == saved->toJSONString())
        return false;

    state.setObject(backgroundContentEventListenersKeyHaiku, WTF::move(saved));
    state.setInteger(backgroundContentEventListenersVersionKeyHaiku, currentBackgroundContentListenerStateVersionHaiku);
    return true;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
