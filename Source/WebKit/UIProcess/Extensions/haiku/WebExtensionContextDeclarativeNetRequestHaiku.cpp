/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIContentRuleList.h"
#include "APIContentRuleListStore.h"
#include "APIData.h"
#include "WebExtensionConstants.h"
#include "WebExtensionDeclarativeNetRequestRulesHaiku.h"
#include "WebExtensionPermission.h"
#include "WebUserContentControllerProxy.h"
#include <wtf/NeverDestroyed.h>
#include <wtf/RunLoop.h>
#include <wtf/UUID.h>
#include <wtf/WorkQueue.h>
#include <wtf/text/MakeString.h>

namespace WebKit {
namespace {
struct RulesetSource { String identifier; String json; };
using Translation = std::expected<WebExtensionDeclarativeNetRequestTranslationHaiku, String>;
constexpr size_t maximumSourceLength = 64 * 1024 * 1024;
constexpr auto sessionRulesetID = "_session"_s;
constexpr auto dynamicRulesetID = "_dynamic"_s;
void discardRuleFile(String path)
{
    if (!path.isEmpty())
        API::ContentRuleListStore::defaultStoreSingleton().removeContentRuleListFile(WTF::move(path), [](std::error_code) { });
}
void translateRules(Vector<RulesetSource>&& input, CompletionHandler<void(Translation&&)>&& completion)
{
    static NeverDestroyed<Ref<WorkQueue>> queue(WorkQueue::create("WebKit DNR translation"_s));
    Vector<RulesetSource> sources;
    for (auto& source : input)
        sources.append({ source.identifier.isolatedCopy(), source.json.isolatedCopy() });
    // The completion owns engine references on main. Move it through this
    // worker without invoking or copying it, and destroy it only on main.
    queue.get()->dispatch([sources = WTF::move(sources), completion = WTF::move(completion)]() mutable {
        auto translate = [&]() -> Translation {
            Vector<WebExtensionDeclarativeNetRequestRulesetHaiku> rulesets;
            size_t length = 0;
            for (auto& source : sources) {
                if (source.json.length() > maximumSourceLength - length)
                    return makeUnexpected("DNR ruleset source size limit exceeded."_s);
                length += source.json.length();
                auto json = JSON::Value::parseJSON(source.json);
                auto array = json ? json->asArray() : nullptr;
                if (!array)
                    return makeUnexpected(makeString("Ruleset must contain a valid JSON array: "_s, source.identifier));
                bool isStatic = source.identifier != sessionRulesetID && source.identifier != dynamicRulesetID;
                rulesets.append({ source.identifier, array.releaseNonNull(), isStatic });
            }
            return translateDeclarativeNetRequestRulesHaiku(rulesets);
        };
        auto result = translate();
        if (result && result->skippedRuleCount)
            WTFLogAlways("Web Extension DNR: %zu static rules use conditions or actions this platform cannot enforce and were not loaded.", result->skippedRuleCount);
        Translation isolated = makeUnexpected(nullString());
        if (!result)
            isolated = makeUnexpected(result.error().isolatedCopy());
        else {
            WebExtensionDeclarativeNetRequestTranslationHaiku value;
            value.json = result->json.isolatedCopy();
            value.ruleCount = result->ruleCount;
            for (auto& entry : result->identifiers)
                value.identifiers.add(entry.key.isolatedCopy(), WTF::move(entry.value));
            isolated = WTF::move(value);
        }
        RunLoop::mainSingleton().dispatch([completion = WTF::move(completion), result = WTF::move(isolated)]() mutable {
            completion(WTF::move(result));
        });
    });
}
} // namespace

void WebExtensionContext::clearNativeDeclarativeNetRequestRules()
{
    ASSERT(RunLoop::isMain());
    RELEASE_ASSERT(++m_nativeDNRLoadGeneration);
    removeDeclarativeNetRequestRules();
    m_nativeDNRRules = nullptr;
    m_nativeDNRStaticRuleCount = 0;
    discardRuleFile(std::exchange(m_nativeDNRRuleFile, { }));
}

bool WebExtensionContext::nativeDeclarativeNetRequestAllowed()
{
    return hasPermission(WebExtensionPermission::declarativeNetRequest())
        || (hasPermission(WebExtensionPermission::declarativeNetRequestWithHostAccess()) && hasAccessToAllURLs());
}

void WebExtensionContext::loadDeclarativeNetRequestRules(CompletionHandler<void(bool)>&& completion)
{
    ASSERT(RunLoop::isMain());
    if (!isLoaded()) {
        completion(false);
        return;
    }
    // Host-access-only DNR needs action-level permission evaluation in the
    // request path. Do not install an unrestricted blocker in its place.
    if (!nativeDeclarativeNetRequestAllowed()) {
        clearNativeDeclarativeNetRequestRules();
        if (hasPermission(WebExtensionPermission::declarativeNetRequestWithHostAccess()))
            recordErrorIfNeeded(createError(Error::Unknown, "Host-access-only DNR requires native request permission integration."_s));
        completion(false);
        return;
    }
    RELEASE_ASSERT(++m_nativeDNRLoadGeneration);
    auto generation = m_nativeDNRLoadGeneration;
    auto loadIdentifier = privilegedIdentifier();
    auto current = [this, generation, loadIdentifier] {
        return isLoaded() && privilegedIdentifier() == loadIdentifier && generation == m_nativeDNRLoadGeneration
            && nativeDeclarativeNetRequestAllowed();
    };
    auto fail = [this](const String& message) {
        recordErrorIfNeeded(createError(Error::Unknown, message));
    };
    auto sources = std::make_shared<Vector<RulesetSource>>();
    auto readError = std::make_shared<String>();
    auto loadStatic = [this, protectedThis = Ref { *this }, current, fail, sources, readError, completion = WTF::move(completion)]() mutable {
        if (!current()) { completion(false); return; }
        if (!readError->isEmpty()) { fail(*readError); completion(false); return; }
        size_t length = 0;
        for (auto& source : *sources)
            length += source.json.length();
        for (auto& ruleset : extension().declarativeNetRequestRulesets()) {
            if (!m_enabledStaticRulesetIDs.contains(ruleset.rulesetID))
                continue;
            auto data = extension().resourceDataForPath(ruleset.jsonPath);
            if (!data) {
                recordErrorIfNeeded(data.error());
                completion(false);
                return;
            }
            if (length > maximumSourceLength || (*data)->span().size() > maximumSourceLength - length) {
                fail("DNR ruleset source size limit exceeded."_s); completion(false); return;
            }
            auto json = String::fromUTF8((*data)->span());
            if (json.isNull()) {
                fail("DNR ruleset is not valid UTF-8."_s); completion(false); return;
            }
            length += json.length();
            sources->append({ ruleset.rulesetID, WTF::move(json) });
        }
        translateRules(WTF::move(*sources), [this, protectedThis = Ref { *this }, current, fail, completion = WTF::move(completion)](Translation&& translation) mutable {
            if (!current()) { completion(false); return; }
            if (!translation) { fail(translation.error()); completion(false); return; }
            auto sessionIDs = translation->identifiers.take(String(sessionRulesetID));
            auto dynamicIDs = translation->identifiers.take(String(dynamicRulesetID));
            size_t staticRuleCount = 0;
            for (auto& entry : translation->identifiers)
                staticRuleCount += entry.value.size();
            if (!translation->ruleCount) {
                removeDeclarativeNetRequestRules();
                m_nativeDNRRules = nullptr;
                m_sessionRulesIDs = WTF::move(sessionIDs);
                m_dynamicRulesIDs = WTF::move(dynamicIDs);
                m_nativeDNRStaticRuleCount = staticRuleCount;
                auto oldFile = std::exchange(m_nativeDNRRuleFile, { });
                if (oldFile.isEmpty()) { completion(true); return; }
                API::ContentRuleListStore::defaultStoreSingleton().removeContentRuleListFile(WTF::move(oldFile),
                    [protectedThis = Ref { *this }, current, completion = WTF::move(completion)](std::error_code error) mutable {
                        completion(current() && !error);
                    });
                return;
            }
            auto basePath = declarativeNetRequestContentRuleListFilePath();
            if (basePath.isEmpty()) { fail("DNR rule cache directory is unavailable."_s); completion(false); return; }
            // Every compilation owns a unique path. A stale completion can
            // neither overwrite nor delete a newer load's compiled rule file.
            String path = makeString(basePath, '.', WTF::UUID::createVersion4().toString());
            API::ContentRuleListStore::defaultStoreSingleton().compileContentRuleListFile(String(path), uniqueIdentifier().isolatedCopy(), WTF::move(translation->json),
                WebCore::ContentExtensions::CSSSelectorsAllowed::No,
                [this, protectedThis = Ref { *this }, current, fail, path, staticRuleCount, sessionIDs = WTF::move(sessionIDs), dynamicIDs = WTF::move(dynamicIDs), completion = WTF::move(completion)](RefPtr<API::ContentRuleList> rules, std::error_code error) mutable {
                    if (!current() || error || !rules) {
                        discardRuleFile(path);
                        if (current())
                            fail("DNR rules could not be compiled; the previous rules remain installed."_s);
                        completion(false);
                        return;
                    }
                    auto oldFile = std::exchange(m_nativeDNRRuleFile, path);
                    m_nativeDNRRules = rules;
                    m_sessionRulesIDs = WTF::move(sessionIDs);
                    m_dynamicRulesIDs = WTF::move(dynamicIDs);
                    m_nativeDNRStaticRuleCount = staticRuleCount;
                    for (Ref controller : userContentControllers())
                        controller->addContentRuleList(*rules, m_baseURL);
                    discardRuleFile(WTF::move(oldFile));
                    completion(true);
                });
        });
    };
    auto loadDynamic = [this, protectedThis = Ref { *this }, current, sources, readError, loadStatic = WTF::move(loadStatic)]() mutable {
        if (!current() || !readError->isEmpty()) { loadStatic(); return; }
        declarativeNetRequestDynamicRulesStore()->getRulesWithRuleIDs({ },
            [current, sources, readError, loadStatic = WTF::move(loadStatic)](RefPtr<JSON::Array> rules, const String& error) mutable {
                if (!current()) { loadStatic(); return; }
                if (!error.isEmpty() || !rules) {
                    *readError = error.isEmpty() ? "Dynamic DNR rules could not be read."_s : error;
                } else
                    sources->append({ String(dynamicRulesetID), rules->toJSONString() });
                loadStatic();
            });
    };
    declarativeNetRequestSessionRulesStore()->getRulesWithRuleIDs({ },
        [current, sources, readError, loadDynamic = WTF::move(loadDynamic)](RefPtr<JSON::Array> rules, const String& error) mutable {
            if (!current()) { loadDynamic(); return; }
            if (!error.isEmpty() || !rules) {
                *readError = error.isEmpty() ? "Session DNR rules could not be read."_s : error;
            } else
                sources->append({ String(sessionRulesetID), rules->toJSONString() });
            loadDynamic();
        });
}
} // namespace WebKit
#endif
