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
#include "WebExtensionCommandShortcut.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <wtf/ASCIICType.h>
#include <wtf/text/StringBuilder.h>
#include <wtf/text/StringToIntegerConversion.h>

namespace WebKit {

struct CommandSpecialKey { ASCIILiteral name; char16_t value; };
static constexpr CommandSpecialKey commandSpecialKeys[] {
    { "Comma"_s, ',' }, { "Period"_s, '.' }, { "Space"_s, ' ' },
    { "Insert"_s, 0xF727 }, { "Delete"_s, 0xF728 }, { "Home"_s, 0xF729 },
    { "End"_s, 0xF72B }, { "PageUp"_s, 0xF72C }, { "PageDown"_s, 0xF72D },
    { "Up"_s, 0xF700 }, { "Down"_s, 0xF701 }, { "Left"_s, 0xF702 }, { "Right"_s, 0xF703 }
};

static bool isCommandMediaKey(const String& key)
{
    return key == "MediaNextTrack"_s || key == "MediaPlayPause"_s || key == "MediaPrevTrack"_s || key == "MediaStop"_s;
}

bool isWebExtensionCommandActivationKey(const String& key)
{
    if (key.isEmpty() || isCommandMediaKey(key))
        return true;
    if (key.length() != 1)
        return false;
    auto character = key[0];
    if (isASCIIAlphanumeric(character) || (character >= 0xF704 && character <= 0xF716))
        return true;
    for (auto& special : commandSpecialKeys) {
        if (character == special.value)
            return true;
    }
    return false;
}

std::optional<WebExtensionCommandShortcut> parseWebExtensionCommandShortcut(const String& shortcut, WebExtensionCommandShortcutSource source)
{
    if (shortcut.isEmpty())
        return WebExtensionCommandShortcut { emptyString(), { } };
    if (shortcut.length() > 32)
        return std::nullopt;
    // Check the original spelling: split() would hide doubled/trailing separators.
    auto parts = shortcut.splitAllowingEmptyEntries('+');
    if (parts.size() > 3)
        return std::nullopt;
    String key = parts.takeLast();
    if (key.isEmpty())
        return std::nullopt;
    bool functionKey = false;
    if (key.length() > 1 && key[0] == 'F') {
        auto number = parseInteger<unsigned>(StringView(key).substring(1));
        if (!number || !*number || *number > (source == WebExtensionCommandShortcutSource::Manifest ? 12u : 19u)
            || key != makeString('F', *number))
            return std::nullopt;
        key = makeString(static_cast<char16_t>(0xF703 + *number));
        functionKey = true;
    } else if (isCommandMediaKey(key)) {
        if (!parts.isEmpty())
            return std::nullopt;
        return WebExtensionCommandShortcut { key, { } };
    } else if (key.length() == 1) {
        if (!isASCIIAlphanumeric(key[0]))
            return std::nullopt;
        key = key.convertToASCIILowercase();
    } else {
        bool found = false;
        for (auto& special : commandSpecialKeys) {
            if (key == special.name) {
                key = makeString(special.value);
                found = true;
                break;
            }
        }
        if (!found)
            return std::nullopt;
    }
    if (!functionKey && !parts.isEmpty() && parts[0] == "Shift"_s)
        return std::nullopt;
    OptionSet<WebExtensionCommandModifier> modifiers;
    for (auto& part : parts) {
        std::optional<WebExtensionCommandModifier> modifier;
        if (part == "Ctrl"_s)
            modifier = WebExtensionCommandModifier::Control;
        else if (part == "Alt"_s)
            modifier = WebExtensionCommandModifier::Option;
        else if (part == "Shift"_s)
            modifier = WebExtensionCommandModifier::Shift;
        if (!modifier || modifiers.contains(*modifier))
            return std::nullopt;
        modifiers.add(*modifier);
    }
    if (!functionKey && !modifiers.containsAny({ WebExtensionCommandModifier::Control, WebExtensionCommandModifier::Option }))
        return std::nullopt;
    return WebExtensionCommandShortcut { key, modifiers };
}

String webExtensionCommandShortcutString(const String& key, OptionSet<WebExtensionCommandModifier> modifiers)
{
    if (key.isEmpty() || !isWebExtensionCommandActivationKey(key))
        return emptyString();
    if (!modifiers.containsOnly({ WebExtensionCommandModifier::Control, WebExtensionCommandModifier::Option, WebExtensionCommandModifier::Shift }))
        return emptyString();
    if (isCommandMediaKey(key))
        return modifiers.isEmpty() ? key : emptyString();
    StringBuilder result;
    if (modifiers.contains(WebExtensionCommandModifier::Control))
        result.append("Ctrl+"_s);
    if (modifiers.contains(WebExtensionCommandModifier::Option))
        result.append("Alt+"_s);
    if (modifiers.contains(WebExtensionCommandModifier::Shift))
        result.append("Shift+"_s);
    auto character = key[0];
    if (character >= 0xF704 && character <= 0xF716)
        result.append('F', character - 0xF703);
    else {
        bool found = false;
        for (auto& special : commandSpecialKeys) {
            if (character == special.value) {
                result.append(special.name);
                found = true;
                break;
            }
        }
        if (!found)
            result.append(key.convertToASCIIUppercase());
    }
    // Individual native setters can temporarily hold an incomplete combination.
    // Report it as unassigned until it forms a valid shortcut.
    auto text = result.toString();
    return parseWebExtensionCommandShortcut(text, WebExtensionCommandShortcutSource::Update) ? text : emptyString();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
