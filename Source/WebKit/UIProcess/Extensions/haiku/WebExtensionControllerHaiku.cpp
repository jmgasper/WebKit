/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebExtensionController.h"

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include "BrowserCommandBrokerHaiku.h"
#include "BrowserTabRegistryHaiku.h"
#include "APINavigationAction.h"
#include "APIWebsitePolicies.h"
#include "ExtensionPermissionPromptHaiku.h"
#include "WebExtensionContext.h"
#include "WebExtensionMatchPattern.h"
#include "WebExtensionPermission.h"

namespace WebKit {

void WebExtensionController::initializePlatform()
{
    ASSERT(RunLoop::isMain());
    ASSERT(!m_nativeBrowserTabs);
    // Page configurations, the native SDK and loaded extension contexts share
    // this controller's host state without retaining native windows or pages.
    m_nativeBrowserTabs = std::make_shared<BrowserTabRegistryHaiku>();
    m_nativeBrowserTabs->setObserver([weakThis = WeakPtr { *this }](const auto& before, const auto& after) {
        RefPtr controller = weakThis.get();
        if (!controller)
            return;
        Vector<uint64_t> oldWindows, newWindows;
        for (auto& window : before.windows)
            oldWindows.append(window.identifier);
        for (auto& window : after.windows)
            newWindows.append(window.identifier);
        auto flatten = [](const auto& snapshot) {
            Vector<WebExtensionContext::NativeBrowserTabState> tabs;
            for (auto& window : snapshot.windows) {
                for (size_t index = 0; index < window.pages.size(); ++index)
                    tabs.append({ window.pages[index], window.identifier, index, window.activePage == window.pages[index] });
            }
            return tabs;
        };
        auto oldTabs = flatten(before);
        auto newTabs = flatten(after);
        auto contexts = controller->extensionContexts();
        for (Ref context : contexts) {
            if (!context->isLoaded() || context->extensionController() != controller.get())
                continue;
            // Browsers report a closing tab before its window, and a new
            // window before the tabs it opens with.
            context->nativeBrowserTabsWillChange(oldTabs, newTabs);
            context->nativeBrowserWindowsDidChange(oldWindows, newWindows, before.focusedWindow, after.focusedWindow);
            context->nativeBrowserTabsDidChange(oldTabs, newTabs);
        }
    });
    m_nativePermissionPrompts = ExtensionPermissionPromptHaiku::create();
    m_nativeBrowserCommands = BrowserCommandBrokerHaiku::create();
}

// WebCore applies an extension's redirect and modify-headers rules only to
// URLs listed here for its rule list, as on Cocoa: the host permissions it
// holds. Without this no declarativeNetRequest redirect ever applied.
void WebExtensionController::updateWebsitePoliciesForNavigation(API::WebsitePolicies& websitePolicies, API::NavigationAction&)
{
    auto actionPatterns = websitePolicies.activeContentRuleListActionPatterns();
    for (Ref context : m_extensionContexts) {
        if (!context->hasPermission(WebExtensionPermission::declarativeNetRequest())
            && !context->hasPermission(WebExtensionPermission::declarativeNetRequestWithHostAccess()))
            continue;
        OptionSet<WebExtensionMatchPattern::Options> expandOptions;
        if (context->hasAccessToFileURLs())
            expandOptions.add(WebExtensionMatchPattern::Options::AllowFileScheme);
        Vector<String> patterns;
        for (Ref pattern : context->currentPermissionMatchPatterns())
            patterns.appendVector(pattern->expandedStrings(expandOptions));
        actionPatterns.set(context->uniqueIdentifier(), WTF::move(patterns));
    }
    websitePolicies.setActiveContentRuleListActionPatterns(WTF::move(actionPatterns));
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
