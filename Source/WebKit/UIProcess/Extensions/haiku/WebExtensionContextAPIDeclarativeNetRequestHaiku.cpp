/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "WebExtensionConstants.h"
#include "WebExtensionDeclarativeNetRequestRulesHaiku.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include <wtf/text/MakeString.h>

// C++ counterpart of Cocoa/API/WebExtensionContextAPIDeclarativeNetRequestCocoa.mm
// for the parts the native content-rule path can honour. Dynamic and session
// rule storage is shared with the other ports in API/WebExtensionContextAPIDeclarativeNetRequest.cpp.

namespace WebKit {

static constexpr auto declarativeNetRequestRulesetStateKey = "DeclarativeNetRequestRulesetState"_s;
static constexpr auto validationRulesetIdentifier = "_validate"_s;

void WebExtensionContext::saveDeclarativeNetRequestRulesetStateToStorage(const HashMap<String, bool>& rulesetState)
{
    ASSERT(m_state);
    RefPtr saved = m_state->getObject(declarativeNetRequestRulesetStateKey);
    Ref updated = JSON::Object::create();
    if (saved) {
        for (auto& entry : *saved) {
            if (auto enabled = entry.value->asBoolean())
                updated->setBoolean(entry.key, *enabled);
        }
    }
    for (auto& entry : rulesetState)
        updated->setBoolean(entry.key, entry.value);
    m_state->setObject(declarativeNetRequestRulesetStateKey, WTF::move(updated));
    writeStateToStorage();
}

void WebExtensionContext::declarativeNetRequestUpdateEnabledRulesets(const Vector<String>& rulesetIdentifiersToEnable, const Vector<String>& rulesetIdentifiersToDisable, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&& completionHandler)
{
    static constexpr auto apiName = "declarativeNetRequest.updateEnabledRulesets()"_s;

    if (rulesetIdentifiersToEnable.isEmpty() && rulesetIdentifiersToDisable.isEmpty()) {
        completionHandler({ });
        return;
    }

    auto validatedRulesetsToEnable = declarativeNetRequestValidateRulesetIdentifiers(rulesetIdentifiersToEnable);
    if (!validatedRulesetsToEnable) {
        completionHandler(makeUnexpected(validatedRulesetsToEnable.error()));
        return;
    }

    auto validatedRulesetsToDisable = declarativeNetRequestValidateRulesetIdentifiers(rulesetIdentifiersToDisable);
    if (!validatedRulesetsToDisable) {
        completionHandler(makeUnexpected(validatedRulesetsToDisable.error()));
        return;
    }

    // Count the resulting set; identifiers may repeat or already be in the requested state.
    auto updated = m_enabledStaticRulesetIDs;
    for (auto& identifier : rulesetIdentifiersToDisable)
        updated.remove(identifier);
    for (auto& identifier : rulesetIdentifiersToEnable)
        updated.add(identifier);
    if (updated.size() > webExtensionDeclarativeNetRequestMaximumNumberOfEnabledRulesets) {
        completionHandler(toWebExtensionError(apiName, nullString(), makeString("The number of enabled static rulesets exceeds the limit. Only "_s, webExtensionDeclarativeNetRequestMaximumNumberOfEnabledRulesets, " rulesets can be enabled at once."_s)));
        return;
    }

    if (!nativeDeclarativeNetRequestAllowed()) {
        completionHandler(toWebExtensionError(apiName, nullString(), "Rules of an extension with only the 'declarativeNetRequestWithHostAccess' permission are not enforced on this platform"_s));
        return;
    }

    HashMap<String, bool> changedState;
    for (auto& identifier : rulesetIdentifiersToDisable)
        changedState.set(identifier, false);
    for (auto& identifier : rulesetIdentifiersToEnable)
        changedState.set(identifier, true);

    auto previous = std::exchange(m_enabledStaticRulesetIDs, WTF::move(updated));
    loadDeclarativeNetRequestRules([this, protectedThis = Ref { *this }, completionHandler = WTF::move(completionHandler), changedState = WTF::move(changedState), previous = WTF::move(previous)](bool success) mutable {
        if (success) {
            saveDeclarativeNetRequestRulesetStateToStorage(changedState);
            completionHandler({ });
            return;
        }

        // A failed load leaves the previously compiled rules installed. Restore
        // the matching ruleset state; nothing was saved to disk.
        m_enabledStaticRulesetIDs = WTF::move(previous);
        completionHandler(toWebExtensionError(apiName, nullString(), "Failed to apply rules."_s));
    });
}

void WebExtensionContext::declarativeNetRequestGetAvailableStaticRuleCount(CompletionHandler<void(double)>&& completionHandler)
{
    size_t guaranteed = webExtensionDeclarativeNetRequestGuaranteedMinimumStaticRules;
    completionHandler(m_nativeDNRStaticRuleCount >= guaranteed ? 0 : static_cast<double>(guaranteed - m_nativeDNRStaticRuleCount));
}

std::expected<void, WebExtensionError> WebExtensionContext::validateNativeDeclarativeNetRequestUpdate(const String& apiName, const JSON::Array& rulesToAdd, const Vector<double>& ruleIDsToDelete, bool session)
{
    // Host-access-only DNR needs action-level permission checks in the request
    // path, which the native loader does not have; it installs no rules then.
    if (!nativeDeclarativeNetRequestAllowed())
        return toWebExtensionError(apiName, nullString(), "Rules of an extension with only the 'declarativeNetRequestWithHostAccess' permission are not enforced on this platform"_s);

    auto remaining = session ? m_sessionRulesIDs : m_dynamicRulesIDs;
    for (auto identifier : ruleIDsToDelete)
        remaining.remove(identifier);
    auto otherCount = session ? m_dynamicRulesIDs.size() : m_sessionRulesIDs.size();
    if (remaining.size() + rulesToAdd.length() + otherCount > webExtensionDeclarativeNetRequestMaximumNumberOfDynamicAndSessionRules)
        return toWebExtensionError(apiName, nullString(), "Maximum number of dynamic and session rules exceeded"_s);

    // Translate each rule alone with the translator the loader uses. A rule
    // passes only if the native content-rule path can enforce it as written.
    HashSet<double> added;
    size_t index = 0;
    for (Ref rule : rulesToAdd) {
        Ref single = JSON::Array::create();
        single->pushValue(rule.copyRef());
        Vector<WebExtensionDeclarativeNetRequestRulesetHaiku> rulesets;
        rulesets.append({ String(validationRulesetIdentifier), WTF::move(single) });
        auto translation = translateDeclarativeNetRequestRulesHaiku(rulesets);
        if (!translation) {
            auto reason = translation.error();
            auto prefix = makeString("Ruleset "_s, validationRulesetIdentifier, ": "_s);
            if (reason.startsWith(prefix))
                reason = reason.substring(prefix.length());
            else if (reason.startsWith(makeString("Ruleset "_s, validationRulesetIdentifier, ' ')))
                reason = "The rule is not an object"_s;
            return toWebExtensionError(apiName, nullString(), makeString("Rule at index "_s, index, " is invalid: "_s, reason));
        }
        auto identifiers = translation->identifiers.get(String(validationRulesetIdentifier));
        for (auto identifier : identifiers) {
            if (remaining.contains(identifier) || !added.add(identifier).isNewEntry)
                return toWebExtensionError(apiName, nullString(), makeString("Rule with id "_s, static_cast<uint64_t>(identifier), " does not have a unique ID"_s));
        }
        ++index;
    }
    return { };
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
