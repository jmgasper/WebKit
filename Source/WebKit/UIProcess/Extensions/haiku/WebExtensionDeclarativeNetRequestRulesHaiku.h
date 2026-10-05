/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#if ENABLE(WK_WEB_EXTENSIONS) && ENABLE(CONTENT_EXTENSIONS)
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/JSONValues.h>
#include <wtf/Vector.h>
#include <expected>

namespace WebKit {
struct WebExtensionDeclarativeNetRequestRulesetHaiku {
    String identifier;
    Ref<JSON::Array> rules;
    // Static (manifest) rulesets: leave out rules that cannot be represented
    // instead of failing the translation. Dynamic/session updates stay strict.
    bool skipUnsupportedRules { false };
};
struct WebExtensionDeclarativeNetRequestTranslationHaiku {
    String json;
    HashMap<String, HashSet<double>> identifiers;
    size_t ruleCount { 0 };
    size_t skippedRuleCount { 0 };
};
struct WebExtensionDeclarativeNetRequestLimitsHaiku {
    size_t maximumRules { 300000 };
    size_t maximumOutputLength { 64 * 1024 * 1024 };
};
// Every input rule is validated. Unsupported actions/conditions fail the whole
// translation, except in rulesets marked skipUnsupportedRules, where such rules
// are counted and left out. Callers retain the previous rule list on error.
std::expected<WebExtensionDeclarativeNetRequestTranslationHaiku, String> translateDeclarativeNetRequestRulesHaiku(
    const Vector<WebExtensionDeclarativeNetRequestRulesetHaiku>&, WebExtensionDeclarativeNetRequestLimitsHaiku = { });
} // namespace WebKit
#endif
