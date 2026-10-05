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
#include "WebExtensionCommand.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContext.h"
#include <wtf/RunLoop.h>

namespace WebKit {

void WebExtensionCommand::dispatchChangedEventSoonIfNeeded()
{
    if (!m_oldShortcut.isNull())
        return;
    RefPtr context = extensionContext();
    if (!context || !context->isLoaded())
        return;
    m_oldShortcut = shortcutString();
    auto loadIdentifier = context->privilegedIdentifier();
    RunLoop::mainSingleton().dispatch([this, protectedThis = Ref { *this }, loadIdentifier] {
        auto oldShortcut = std::exchange(m_oldShortcut, nullString());
        RefPtr context = extensionContext();
        if (!context || !context->isLoaded() || context->privilegedIdentifier() != loadIdentifier
            || context->command(identifier()) != this || oldShortcut == shortcutString())
            return;
        context->fireCommandChangedEventIfNeeded(*this, oldShortcut);
    });
}

bool WebExtensionCommand::setActivationKey(String key, SuppressEvents suppressEvents)
{
    if (!isWebExtensionCommandActivationKey(key))
        return false;
    if (suppressEvents == SuppressEvents::No && key != m_activationKey)
        dispatchChangedEventSoonIfNeeded();
    m_activationKey = key.length() == 1 ? key.convertToASCIILowercase() : key;
    return true;
}

bool WebExtensionCommand::setShortcut(const String& shortcut, SuppressEvents suppressEvents)
{
    auto parsed = parseWebExtensionCommandShortcut(shortcut, WebExtensionCommandShortcutSource::Update);
    if (!parsed)
        return false;
    if (suppressEvents == SuppressEvents::No && (parsed->activationKey != m_activationKey || parsed->modifiers != m_modifierFlags))
        dispatchChangedEventSoonIfNeeded();
    m_activationKey = parsed->activationKey;
    m_modifierFlags = parsed->modifiers;
    return true;
}

String WebExtensionCommand::shortcutString() const
{
    return webExtensionCommandShortcutString(m_activationKey, m_modifierFlags);
}

String WebExtensionCommand::userVisibleShortcut() const
{
    return shortcutString();
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
