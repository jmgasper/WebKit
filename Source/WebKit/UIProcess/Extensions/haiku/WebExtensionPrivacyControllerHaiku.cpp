/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionPrivacyControllerHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIPageConfiguration.h"
#include "WebExtensionContext.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionController.h"
#include "WebExtensionControllerConfiguration.h"
#include "WebExtensionPermission.h"
#include "WebPageProxy.h"
#include "WebPreferences.h"
#include "WebsiteDataStore.h"
#include <wtf/SetForScope.h>
#include <wtf/TZoneMallocInlines.h>

namespace WebKit {
WTF_MAKE_TZONE_ALLOCATED_IMPL(WebExtensionPrivacyControllerHaiku);
WebExtensionPrivacyControllerHaiku& WebExtensionController::nativePrivacy()
{
    if (!m_nativePrivacy)
        m_nativePrivacy = makeUnique<WebExtensionPrivacyControllerHaiku>(*this);
    return *m_nativePrivacy;
}
WebExtensionPrivacyControllerHaiku::WebExtensionPrivacyControllerHaiku(WebExtensionController& controller)
    : m_controller(controller)
{
    Ref configuration = API::PageConfiguration::create();
    auto& preferences = configuration->preferences();
    m_defaults = { preferences.linkPrefetchEnabled() || preferences.speculationRulesPrefetchEnabled(), preferences.hyperlinkAuditingEnabled() };
}
bool WebExtensionPrivacyControllerHaiku::isPrivate(WebPageProxy& page) const
{
    // A nonpersistent controller's own store is its regular profile, matching
    // WebExtensionController's content-controller classification.
    return !page.websiteDataStore().isPersistent()
        && &page.websiteDataStore() != &m_controller.configuration().defaultWebsiteDataStore();
}
bool WebExtensionPrivacyControllerHaiku::hasPrivatePages() const
{
    for (Ref page : m_controller.allPages()) {
        if (isPrivate(page))
            return true;
    }
    return false;
}
void WebExtensionPrivacyControllerHaiku::addPage(WebPageProxy& page)
{
    if (!m_defaultBrowserPage && !isPrivate(page) && page.configuration().requiredWebExtensionBaseURL().isEmpty())
        m_defaultBrowserPage = page;
    // Background wake-up can construct a page during refresh. Apply before its
    // first load even when the remaining event work must be deferred.
    bool incognito = isPrivate(page);
    apply(page, winner(WebExtensionPrivacySettingHaiku::NetworkPrediction, incognito), winner(WebExtensionPrivacySettingHaiku::HyperlinkAuditing, incognito));
    refresh();
}
void WebExtensionPrivacyControllerHaiku::removePage(WebPageProxy& page)
{
    if (m_defaultBrowserPage.get() == &page)
        m_defaultBrowserPage = nullptr;
    page.setNativePrivacyPreferences(std::nullopt, std::nullopt);
    refresh();
}
WebExtensionPrivacyControllerHaiku::Winner WebExtensionPrivacyControllerHaiku::winner(WebExtensionPrivacySettingHaiku setting, bool incognito)
{
    Winner result;
    auto contexts = m_controller.extensionContexts();
    for (Ref context : contexts) {
        if (!context->isLoaded() || !context->nativeInstallationOrder() || !context->hasPermission(WebExtensionPermission::privacy())
            || (incognito && !context->hasAccessToPrivateData()))
            continue;
        auto session = m_sessionValues.find(context->nativeInstallationIdentifier());
        auto value = context->nativePrivacySettings().effective(setting, incognito, session == m_sessionValues.end() ? nullptr : &session->value);
        if (!value || (result.context && result.context->nativeInstallationOrder() > context->nativeInstallationOrder()))
            continue;
        result = { context.ptr(), value };
    }
    return result;
}
Ref<JSON::Object> WebExtensionPrivacyControllerHaiku::details(WebExtensionContext& context, WebExtensionPrivacySettingHaiku setting, bool incognito)
{
    return details(context, setting, incognito, winner(setting, incognito));
}
Ref<JSON::Object> WebExtensionPrivacyControllerHaiku::details(WebExtensionContext& context, WebExtensionPrivacySettingHaiku setting, bool incognito, const Winner& winner)
{
    Ref result = JSON::Object::create();
    result->setBoolean("value"_s, winner.value ? winner.value->value : m_defaults[static_cast<size_t>(setting)]);
    String control = "controllable_by_this_extension"_s;
    if (!context.nativeInstallationOrder() || (incognito && !context.hasAccessToPrivateData()))
        control = "not_controllable"_s;
    else if (winner.context.get() == &context)
        control = "controlled_by_this_extension"_s;
    else if (winner.context && winner.context->nativeInstallationOrder() > context.nativeInstallationOrder())
        control = "controlled_by_other_extensions"_s;
    result->setString("levelOfControl"_s, control);
    if (context.hasAccessToPrivateData())
        result->setBoolean("incognitoSpecific"_s, incognito && winner.value && winner.value->incognitoSpecific);
    return result;
}
void WebExtensionPrivacyControllerHaiku::setSessionValue(WebExtensionContext& context, WebExtensionPrivacySettingHaiku setting, std::optional<bool> value)
{
    auto& values = m_sessionValues.ensure(context.nativeInstallationIdentifier(), [] { return WebExtensionPrivacyValuesHaiku { }; }).iterator->value;
    values[static_cast<size_t>(setting)] = value;
    refresh();
}
void WebExtensionPrivacyControllerHaiku::apply(WebPageProxy& page, const Winner& prediction, const Winner& hyperlink)
{
    page.setNativePrivacyPreferences(prediction.value ? std::optional { prediction.value->value } : std::nullopt,
        hyperlink.value ? std::optional { hyperlink.value->value } : std::nullopt);
}
void WebExtensionPrivacyControllerHaiku::dispatch(WebExtensionContext& context, WebExtensionPrivacySettingHaiku setting, const String& details, bool incognito)
{
    auto generation = context.nativePrivacyGeneration();
    context.wakeUpBackgroundContentIfNecessaryToFireEvents({ WebExtensionEventListenerType::PrivacyOnChange },
        [protectedContext = Ref { context }, generation, setting, details, incognito] {
            if (!protectedContext->isLoaded() || protectedContext->nativePrivacyGeneration() != generation
                || !protectedContext->hasPermission(WebExtensionPermission::privacy())
                || (incognito && !protectedContext->hasAccessToPrivateData()))
                return;
            protectedContext->sendToProcessesForEvent(WebExtensionEventListenerType::PrivacyOnChange,
                Messages::WebExtensionContextProxy::DispatchPrivacyChanged(webExtensionPrivacySettingNameHaiku(setting), details));
        });
}
void WebExtensionPrivacyControllerHaiku::refresh()
{
    if (m_stopped)
        return;
    if (m_refreshing) {
        m_refreshAgain = true;
        return;
    }
    SetForScope guard(m_refreshing, true);
    do {
        m_refreshAgain = false;
        if (!m_defaultBrowserPage) {
            for (Ref page : m_controller.allPages()) {
                if (!isPrivate(page) && page->configuration().requiredWebExtensionBaseURL().isEmpty()) {
                    m_defaultBrowserPage = page;
                    break;
                }
            }
        }
        if (RefPtr page = m_defaultBrowserPage.get()) {
            auto& preferences = page->preferences();
            m_defaults = { preferences.linkPrefetchEnabled() || preferences.speculationRulesPrefetchEnabled(), preferences.hyperlinkAuditingEnabled() };
        }
        bool privatePages = hasPrivatePages();
        if (m_hadPrivatePages && !privatePages)
            m_sessionValues.clear();
        m_hadPrivatePages = privatePages;
        std::array<std::array<Winner, 2>, 2> winners;
        for (size_t setting = 0; setting < 2; ++setting) {
            for (size_t incognito = 0; incognito < 2; ++incognito)
                winners[setting][incognito] = winner(static_cast<WebExtensionPrivacySettingHaiku>(setting), incognito);
        }
        for (Ref page : m_controller.allPages()) {
            bool incognito = isPrivate(page);
            apply(page, winners[0][incognito], winners[1][incognito]);
        }
        HashSet<uint64_t> active;
        auto contexts = m_controller.extensionContexts();
        for (Ref context : contexts) {
            if (!context->isLoaded() || !context->hasPermission(WebExtensionPermission::privacy()))
                continue;
            auto identifier = context->identifier().toUInt64();
            active.add(identifier);
            std::array<String, 4> current;
            for (size_t setting = 0; setting < 2; ++setting) {
                current[setting * 2] = details(context, static_cast<WebExtensionPrivacySettingHaiku>(setting), false, winners[setting][0])->toJSONString();
                if (context->hasAccessToPrivateData())
                    current[setting * 2 + 1] = details(context, static_cast<WebExtensionPrivacySettingHaiku>(setting), true, winners[setting][1])->toJSONString();
            }
            auto previous = m_eventDetails.find(identifier);
            if (previous != m_eventDetails.end()) {
                for (size_t setting = 0; setting < 2; ++setting) {
                    bool regularChanged = current[setting * 2] != previous->value[setting * 2];
                    if (regularChanged)
                        dispatch(context, static_cast<WebExtensionPrivacySettingHaiku>(setting), current[setting * 2], false);
                    if (!current[setting * 2 + 1].isNull() && !previous->value[setting * 2 + 1].isNull()
                        && current[setting * 2 + 1] != previous->value[setting * 2 + 1]
                        && (!regularChanged || current[setting * 2 + 1] != current[setting * 2]))
                        dispatch(context, static_cast<WebExtensionPrivacySettingHaiku>(setting), current[setting * 2 + 1], true);
                }
            }
            m_eventDetails.set(identifier, WTF::move(current));
        }
        m_eventDetails.removeIf([&](const auto& entry) { return !active.contains(entry.key); });
    } while (m_refreshAgain);
}
void WebExtensionPrivacyControllerHaiku::shutdown()
{
    m_stopped = true;
    for (Ref page : m_controller.allPages())
        apply(page, { }, { });
    m_sessionValues.clear();
    m_eventDetails.clear();
}
}
#endif
