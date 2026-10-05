/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionPrivacySettingsHaiku.h"
#include <wtf/HashMap.h>
#include <wtf/RefPtr.h>
#include <wtf/WeakPtr.h>
#include <wtf/TZoneMalloc.h>

namespace WebKit {
class WebExtensionContext;
class WebExtensionController;
class WebPageProxy;
class WebExtensionPrivacyControllerHaiku {
    WTF_MAKE_TZONE_ALLOCATED(WebExtensionPrivacyControllerHaiku);
public:
    explicit WebExtensionPrivacyControllerHaiku(WebExtensionController&);
    void addPage(WebPageProxy&);
    void removePage(WebPageProxy&);
    void refresh();
    void shutdown();
    bool hasPrivatePages() const;
    Ref<JSON::Object> details(WebExtensionContext&, WebExtensionPrivacySettingHaiku, bool incognito);
    void setSessionValue(WebExtensionContext&, WebExtensionPrivacySettingHaiku, std::optional<bool>);
private:
    struct Winner {
        RefPtr<WebExtensionContext> context;
        std::optional<WebExtensionPrivacyValueHaiku> value;
    };
    bool isPrivate(WebPageProxy&) const;
    Winner winner(WebExtensionPrivacySettingHaiku, bool incognito);
    Ref<JSON::Object> details(WebExtensionContext&, WebExtensionPrivacySettingHaiku, bool incognito, const Winner&);
    void apply(WebPageProxy&, const Winner& prediction, const Winner& hyperlink);
    void dispatch(WebExtensionContext&, WebExtensionPrivacySettingHaiku, const String&, bool incognito);

    WebExtensionController& m_controller;
    WeakPtr<WebPageProxy> m_defaultBrowserPage;
    HashMap<String, WebExtensionPrivacyValuesHaiku> m_sessionValues;
    HashMap<uint64_t, std::array<String, 4>> m_eventDetails;
    std::array<bool, 2> m_defaults;
    bool m_hadPrivatePages { false };
    bool m_refreshing { false };
    bool m_refreshAgain { false };
    bool m_stopped { false };
};
}
#endif
