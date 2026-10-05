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

#include "config.h"
#include "WebExtensionMenuItem.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionContext.h"
#include "WebExtensionMenuItemParameters.h"
#include "WebExtensionUtilities.h"
namespace WebKit {
WebExtensionMenuItem::WebExtensionMenuItem(WebExtensionContext& extensionContext, const WebExtensionMenuItemParameters& parameters)
    : m_extensionContext(extensionContext)
    , m_type(parameters.type.value_or(WebExtensionMenuItemType::Normal))
    , m_identifier(parameters.identifier)
    , m_title(removeAmpersands(parameters.title))
    , m_command(extensionContext.command(parameters.command))
    , m_checked(isCheckedType(m_type) && parameters.checked.value_or(false))
    , m_enabled(parameters.enabled.value_or(true))
    , m_visible(parameters.visible.value_or(true))
{
    if (parameters.parentIdentifier) {
        if (RefPtr parentMenuItem = extensionContext.menuItem(parameters.parentIdentifier.value()))
            parentMenuItem->addSubmenuItem(*this);
    }

    if (parameters.contexts && !parameters.contexts.value().isEmpty())
        m_contexts = parameters.contexts.value();
    else if (m_parentMenuItem)
        m_contexts = m_parentMenuItem->contexts();
    else
        m_contexts = WebExtensionMenuItemContextType::Page;

    if (!parameters.iconsJSON.isEmpty()) {
        RefPtr parsedIcons = JSON::Value::parseJSON(parameters.iconsJSON);
        m_icons = parsedIcons ? parsedIcons->asObject() : nullptr;
#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
        m_iconVariants = parsedIcons ? parsedIcons->asArray() : nullptr;
#endif
        clearIconCache();
    }

    if (parameters.documentURLPatterns) {
        for (auto& patternString : parameters.documentURLPatterns.value()) {
            if (RefPtr pattern = WebExtensionMatchPattern::getOrCreate(patternString))
                m_documentPatterns.add(pattern.releaseNonNull());
        }
    }

    if (parameters.targetURLPatterns) {
        for (auto& patternString : parameters.targetURLPatterns.value()) {
            if (RefPtr pattern = WebExtensionMatchPattern::getOrCreate(patternString))
                m_targetPatterns.add(pattern.releaseNonNull());
        }
    }
}

WebExtensionMenuItemParameters WebExtensionMenuItem::minimalParameters() const
{
    return {
        identifier(),
        parentMenuItem() ? std::optional { parentMenuItem()->identifier() } : std::nullopt,

        type(),

        nullString(), // title
        nullString(), // command
        nullString(), // iconsJSON

        isChecked(),
        isEnabled(),
        isVisible(),

        std::nullopt, // documentURLPatterns
        std::nullopt, // targetURLPatterns

        contexts(),
    };
}

void WebExtensionMenuItem::update(const WebExtensionMenuItemParameters& parameters)
{
    RefPtr extensionContext = this->extensionContext();
    ASSERT(extensionContext);

    if (parameters.type) {
        m_type = parameters.type.value();
        if (!isCheckedType(m_type))
            m_checked = false;
    }

    if (!parameters.identifier.isEmpty())
        m_identifier = parameters.identifier;

    if (parameters.parentIdentifier) {
        RefPtr updatedParentMenuItem = extensionContext->menuItem(parameters.parentIdentifier.value());
        if (updatedParentMenuItem.get() != m_parentMenuItem) {
            if (RefPtr parentMenuItem = m_parentMenuItem.get())
                parentMenuItem->removeSubmenuItem(*this);

            if (updatedParentMenuItem)
                updatedParentMenuItem->addSubmenuItem(*this);
        }
    }

    if (!parameters.title.isNull())
        m_title = removeAmpersands(parameters.title);

    if (!parameters.command.isNull())
        m_command = extensionContext->command(parameters.command);

    if (!parameters.iconsJSON.isNull()) {
        RefPtr parsedIcons = JSON::Value::parseJSON(parameters.iconsJSON);
        m_icons = parsedIcons ? parsedIcons->asObject() : nullptr;
#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
        m_iconVariants = parsedIcons ? parsedIcons->asArray() : nullptr;
#endif
        clearIconCache();
    }

    if (parameters.checked)
        m_checked = isCheckedType(m_type) && parameters.checked.value();

    if (parameters.enabled)
        m_enabled = parameters.enabled.value();

    if (parameters.visible)
        m_visible = parameters.visible.value();

    if (parameters.contexts) {
        if (!parameters.contexts.value().isEmpty())
            m_contexts = parameters.contexts.value();
        else if (m_parentMenuItem)
            m_contexts = m_parentMenuItem->contexts();
        else
            m_contexts = WebExtensionMenuItemContextType::Page;
    }

    if (parameters.documentURLPatterns) {
        m_documentPatterns.clear();

        for (auto& patternString : parameters.documentURLPatterns.value()) {
            if (RefPtr pattern = WebExtensionMatchPattern::getOrCreate(patternString))
                m_documentPatterns.add(pattern.releaseNonNull());
        }
    }

    if (parameters.targetURLPatterns) {
        m_targetPatterns.clear();

        for (auto& patternString : parameters.targetURLPatterns.value()) {
            if (RefPtr pattern = WebExtensionMatchPattern::getOrCreate(patternString))
                m_targetPatterns.add(pattern.releaseNonNull());
        }
    }
}

RefPtr<WebCore::Icon> WebExtensionMenuItem::icon(WebCore::FloatSize idealSize) const
{
    RefPtr extensionContext = this->extensionContext();
    ASSERT(extensionContext);

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    if (!m_iconVariants && !m_icons)
#else
    if (!m_icons)
#endif
        return nullptr;

    // Clear the cache if the display scales change (connecting display, etc.)
    auto currentScales = availableScreenScales();
    if (currentScales != m_cachedIconScales)
        clearIconCache();

    if (m_cachedIcon && idealSize == m_cachedIconIdealSize)
        return m_cachedIcon;

    RefPtr<WebCore::Icon> result;

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    if (m_iconVariants) {
        result = protect(extensionContext->extension())->bestIconVariant(m_iconVariants, WebCore::FloatSize(idealSize), [&](Ref<API::Error> error) {
            extensionContext->recordError(error);
        });
    } else
#endif // ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    if (m_icons) {
        result = protect(extensionContext->extension())->bestIcon(m_icons, WebCore::FloatSize(idealSize), [&](Ref<API::Error> error) {
            extensionContext->recordError(error);
        });
    }

    if (result) {
        m_cachedIcon = result;
        m_cachedIconIdealSize = idealSize;
        m_cachedIconScales = currentScales;

        return result;
    }

    clearIconCache();

    return nullptr;
}

void WebExtensionMenuItem::clearIconCache() const
{
    m_cachedIcon = nullptr;
    m_cachedIconScales = { };
    m_cachedIconIdealSize = { };
}

}
#endif
