/*
 * Copyright (C) 2023 Apple Inc. All rights reserved.
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
#include "WebExtensionAction.h"
#include "WebExtensionActionDetails.h"
#include "WebExtension.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionUtilities.h"
#include <WebCore/CSSPropertyParserConsumer+Color.h>
#include <WebCore/Color.h>
#include <WebCore/DataURLDecoder.h>
namespace WebKit {
enum class CreateAction : bool { No, Yes };
static std::expected<Ref<WebExtensionAction>, WebExtensionError> actionWithIdentifiers(WebExtensionContext& context,
    std::optional<WebExtensionWindowIdentifier> windowID, std::optional<WebExtensionTabIdentifier> tabID, CreateAction create, ASCIILiteral api)
{
    if ((windowID && tabID) || (windowID && (!isValid(windowID) || (!isCurrent(windowID) && windowID->toUInt64() > 9007199254740991ULL)))
        || (tabID && (!isValid(tabID) || tabID->toUInt64() > 9007199254740991ULL)))
        return toWebExtensionError(api, "details"_s, "invalid action target"_s);
    if (windowID) {
        RefPtr window = context.getWindow(*windowID);
        if (!window || !window->isOpen() || !window->extensionHasAccess())
            return toWebExtensionError(api, nullString(), "window not found"_s);
        return create == CreateAction::Yes ? context.getOrCreateAction(window.get()) : context.getAction(window.get());
    }
    if (tabID) {
        RefPtr tab = context.getTab(*tabID);
        if (!tab || !tab->isOpen() || !tab->extensionHasAccess())
            return toWebExtensionError(api, nullString(), "tab not found"_s);
        return create == CreateAction::Yes ? context.getOrCreateAction(tab.get()) : context.getAction(tab.get());
    }
    return Ref { context.defaultAction() };
}
bool WebExtensionContext::isActionMessageAllowed(IPC::Decoder& message)
{
    Ref extension = *m_extension;
    return isLoadedAndPrivilegedMessage(message) && (extension->hasAction() || extension->hasBrowserAction() || extension->hasPageAction());
}
void WebExtensionContext::actionSetIcon(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, const String& iconsJSON, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    constexpr auto api = "action.setIcon()"_s;
    auto parsed = parseWebExtensionActionIconJSON(iconsJSON);
    if (!parsed) {
        completion(toWebExtensionError(api, "details"_s, parsed.error()));
        return;
    }
    auto existing = actionWithIdentifiers(*this, window, tab, CreateAction::No, api);
    if (!existing) {
        completion(std::unexpected(existing.error()));
        return;
    }
    RefPtr icons = *parsed;
    if (icons) {
        Ref extension = *m_extension;
        // Decode every supplied representation before replacing scoped state.
        // The extension's resource resolver enforces package containment.
        for (auto& dimension : icons->keys()) {
            auto path = icons->getString(dimension);
            if (path.startsWith("data:"_s)) {
                auto data = WebCore::DataURLDecoder::decode(URL { path });
                if (!data || data->data.isEmpty()) {
                    completion(toWebExtensionError(api, "path"_s, "invalid icon data URL"_s));
                    return;
                }
            }
            if (!extension->iconForPath(path, { 16, 16 })) {
                completion(toWebExtensionError(api, "path"_s, "icon resource could not be decoded"_s));
                return;
            }
        }
    }
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::Yes, api);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    (*action)->setIcons(WTF::move(icons));
    completion({ });
}

void WebExtensionContext::actionGetBadgeColor(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, bool background, CompletionHandler<void(std::expected<uint32_t, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::No, background ? "action.getBadgeBackgroundColor()"_s : "action.getBadgeTextColor()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    completion(background ? (*action)->badgeBackgroundColor() : (*action)->badgeTextColor());
}
void WebExtensionContext::actionSetBadgeColor(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, bool background, const String& cssColor, std::optional<uint32_t> color, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    const auto api = background ? "action.setBadgeBackgroundColor()"_s : "action.setBadgeTextColor()"_s;
    if (!cssColor.isNull()) {
        // Badge settings have no document from which to resolve currentColor,
        // system colors or CSS variables. Parse absolute CSS colors in WebCore.
        auto parsed = WebCore::CSSPropertyParserHelpers::deprecatedParseColorRawWithoutContext(cssColor,
            { .allowedColorTypes = { WebCore::CSS::ColorType::Absolute } });
        if (color || cssColor.contains('\0') || !parsed.isValid()) {
            completion(toWebExtensionError(api, "color"_s, "invalid badge color"_s));
            return;
        }
        const auto [red, green, blue, alpha] = parsed.toColorTypeLossy<WebCore::SRGBA<uint8_t>>().resolved();
        color = (uint32_t(red) << 24) | (uint32_t(green) << 16) | (uint32_t(blue) << 8) | alpha;
    }
    if (!background && color && !(*color & 255)) {
        completion(toWebExtensionError(api, "color"_s, "badge text must not be fully transparent"_s));
        return;
    }
    // Resolve and validate access before creating or modifying scoped state.
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::Yes, api);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    if (background)
        (*action)->setBadgeBackgroundColor(color);
    else
        (*action)->setBadgeTextColor(color);
    completion({ });
}
void WebExtensionContext::actionGetTitle(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::No, "action.getTitle()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    completion((*action)->label(WebExtensionAction::FallbackWhenEmpty::No));
}
void WebExtensionContext::actionGetBadgeText(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::No, "action.getBadgeText()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    completion((*action)->badgeText());
}
void WebExtensionContext::actionGetPopup(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::No, "action.getPopup()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    completion((*action)->popupPath());
}
void WebExtensionContext::actionGetEnabled(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::No, "action.isEnabled()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    completion((*action)->isEnabled());
}
void WebExtensionContext::actionSetTitle(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, const String& value, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    if (value.contains('\0')) {
        completion(toWebExtensionError("action.setTitle()"_s, nullString(), "text must not contain null characters"_s));
        return;
    }
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::Yes, "action.setTitle()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    (*action)->setLabel(value);
    completion({ });
}
void WebExtensionContext::actionSetBadgeText(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, const String& value, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    if (value.contains('\0')) {
        completion(toWebExtensionError("action.setBadgeText()"_s, nullString(), "text must not contain null characters"_s));
        return;
    }
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::Yes, "action.setBadgeText()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    (*action)->setBadgeText(value);
    completion({ });
}
void WebExtensionContext::actionSetPopup(std::optional<WebExtensionWindowIdentifier> window, std::optional<WebExtensionTabIdentifier> tab, const String& value, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    if (value.contains('\0')) {
        completion(toWebExtensionError("action.setPopup()"_s, nullString(), "text must not contain null characters"_s));
        return;
    }
    auto action = actionWithIdentifiers(*this, window, tab, CreateAction::Yes, "action.setPopup()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    (*action)->setPopupPath(value);
    completion({ });
}
void WebExtensionContext::actionSetEnabled(std::optional<WebExtensionTabIdentifier> tab, bool enabled, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completion)
{
    auto action = actionWithIdentifiers(*this, std::nullopt, tab, CreateAction::Yes, enabled ? "action.enable()"_s : "action.disable()"_s);
    if (!action) {
        completion(std::unexpected(action.error()));
        return;
    }
    (*action)->setEnabled(enabled);
    completion({ });
}
void WebExtensionContext::fireActionClickedEventIfNeeded(WebExtensionTab* tab)
{
    if (!isLoaded() || (tab && (!isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess())))
        return;
    Ref action = getAction(tab);
    if (!action->isEnabled() || action->presentsPopup())
        return;
    auto loadIdentifier = privilegedIdentifier();
    constexpr auto type = WebExtensionEventListenerType::ActionOnClicked;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, tab = RefPtr { tab }, loadIdentifier] {
        if (!isLoaded() || !m_privilegedIdentifier || *m_privilegedIdentifier != loadIdentifier)
            return;
        if (tab && (!isValidTab(*tab) || !tab->isOpen() || !tab->extensionHasAccess()))
            return;
        Ref action = getAction(tab.get());
        if (!action->isEnabled() || action->presentsPopup())
            return;
        sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchActionClickedEvent(tab ? std::optional(tab->parameters()) : std::nullopt));
    });
}
}
#endif
