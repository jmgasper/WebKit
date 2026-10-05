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
#include "WebExtensionContext.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionCommand.h"
#include "WebExtensionCommandParameters.h"
#include "WebExtensionCommandSettings.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionStateHaiku.h"
#include "WebExtensionTab.h"
#include "WebExtensionUtilities.h"
#include "WebExtensionWindow.h"

namespace WebKit {

const WebExtensionContext::CommandsVector& WebExtensionContext::commands()
{
    if (m_populatedCommands)
        return m_commands;
    for (auto& data : protect(extension())->commands()) {
        auto command = WebExtensionCommand::create(*this, data);
        RefPtr saved = webExtensionCommandOverride(m_state.get(), data.identifier);
        if (saved) {
            if (RefPtr description = saved->getValue("description"_s); description && description->type() == JSON::Value::Type::String)
                command->setDescription(description->asString());
            if (RefPtr shortcut = saved->getValue("shortcut"_s); shortcut && shortcut->type() == JSON::Value::Type::String)
                command->setShortcut(shortcut->asString(), WebExtensionCommand::SuppressEvents::Yes);
        }
        m_commands.append(WTF::move(command));
    }
    m_populatedCommands = true;
    return m_commands;
}

WebExtensionCommand* WebExtensionContext::command(const String& identifier)
{
    for (auto& command : commands()) {
        if (command->identifier() == identifier)
            return command.ptr();
    }
    return nullptr;
}

void WebExtensionContext::performCommand(WebExtensionCommand& target, UserTriggered userTriggered)
{
    if (!isLoaded() || target.extensionContext() != this || command(target.identifier()) != &target)
        return;
    // Resolve the real frontmost window first. An inaccessible private window
    // must not redirect a shortcut to a different, accessible browser window.
    RefPtr window = frontmostWindow(IgnoreExtensionAccess::Yes);
    if (window && (!isValidWindow(*window) || !window->extensionHasAccess()))
        return;
    RefPtr tab = window ? window->activeTab() : nullptr;
    performCommand(target, tab.get(), userTriggered);
}

void WebExtensionContext::performCommand(WebExtensionCommand& target, WebExtensionTab* tab, UserTriggered userTriggered)
{
    if (!isLoaded() || target.extensionContext() != this || command(target.identifier()) != &target)
        return;
    if (tab && (!isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess()))
        return;
    Ref protectedCommand { target };
    RefPtr protectedTab = tab;
    if (target.isActionCommand()) {
        performAction(tab, userTriggered);
        return;
    }
    if (tab && userTriggered == UserTriggered::Yes)
        userGesturePerformed(*tab);
    fireCommandEventIfNeeded(target, tab);
}

void WebExtensionContext::resetCommands()
{
    m_commands.clear();
    m_populatedCommands = false;
}

bool WebExtensionContext::isCommandsMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message);
}

void WebExtensionContext::commandsGetAll(CompletionHandler<void(Vector<WebExtensionCommandParameters>)>&& completionHandler)
{
    completionHandler(WTF::map(commands(), [](auto& command) { return command->parameters(); }));
}

std::expected<void, WebExtensionError> WebExtensionContext::saveCommandState(Ref<JSON::Object>&& state)
{
    if (!isLoaded())
        return toWebExtensionError("commands"_s, nullString(), "extension is not loaded"_s);
    if (storageIsPersistent()) {
        if (auto result = writeWebExtensionStateHaiku(stateFilePath(), state.get()); !result)
            return toWebExtensionError("commands"_s, nullString(), "could not save command settings"_s);
    }
    m_state = WTF::move(state);
    return { };
}

void WebExtensionContext::commandsUpdate(const String& name, const std::optional<String>& description, const std::optional<String>& shortcut, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr target = command(name);
    if (!target) {
        completionHandler(toWebExtensionError("commands.update()"_s, nullString(), "command not found"_s));
        return;
    }
    auto parsed = shortcut ? parseWebExtensionCommandShortcut(*shortcut, WebExtensionCommandShortcutSource::Update) : std::nullopt;
    if (shortcut && !parsed) {
        completionHandler(toWebExtensionError("commands.update()"_s, "shortcut"_s, "invalid shortcut"_s));
        return;
    }
    String normalized = parsed ? webExtensionCommandShortcutString(parsed->activationKey, parsed->modifiers) : nullString();
    auto state = webExtensionCommandStateWithUpdate(m_state.get(), name, WebExtensionCommandUpdate { name, description, shortcut ? std::optional(normalized) : std::nullopt });
    if (!state) {
        completionHandler(toWebExtensionError("commands.update()"_s, nullString(), "could not copy command settings"_s));
        return;
    }
    auto result = saveCommandState(state.releaseNonNull());
    if (!result) {
        completionHandler(WTF::move(result));
        return;
    }
    // Validation and durable storage precede either in-memory mutation.
    if (description)
        target->setDescription(*description);
    if (shortcut)
        target->setShortcut(normalized);
    completionHandler({ });
}

void WebExtensionContext::commandsReset(const String& name, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    RefPtr target = command(name);
    if (!target) {
        completionHandler(toWebExtensionError("commands.reset()"_s, nullString(), "command not found"_s));
        return;
    }
    auto state = webExtensionCommandStateWithUpdate(m_state.get(), name, std::nullopt);
    if (!state) {
        completionHandler(toWebExtensionError("commands.reset()"_s, nullString(), "could not copy command settings"_s));
        return;
    }
    auto result = saveCommandState(state.releaseNonNull());
    if (!result) {
        completionHandler(WTF::move(result));
        return;
    }
    for (auto& data : protect(extension())->commands()) {
        if (data.identifier != name)
            continue;
        target->setDescription(data.description);
        target->setShortcut(webExtensionCommandShortcutString(data.activationKey, data.modifierFlags));
        break;
    }
    completionHandler({ });
}

void WebExtensionContext::fireCommandEventIfNeeded(const WebExtensionCommand& target, WebExtensionTab* tab)
{
    if (!isLoaded() || command(target.identifier()) != &target || isWebExtensionActionCommandIdentifier(target.identifier()))
        return;
    if (tab && (!isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess()))
        return;
    auto loadIdentifier = privilegedIdentifier();
    constexpr auto type = WebExtensionEventListenerType::CommandsOnCommand;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, target = Ref { target }, tab = RefPtr { tab }, loadIdentifier] {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier || command(target->identifier()) != target.ptr())
            return;
        if (tab && (!isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess()))
            return;
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchCommandsCommandEvent(target->identifier(), tab ? std::optional(tab->parameters()) : std::nullopt));
    });
}

void WebExtensionContext::fireCommandChangedEventIfNeeded(const WebExtensionCommand& target, const String& oldShortcut)
{
    if (!isLoaded() || command(target.identifier()) != &target)
        return;
    auto newShortcut = target.shortcutString();
    if (oldShortcut == newShortcut)
        return;
    auto loadIdentifier = privilegedIdentifier();
    constexpr auto type = WebExtensionEventListenerType::CommandsOnChanged;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, target = Ref { target }, loadIdentifier, oldShortcut, newShortcut] {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier || command(target->identifier()) != target.ptr())
            return;
        // Capture the committed transition, not a later value after another update.
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchCommandsChangedEvent(target->identifier(), oldShortcut, newShortcut));
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
