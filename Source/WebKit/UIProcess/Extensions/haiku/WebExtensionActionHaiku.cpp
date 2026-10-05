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
#include "WebExtensionAction.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "WebExtensionContext.h"
#include "WebExtensionUtilities.h"
#include <InterfaceDefs.h>
#include <cmath>
#include <limits>

namespace WebKit {

WebExtensionAction::WebExtensionAction(WebExtensionContext& extensionContext)
    : m_extensionContext(extensionContext)
{
}

WebExtensionAction::WebExtensionAction(WebExtensionContext& extensionContext, WebExtensionTab& tab)
    : m_extensionContext(extensionContext)
    , m_tab(&tab)
{
}

WebExtensionAction::WebExtensionAction(WebExtensionContext& extensionContext, WebExtensionWindow& window)
    : m_extensionContext(extensionContext)
    , m_window(&window)
{
}

bool WebExtensionAction::operator==(const WebExtensionAction& other) const
{
    return this == &other || (m_extensionContext == other.m_extensionContext && m_tab == other.m_tab && m_window == other.m_window);
}

WebExtensionContext* WebExtensionAction::extensionContext() const
{
    return m_extensionContext.get();
}

void WebExtensionAction::clearCustomizations()
{
#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    if (!m_customIcons && !m_customIconVariants && m_customPopupPath.isNull() && m_customLabel.isNull() && m_customBadgeText.isNull() && !m_customEnabled && !m_blockedResourceCount && !m_hasUnreadBadgeText && !m_customBadgeBackgroundColor && !m_customBadgeTextColor)
#else
    if (!m_customIcons && m_customPopupPath.isNull() && m_customLabel.isNull() && m_customBadgeText.isNull() && !m_customEnabled && !m_blockedResourceCount && !m_hasUnreadBadgeText && !m_customBadgeBackgroundColor && !m_customBadgeTextColor)
#endif
        return;

    m_customIcons = nullptr;
#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    m_customIconVariants = nullptr;
#endif
    m_customPopupPath = nullString();
    m_customLabel = nullString();
    m_customBadgeText = nullString();
    m_customBadgeBackgroundColor = std::nullopt;
    m_customBadgeTextColor = std::nullopt;
    m_customEnabled = std::nullopt;
    m_blockedResourceCount = 0;
    m_hasUnreadBadgeText = std::nullopt;

#if PLATFORM(MAC)
    // Reset the popover appearance until the color-scheme CSS can be checked on the next load.
    m_popoverAppearance = Appearance::Default;
#endif

    clearIconCache();
    propertiesDidChange();
}

RefPtr<WebExtensionAction> WebExtensionAction::fallbackAction() const
{
    RefPtr extensionContext = this->extensionContext();
    if (!extensionContext)
        return nullptr;

    // Tab actions whose tab references have not dropped fallback to the window action.
    if (RefPtr tab = this->tab())
        return extensionContext->getAction(tab->window().get());

    // Window actions and tab actions whose tab references have dropped fallback to the default action.
    if (m_window.has_value() || m_tab.has_value())
        return extensionContext->defaultAction();

    // Default actions have no fallback.
    return nullptr;
}

RefPtr<WebCore::Icon> WebExtensionAction::icon(WebCore::FloatSize idealSize)
{
    RefPtr extensionContext = this->extensionContext();
    if (!extensionContext)
        return nullptr;

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    if (m_customIconVariants || m_customIcons)
#else
    if (m_customIcons)
#endif
    {
        // Clear the cache if the display scales change (connecting display, etc.)
        auto currentScales = availableScreenScales();
        if (currentScales != m_cachedIconScales)
            clearIconCache();

        if (m_cachedIcon && idealSize == m_cachedIconIdealSize)
            return m_cachedIcon;

        RefPtr<WebCore::Icon> result;

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
        if (m_customIconVariants) {
            result = protect(extensionContext->extension())->bestIconVariant(m_customIconVariants, WebCore::FloatSize(idealSize), [&](Ref<API::Error> error) {
                extensionContext->recordError(error);
            });
        } else
#endif // ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
        if (m_customIcons) {
            result = protect(extensionContext->extension())->bestIcon(m_customIcons, WebCore::FloatSize(idealSize), [&](Ref<API::Error> error) {
                extensionContext->recordError(error);
            });
        }

        if (result) {
            m_cachedIcon = result;
            m_cachedIconScales = currentScales;
            m_cachedIconIdealSize = idealSize;

            return result;
        }

        clearIconCache();

        // If custom icons fail, fallback.
    }

    if (RefPtr fallback = fallbackAction())
        return fallback->icon(idealSize);

    // Default
    return protect(extensionContext->extension())->actionIcon(WebCore::FloatSize(idealSize));
}

void WebExtensionAction::setIcons(RefPtr<JSON::Object> icons)
{
    if (m_customIcons == icons)
        return;

    m_customIcons = icons && icons->size() ? icons : nullptr;
#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
    m_customIconVariants = nullptr;
#endif

    clearIconCache();
    propertiesDidChange();
}

#if ENABLE(WK_WEB_EXTENSIONS_ICON_VARIANTS)
void WebExtensionAction::setIconVariants(RefPtr<JSON::Array> iconVariants)
{
    if (m_customIconVariants == iconVariants)
        return;

    m_customIconVariants = iconVariants && iconVariants->length() ? iconVariants : nullptr;
    m_customIcons = nullptr;

    clearIconCache();
    propertiesDidChange();
}
#endif

void WebExtensionAction::clearIconCache()
{
    m_cachedIcon = nullptr;
    m_cachedIconScales = { };
    m_cachedIconIdealSize = { };
}

String WebExtensionAction::popupPath() const
{
    RefPtr extensionContext = this->extensionContext();
    if (!extensionContext)
        return emptyString();

    if (!m_customPopupPath.isNull())
        return m_customPopupPath;

    if (RefPtr fallback = fallbackAction())
        return fallback->popupPath();

    // Default
    return protect(extensionContext->extension())->actionPopupPath();
}

void WebExtensionAction::setPopupPath(String path)
{
    if (m_customPopupPath == path)
        return;

    m_customPopupPath = path;

#if PLATFORM(MAC)
    // Reset the popover appearance until the color-scheme CSS can be checked on the next load.
    m_popoverAppearance = Appearance::Default;
#endif

    propertiesDidChange();
}

String WebExtensionAction::label(FallbackWhenEmpty fallbackWhenEmpty) const
{
    RefPtr extensionContext = this->extensionContext();
    if (!extensionContext)
        return emptyString();

    if (!m_customLabel.isNull()) {
        if (!m_customLabel.isEmpty() || fallbackWhenEmpty == FallbackWhenEmpty::No)
            return m_customLabel;

        return protect(extensionContext->extension())->displayName();
    }

    if (RefPtr fallback = fallbackAction())
        return fallback->label(fallbackWhenEmpty);

    // Default
    Ref extension = extensionContext->extension();
    if (auto defaultLabel = extension->displayActionLabel(); !defaultLabel.isEmpty() || fallbackWhenEmpty == FallbackWhenEmpty::No)
        return defaultLabel;

    return extension->displayName();
}

void WebExtensionAction::setLabel(String label)
{
    if (m_customLabel == label)
        return;

    m_customLabel = label;

    propertiesDidChange();
}

String WebExtensionAction::badgeText() const
{
    if (!extensionContext())
        return emptyString();

    if (!m_customBadgeText.isNull())
        return m_customBadgeText;

    if (m_blockedResourceCount)
        return String::number(m_blockedResourceCount);

    if (RefPtr fallback = fallbackAction())
        return fallback->badgeText();

    // Default
    return emptyString();
}

void WebExtensionAction::setBadgeText(String badgeText)
{
    if (m_customBadgeText == badgeText)
        return;

    m_customBadgeText = badgeText;

    if (!badgeText.isNull())
        m_hasUnreadBadgeText = !badgeText.isEmpty();
    else
        m_hasUnreadBadgeText = std::nullopt;

    propertiesDidChange();
}

uint32_t WebExtensionAction::badgeBackgroundColor() const
{
    if (m_customBadgeBackgroundColor)
        return *m_customBadgeBackgroundColor;
    if (RefPtr fallback = fallbackAction())
        return fallback->badgeBackgroundColor();
    return 0xD90000FF;
}

std::optional<uint32_t> WebExtensionAction::customBadgeTextColor() const
{
    if (m_customBadgeTextColor)
        return m_customBadgeTextColor;
    if (RefPtr fallback = fallbackAction())
        return fallback->customBadgeTextColor();
    return std::nullopt;
}

uint32_t WebExtensionAction::badgeTextColor() const
{
    if (auto custom = customBadgeTextColor())
        return *custom;
    // Inherit explicit foreground overrides, but compute automatic contrast
    // using this action's effective background, including a tab override.
    const auto background = badgeBackgroundColor();
    const auto panel = ui_color(B_PANEL_BACKGROUND_COLOR);
    const double alpha = (background & 255) / 255.0;
    const auto linear = [alpha](unsigned component, uint8_t panelComponent) {
        double value = (component * alpha + panelComponent * (1 - alpha)) / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow((value + 0.055) / 1.055, 2.4);
    };
    const double luminance = 0.2126 * linear(background >> 24, panel.red)
        + 0.7152 * linear((background >> 16) & 255, panel.green)
        + 0.0722 * linear((background >> 8) & 255, panel.blue);
    return (luminance + 0.05) / 0.05 >= 1.05 / (luminance + 0.05) ? 0x000000FF : 0xFFFFFFFF;
}

void WebExtensionAction::setBadgeBackgroundColor(std::optional<uint32_t> color)
{
    if (m_customBadgeBackgroundColor == color)
        return;
    m_customBadgeBackgroundColor = color;
    propertiesDidChange();
}

void WebExtensionAction::setBadgeTextColor(std::optional<uint32_t> color)
{
    if (m_customBadgeTextColor == color)
        return;
    m_customBadgeTextColor = color;
    propertiesDidChange();
}

bool WebExtensionAction::hasUnreadBadgeText() const
{
    if (!extensionContext() || badgeText().isEmpty())
        return false;

    if (m_hasUnreadBadgeText)
        return m_hasUnreadBadgeText.value();

    if (RefPtr fallback = fallbackAction())
        return fallback->hasUnreadBadgeText();

    // Default
    return false;
}

void WebExtensionAction::setHasUnreadBadgeText(bool hasUnreadBadgeText)
{
    auto newValue = !badgeText().isEmpty() ? std::optional(hasUnreadBadgeText) : std::nullopt;
    if (m_hasUnreadBadgeText == newValue)
        return;

    m_hasUnreadBadgeText = newValue;

    // Only propagate the change if we're setting it to false.
    if (RefPtr fallback = fallbackAction(); fallback && !hasUnreadBadgeText)
        fallback->setHasUnreadBadgeText(false);

    propertiesDidChange();
}

bool WebExtensionAction::isEnabled() const
{
    if (!extensionContext())
        return false;

    if (m_customEnabled)
        return m_customEnabled.value();

    if (RefPtr fallback = fallbackAction())
        return fallback->isEnabled();

    // Default
    return true;
}

void WebExtensionAction::setEnabled(std::optional<bool> enabled)
{
    if (m_customEnabled == enabled)
        return;

    m_customEnabled = enabled;

    propertiesDidChange();
}

void WebExtensionAction::clearBlockedResourceCount()
{
    if (!m_blockedResourceCount)
        return;
    m_blockedResourceCount = 0;
    if (badgeText().isEmpty())
        m_hasUnreadBadgeText = std::nullopt;
    propertiesDidChange();
}

void WebExtensionAction::incrementBlockedResourceCount(ssize_t amount)
{
    if (!amount)
        return;
    auto previousCount = m_blockedResourceCount;
    if (amount > 0) {
        constexpr auto maximum = std::numeric_limits<ssize_t>::max();
        m_blockedResourceCount = amount > maximum - previousCount ? maximum : previousCount + amount;
    } else
        m_blockedResourceCount = amount <= -previousCount ? 0 : previousCount + amount;
    if (m_blockedResourceCount == previousCount)
        return;
    if (!badgeText().isEmpty())
        m_hasUnreadBadgeText = true;
    else
        m_hasUnreadBadgeText = std::nullopt;
    propertiesDidChange();
}

void WebExtensionAction::propertiesDidChange()
{
    if (m_updatePending)
        return;
    RefPtr context = extensionContext();
    if (!context || !context->isLoaded())
        return;
    m_updatePending = true;
    auto loadIdentifier = context->privilegedIdentifier();
    RunLoop::mainSingleton().dispatchAfter(25_ms, [this, protectedThis = Ref { *this }, context = WTF::move(context), loadIdentifier] {
        m_updatePending = false;
        if (!context->isLoaded() || context->privilegedIdentifier() != loadIdentifier)
            return;
        context->nativeActionPropertiesDidChange(*this);
    });
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
