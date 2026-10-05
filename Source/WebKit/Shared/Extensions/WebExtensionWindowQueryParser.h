/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionWindow.h"
#include <expected>
#include <wtf/JSONValues.h>
namespace WebKit {
struct WebExtensionWindowQuery {
    WebExtensionWindow::PopulateTabs populate { WebExtensionWindow::PopulateTabs::No };
    OptionSet<WebExtensionWindowTypeFilter> types { allWebExtensionWindowTypeFilters() };
};
enum class WebExtensionWindowQueryKind : uint8_t { Get, Event };
std::expected<WebExtensionWindowQuery, String> parseWebExtensionWindowQuery(const JSON::Value*, WebExtensionWindowQueryKind);
}
#endif
