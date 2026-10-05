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

#if ENABLE(WK_WEB_EXTENSIONS)

#include <optional>
#include <wtf/OptionSet.h>
#include <wtf/text/WTFString.h>

namespace WebKit {

// Values retain the Cocoa API's modifier ABI.
enum class WebExtensionCommandModifier : uint32_t {
    Shift = 1 << 17,
    Control = 1 << 18,
    Option = 1 << 19,
    Command = 1 << 20
};

#if PLATFORM(HAIKU)
enum class WebExtensionCommandShortcutSource : bool { Manifest, Update };
struct WebExtensionCommandShortcut {
    String activationKey;
    OptionSet<WebExtensionCommandModifier> modifiers;
};

std::optional<WebExtensionCommandShortcut> parseWebExtensionCommandShortcut(const String&, WebExtensionCommandShortcutSource = WebExtensionCommandShortcutSource::Manifest);
String webExtensionCommandShortcutString(const String& activationKey, OptionSet<WebExtensionCommandModifier>);
bool isWebExtensionCommandActivationKey(const String&);
#endif

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
