/*
 * Copyright (C) 2023-2024 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebExtensionMenuItemContextParameters.h"
#include "WebExtensionMenuItemParser.h"
#include "WebExtensionMenuItemTree.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
namespace WebKit {
static bool hasMenuPermission(WebExtensionContext& context)
{
    return context.isLoaded() && (context.hasPermission(WebExtensionPermission::menus()) || context.hasPermission(WebExtensionPermission::contextMenus()));
}

bool WebExtensionContext::isMenusMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasMenuPermission(*this);
}

WebExtensionMenuItem* WebExtensionContext::menuItem(const String& identifier) const
{
    return identifier.isEmpty() ? nullptr : m_menuItems.get(identifier);
}

static bool hasMenuCallbackToken(const HashMap<String, WTF::UUID>& callbacks, const WTF::UUID& identifier)
{
    for (auto& token : callbacks.values()) {
        if (token == identifier)
            return true;
    }
    return false;
}

static bool hasLazyMenuBackground(WebExtensionContext& context)
{
    Ref extension = context.extension();
    return extension->hasBackgroundContent() && !extension->backgroundContentIsPersistent();
}

void WebExtensionContext::removeMenusClickHandlers(const Vector<WTF::UUID>& identifiers)
{
    if (identifiers.isEmpty())
        return;
    if (RefPtr controller = extensionController())
        controller->sendToAllProcesses(Messages::WebExtensionContextProxy::RemoveMenusClickHandlers(identifiers), privilegedIdentifier());
}

void WebExtensionContext::menusCreateWithCallback(const WebExtensionMenuItemParameters& parameters, bool explicitIdentifier, const std::optional<WTF::UUID>& token, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    bool lazyBackground = hasLazyMenuBackground(*this);
    if (!hasMenuPermission(*this) || (lazyBackground && (!explicitIdentifier || token)) || (token && (!token->isValid() || hasMenuCallbackToken(m_menuClickHandlerIdentifiers, *token)))) {
        completion(toWebExtensionError("menus.create()"_s, nullString(), lazyBackground ? "event pages and service workers require an explicit menu id and the onClicked event"_s : "menu permission or callback identifier is invalid"_s));
        return;
    }
    menusCreate(parameters, [this, token, identifier = parameters.identifier, completion = WTF::move(completion)](std::expected<void, WebExtensionError>&& result) mutable {
        if (result && token)
            m_menuClickHandlerIdentifiers.set(identifier, *token);
        completion(WTF::move(result));
    });
}

void WebExtensionContext::menusUpdateWithCallback(const String& identifier, const WebExtensionMenuItemParameters& parameters, bool callbackSpecified, const std::optional<WTF::UUID>& token, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    if (!hasMenuPermission(*this) || (token && (!callbackSpecified || hasLazyMenuBackground(*this) || !token->isValid() || hasMenuCallbackToken(m_menuClickHandlerIdentifiers, *token)))) {
        completion(toWebExtensionError("menus.update()"_s, nullString(), "menu permission or callback is invalid; event pages and service workers must use onClicked"_s));
        return;
    }
    menusUpdate(identifier, parameters, [this, token, callbackSpecified, identifier, newIdentifier = parameters.identifier.isNull() ? identifier : parameters.identifier, completion = WTF::move(completion)](std::expected<void, WebExtensionError>&& result) mutable {
        if (result) {
            std::optional<WTF::UUID> previous;
            auto old = m_menuClickHandlerIdentifiers.find(identifier);
            if (old != m_menuClickHandlerIdentifiers.end()) {
                previous = old->value;
                m_menuClickHandlerIdentifiers.remove(old);
            }
            auto current = callbackSpecified ? token : previous;
            if (current)
                m_menuClickHandlerIdentifiers.set(newIdentifier, *current);
            if (callbackSpecified && previous)
                removeMenusClickHandlers({ *previous });
        }
        completion(WTF::move(result));
    });
}

void WebExtensionContext::menusCreate(const WebExtensionMenuItemParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    constexpr auto api = "menus.create()"_s;
    auto validation = validateWebExtensionMenuItemParameters(parameters, WebExtensionMenuItemOperation::Create);
    if (!hasMenuPermission(*this) || !validation) {
        completion(toWebExtensionError(api, nullString(), validation ? "menu permission is not granted"_s : validation.error()));
        return;
    }
    if (m_menuItems.contains(parameters.identifier)) {
        completion(toWebExtensionError(api, nullString(), "identifier is already used"_s));
        return;
    }
    if (parameters.parentIdentifier) {
        RefPtr parent = menuItem(*parameters.parentIdentifier);
        if (!parent || parent->type() == WebExtensionMenuItemType::Separator) {
            completion(toWebExtensionError(api, nullString(), "parent menu item not found or cannot contain children"_s));
            return;
        }
    }
    Ref item = WebExtensionMenuItem::create(*this, parameters);
    m_menuItems.set(parameters.identifier, item.copyRef());
    if (!parameters.parentIdentifier)
        m_mainMenuItems.append(WTF::move(item));
    completion({ });
}

void WebExtensionContext::menusUpdate(const String& identifier, const WebExtensionMenuItemParameters& parameters, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    constexpr auto api = "menus.update()"_s;
    RefPtr item = menuItem(identifier);
    if (!hasMenuPermission(*this) || !item || !decodeWebExtensionMenuIdentifier(identifier)) {
        completion(toWebExtensionError(api, nullString(), "menu item not found or permission is not granted"_s));
        return;
    }
    auto validation = validateWebExtensionMenuItemParameters(parameters, WebExtensionMenuItemOperation::Update);
    if (!validation) {
        completion(toWebExtensionError(api, nullString(), validation.error()));
        return;
    }
    auto newIdentifier = parameters.identifier.isNull() ? identifier : parameters.identifier;
    if (newIdentifier != identifier && m_menuItems.contains(newIdentifier)) {
        completion(toWebExtensionError(api, nullString(), "identifier is already used"_s));
        return;
    }
    auto type = parameters.type.value_or(item->type());
    auto title = parameters.title.isNull() ? item->title() : parameters.title;
    if ((type != WebExtensionMenuItemType::Separator && title.isEmpty()) || (!isCheckedType(type) && parameters.checked.value_or(false)) || (type == WebExtensionMenuItemType::Separator && !item->submenuItems().isEmpty())) {
        completion(toWebExtensionError(api, nullString(), "the updated menu type, title or checked state is invalid"_s));
        return;
    }
    if (parameters.parentIdentifier) {
        RefPtr parent = menuItem(*parameters.parentIdentifier);
        if (!parent || parent->type() == WebExtensionMenuItemType::Separator) {
            completion(toWebExtensionError(api, nullString(), "parent menu item not found or cannot contain children"_s));
            return;
        }
        Function<std::optional<String>(const String&)> parentForItem = [&](const String& key) -> std::optional<String> {
            RefPtr ancestor = menuItem(key);
            RefPtr next = ancestor ? ancestor->parentMenuItem() : nullptr;
            return next ? std::optional { next->identifier() } : std::nullopt;
        };
        if (!canReparentWebExtensionMenuItem(identifier, parent->identifier(), parentForItem)) {
            completion(toWebExtensionError(api, nullString(), "parent menu item would create a cycle"_s));
            return;
        }
    }
    // All fallible validation finishes before any map, root or child mutation.
    bool wasRoot = !item->parentMenuItem();
    item->update(parameters);
    if (newIdentifier != identifier) {
        m_menuItems.remove(identifier);
        m_menuItems.set(newIdentifier, *item);
    }
    if (wasRoot && item->parentMenuItem())
        m_mainMenuItems.removeAll(*item);
    completion({ });
}

void WebExtensionContext::menusRemove(const String& identifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    RefPtr item = menuItem(identifier);
    if (!hasMenuPermission(*this) || !item || !decodeWebExtensionMenuIdentifier(identifier)) {
        completion(toWebExtensionError("menus.remove()"_s, nullString(), "menu item not found or permission is not granted"_s));
        return;
    }
    Vector<Ref<WebExtensionMenuItem>> removed { Ref { *item } };
    HashSet<WebExtensionMenuItem*> seen { item.get() };
    for (size_t index = 0; index < removed.size(); ++index) {
        // A separate Ref survives vector growth while collecting descendants.
        Ref current = removed[index].copyRef();
        for (auto& child : current->submenuItems()) {
            if (seen.add(child.ptr()).isNewEntry)
                removed.append(child.copyRef());
        }
    }
    Vector<WTF::UUID> removedCallbacks;
    for (auto& current : removed) {
        if (RefPtr parent = current->parentMenuItem())
            parent->removeSubmenuItem(current.get());
        m_mainMenuItems.removeAll(current);
        m_menuItems.remove(current->identifier());
        auto handler = m_menuClickHandlerIdentifiers.find(current->identifier());
        if (handler != m_menuClickHandlerIdentifiers.end()) {
            removedCallbacks.append(handler->value);
            m_menuClickHandlerIdentifiers.remove(handler);
        }
    }
    removeMenusClickHandlers(removedCallbacks);
    completion({ });
}

void WebExtensionContext::menusRemoveAll(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    if (!hasMenuPermission(*this)) {
        completion(toWebExtensionError("menus.removeAll()"_s, nullString(), "menu permission is not granted"_s));
        return;
    }
    Vector<WTF::UUID> removedCallbacks;
    for (auto& token : m_menuClickHandlerIdentifiers.values())
        removedCallbacks.append(token);
    m_menuClickHandlerIdentifiers.clear();
    m_mainMenuItems.clear();
    m_menuItems.clear();
    removeMenusClickHandlers(removedCallbacks);
    completion({ });
}

static bool menuCanBeActivated(WebExtensionContext& context, const WebExtensionMenuItem& item, const WebExtensionMenuItemContextParameters& details)
{
    if (!hasMenuPermission(context) || item.extensionContext() != &context || item.type() == WebExtensionMenuItemType::Separator || !item.submenuItems().isEmpty() || !item.matches(details))
        return false;
    HashSet<const WebExtensionMenuItem*> seen;
    for (auto* ancestor = &item; ancestor; ancestor = ancestor->parentMenuItem()) {
        if (!seen.add(ancestor).isNewEntry || context.menuItem(ancestor->identifier()) != ancestor || !ancestor->isEnabled() || !ancestor->isVisible())
            return false;
    }
    if (details.tabIdentifier) {
        RefPtr tab = context.getTab(*details.tabIdentifier);
        if (!tab || !tab->isOpen() || !tab->extensionHasAccess())
            return false;
    }
    return true;
}

void WebExtensionContext::performMenuItem(WebExtensionMenuItem& item, const WebExtensionMenuItemContextParameters& details, UserTriggered userTriggered)
{
    if (userTriggered != UserTriggered::Yes || !menuCanBeActivated(*this, item, details))
        return;
    RefPtr tab = details.tabIdentifier ? getTab(*details.tabIdentifier) : nullptr;
    if (tab)
        userGesturePerformed(*tab);
    if (RefPtr command = item.command()) {
        // Menu commands retain the clicked tab even if window focus changed.
        performCommand(*command, tab.get(), UserTriggered::Yes);
        return;
    }
    bool wasChecked = item.toggleCheckedIfNeeded(details);
    fireMenusClickedEventIfNeeded(item, wasChecked, details);
}

void WebExtensionContext::fireMenusClickedEventIfNeeded(const WebExtensionMenuItem& item, bool wasChecked, const WebExtensionMenuItemContextParameters& details)
{
    if (!menuCanBeActivated(*this, item, details))
        return;
    auto loadIdentifier = privilegedIdentifier();
    auto parameters = item.minimalParameters();
    std::optional<WTF::UUID> callbackIdentifier;
    auto handler = m_menuClickHandlerIdentifiers.find(parameters.identifier);
    if (handler != m_menuClickHandlerIdentifiers.end())
        callbackIdentifier = handler->value;
    RefPtr tab = details.tabIdentifier ? getTab(*details.tabIdentifier) : nullptr;
    constexpr auto type = WebExtensionEventListenerType::MenusOnClicked;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, item = Ref { item }, parameters, wasChecked, details, tab, loadIdentifier, callbackIdentifier]() mutable {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier || !menuCanBeActivated(*this, item.get(), details) || item->identifier() != parameters.identifier)
            return;
        if (tab && getTab(tab->identifier()).get() != tab.get())
            return;
        auto current = m_menuClickHandlerIdentifiers.find(parameters.identifier);
        if (current == m_menuClickHandlerIdentifiers.end() || current->value != callbackIdentifier)
            callbackIdentifier = std::nullopt;
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchMenusClickedEvent(parameters, wasChecked, details, tab ? std::optional { tab->parameters() } : std::nullopt, callbackIdentifier));
    });
}
}
#endif
