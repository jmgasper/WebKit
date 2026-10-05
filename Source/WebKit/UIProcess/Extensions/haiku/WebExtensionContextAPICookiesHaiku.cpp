/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionContext.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include "APIHTTPCookieStore.h"
#include "WebExtensionCookieHostAccess.h"
#include "WebExtensionCookieWriteParser.h"
#include "WebExtensionContextProxyMessages.h"
#include "WebExtensionPermission.h"
#include "WebExtensionUtilities.h"
#include "WebPageProxy.h"
#include "WebsiteDataStore.h"
#include <algorithm>
#include <cmath>
#include <wtf/text/MakeString.h>
namespace WebKit {
static bool validCookieURL(const URL& url)
{
    return url.isValid() && url.protocolIsInHTTPFamily() && !url.host().isEmpty();
}
static bool hasCookieHostPermission(WebExtensionContext& context, const URL& url)
{
    if (!validCookieURL(url))
        return false;
    // Cookie access is profile-wide. Do not use tab metadata permissions or
    // temporary activeTab grants. Host permissions ignore match-pattern paths.
    auto& granted = context.grantedPermissionMatchPatterns();
    auto& denied = context.deniedPermissionMatchPatterns();
    return hasWebExtensionCookieHostAccess(url, granted, denied, WallTime::now());
}
static bool isCurrentCookieRequest(WebExtensionContext& context, WebsiteDataStore& store, WebExtensionContextIdentifier loadIdentifier, const URL& url)
{
    return context.isLoaded() && context.privilegedIdentifier() == loadIdentifier
        && context.hasPermission(WebExtensionPermission::cookies()) && context.websiteDataStore(store.sessionID()) == &store
        && (url.isEmpty() || hasCookieHostPermission(context, url));
}
static String normalizedCookieDomain(String domain)
{
    if (domain.startsWith('.'))
        domain = domain.substring(1);
    if (domain.startsWith('[') && domain.endsWith(']'))
        domain = domain.substring(1, domain.length() - 2);
    return domain.convertToASCIILowercase();
}
static URL cookieOrigin(const WebCore::Cookie& cookie)
{
    auto domain = normalizedCookieDomain(cookie.domain);
    if (domain.isEmpty() || domain.contains('/') || domain.contains('\\') || domain.contains('@')
        || domain.contains('?') || domain.contains('#') || domain.contains('\0'))
        return { };
    if (domain.contains(':'))
        domain = makeString('[', domain, ']');
    return URL { makeString(cookie.secure ? "https://"_s : "http://"_s, domain, '/') };
}
void WebExtensionContext::cookiesDidChange(API::HTTPCookieStore& cookieStore, const Vector<WebCore::CookieChange>& changes)
{
    if (!isLoaded() || !hasPermission(WebExtensionPermission::cookies()))
        return;
    RefPtr store = websiteDataStore(cookieStore.sessionID());
    if (!store || &store->cookieStore() != &cookieStore)
        return;
    Vector<WebCore::CookieChange> permittedChanges;
    for (auto& change : changes) {
        if (change.cookie.partitionKey.isEmpty() && hasCookieHostPermission(*this, cookieOrigin(change.cookie)))
            permittedChanges.append(change);
    }
    if (permittedChanges.isEmpty())
        return;
    auto loadIdentifier = privilegedIdentifier();
    constexpr auto type = WebExtensionEventListenerType::CookiesOnChanged;
    wakeUpBackgroundContentIfNecessaryToFireEvents({ type }, [this, protectedThis = Ref { *this }, store = Ref { *store }, loadIdentifier, changes = WTF::move(permittedChanges)]() mutable {
        if (!isCurrentCookieRequest(*this, store.get(), loadIdentifier, { }))
            return;
        changes.removeAllMatching([&](auto& change) {
            return !hasCookieHostPermission(*this, cookieOrigin(change.cookie));
        });
        if (!changes.isEmpty())
            sendToProcessesForEvent(type, Messages::WebExtensionContextProxy::DispatchCookiesChangedEvent(store->sessionID(), changes));
    });
}

WebsiteDataStore* WebExtensionContext::websiteDataStore(std::optional<PAL::SessionID> session) const
{
    RefPtr controller = extensionController();
    if (!controller || (session && !session->isValid()))
        return nullptr;
    RefPtr store = controller->websiteDataStore(session);
    if (store && !store->isPersistent() && !hasAccessToPrivateData()
        && store.get() != &controller->configuration().defaultWebsiteDataStore())
        return nullptr;
    return store.get();
}
bool WebExtensionContext::isCookiesMessageAllowed(IPC::Decoder& message)
{
    return isLoadedAndPrivilegedMessage(message) && hasPermission(WebExtensionPermission::cookies());
}
void WebExtensionContext::fetchCookies(WebsiteDataStore& store, const URL& url, const WebExtensionCookieFilterParameters& filter, bool requireCookieHostPermission, CompletionHandler<void(std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&&)>&& completion)
{
    if (!isLoaded() || !hasPermission(WebExtensionPermission::cookies()) || websiteDataStore(store.sessionID()) != &store
        || (!url.isEmpty() && !hasCookieHostPermission(*this, url))) {
        completion(toWebExtensionError("cookies"_s, nullString(), "cookie store or host permission is unavailable"_s));
        return;
    }
    for (auto* value : { &filter.name, &filter.domain, &filter.path }) {
        if (*value && (*value)->contains('\0')) {
            completion(toWebExtensionError("cookies"_s, nullString(), "invalid cookie filter"_s));
            return;
        }
    }
    auto loadIdentifier = privilegedIdentifier();
    auto received = [this, protectedThis = Ref { *this }, store = Ref { store }, url, filter, requireCookieHostPermission, loadIdentifier, completion = WTF::move(completion)](Vector<WebCore::Cookie>&& cookies) mutable {
        if (!isCurrentCookieRequest(*this, store.get(), loadIdentifier, url)) {
            completion(toWebExtensionError("cookies"_s, nullString(), "cookie access changed while the request was pending"_s));
            return;
        }
        Vector<WebExtensionCookieParameters> result;
        auto now = WallTime::now().secondsSinceEpoch().milliseconds();
        for (auto& cookie : cookies) {
            if (!cookie.partitionKey.isEmpty() || (!cookie.session && (!cookie.expires || !std::isfinite(*cookie.expires) || *cookie.expires <= now)))
                continue;
            if ((filter.name && cookie.name != *filter.name) || (filter.path && cookie.path != *filter.path)
                || (filter.secure && cookie.secure != *filter.secure) || (filter.session && cookie.session != *filter.session))
                continue;
            if (filter.domain) {
                auto domain = normalizedCookieDomain(cookie.domain);
                auto requestedDomain = normalizedCookieDomain(*filter.domain);
                if (requestedDomain.isEmpty() || (domain != requestedDomain && !domain.endsWith(makeString('.', requestedDomain))))
                    continue;
            }
            if (requireCookieHostPermission && !hasCookieHostPermission(*this, cookieOrigin(cookie)))
                continue;
            result.append({ store->sessionID(), WTF::move(cookie) });
        }
        std::stable_sort(result.begin(), result.end(), [](auto& first, auto& second) {
            if (first.cookie.path.length() != second.cookie.path.length())
                return first.cookie.path.length() > second.cookie.path.length();
            auto firstCreated = std::isfinite(first.cookie.created) ? first.cookie.created : 0;
            auto secondCreated = std::isfinite(second.cookie.created) ? second.cookie.created : 0;
            return firstCreated < secondCreated;
        });
        completion(WTF::move(result));
    };
    if (url.isEmpty())
        protect(store.cookieStore())->cookies(WTF::move(received));
    else
        protect(store.cookieStore())->cookiesForURL(url.isolatedCopy(), WTF::move(received));
}
void WebExtensionContext::cookiesGet(std::optional<PAL::SessionID> session, const String& name, const URL& url, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&& completion)
{
    RefPtr store = websiteDataStore(session);
    if (!store || !validCookieURL(url) || name.contains('\0')) {
        completion(toWebExtensionError("cookies.get()"_s, nullString(), "invalid cookie store, name or URL"_s));
        return;
    }
    WebExtensionCookieFilterParameters filter;
    filter.name = name;
    fetchCookies(*store, url, filter, false, [completion = WTF::move(completion)](auto&& result) mutable {
        if (!result)
            completion(std::unexpected(result.error()));
        else if (result->isEmpty())
            completion({ std::nullopt });
        else
            completion({ WTF::move(result->first()) });
    });
}
void WebExtensionContext::cookiesGetAll(std::optional<PAL::SessionID> session, const URL& url, const WebExtensionCookieFilterParameters& filter, CompletionHandler<void(std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&&)>&& completion)
{
    RefPtr store = websiteDataStore(session);
    if (!store || (!url.isEmpty() && !validCookieURL(url))) {
        completion(toWebExtensionError("cookies.getAll()"_s, nullString(), "invalid cookie store or URL"_s));
        return;
    }
    fetchCookies(*store, url, filter, true, WTF::move(completion));
}
void WebExtensionContext::cookiesSetJSON(const String& details, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&& completion)
{
    auto parsed = parseWebExtensionCookieWrite(JSON::Value::parseJSON(details).get());
    if (!parsed) {
        completion(toWebExtensionError("cookies.set()"_s, nullString(), parsed.error()));
        return;
    }
    RefPtr store = websiteDataStore(parsed->session);
    if (!store || !hasPermission(WebExtensionPermission::cookies()) || !hasCookieHostPermission(*this, parsed->url)) {
        completion(toWebExtensionError("cookies.set()"_s, nullString(), "cookie store or host permission is unavailable"_s));
        return;
    }
    auto loadIdentifier = privilegedIdentifier();
    store->cookieStore().setCookieWithResult(parsed->cookie, [this, protectedThis = Ref { *this }, store, loadIdentifier, url = parsed->url, completion = WTF::move(completion)](bool success, std::optional<WebCore::Cookie>&& cookie) mutable {
        if (!isCurrentCookieRequest(*this, *store, loadIdentifier, url)) {
            completion(toWebExtensionError("cookies.set()"_s, nullString(), "cookie access changed while the request was pending"_s));
            return;
        }
        if (!success) {
            completion(toWebExtensionError("cookies.set()"_s, nullString(), "cookie write could not be confirmed"_s));
            return;
        }
        if (!cookie)
            completion({ std::nullopt });
        else
            completion({ WebExtensionCookieParameters { store->sessionID(), WTF::move(*cookie) } });
    });
}
void WebExtensionContext::cookiesRemove(std::optional<PAL::SessionID> session, const String& name, const URL& url, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&& completion)
{
    RefPtr store = websiteDataStore(session);
    if (!store || !validCookieURL(url) || name.contains('\0')) {
        completion(toWebExtensionError("cookies.remove()"_s, nullString(), "invalid cookie store, name or URL"_s));
        return;
    }
    auto loadIdentifier = privilegedIdentifier();
    WebExtensionCookieFilterParameters filter;
    filter.name = name;
    fetchCookies(*store, url, filter, false, [this, protectedThis = Ref { *this }, store, url, loadIdentifier, completion = WTF::move(completion)](std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&& result) mutable {
        if (!result) {
            completion(std::unexpected(result.error()));
            return;
        }
        if (!isCurrentCookieRequest(*this, *store, loadIdentifier, url)) {
            completion(toWebExtensionError("cookies.remove()"_s, nullString(), "cookie access changed while the request was pending"_s));
            return;
        }
        if (result->isEmpty()) {
            completion({ std::nullopt });
            return;
        }
        auto cookie = WTF::move(result->first());
        // Keep a distinct key copy: the callback captures the selected cookie.
        auto key = cookie.cookie;
        store->cookieStore().deleteCookieWithResult(key, [this, protectedThis = Ref { *this }, store, url, loadIdentifier, cookie = WTF::move(cookie), completion = WTF::move(completion)](bool success) mutable {
            if (!isCurrentCookieRequest(*this, *store, loadIdentifier, url))
                completion(toWebExtensionError("cookies.remove()"_s, nullString(), "cookie access changed while the request was pending"_s));
            else if (!success)
                completion(toWebExtensionError("cookies.remove()"_s, nullString(), "cookie deletion could not be confirmed"_s));
            else
                completion({ WTF::move(cookie) });
        });
    });
}
void WebExtensionContext::cookiesGetAllCookieStores(CompletionHandler<void(std::expected<HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>>, WebExtensionError>&&)>&& completion)
{
    RefPtr controller = extensionController();
    if (!controller || !hasPermission(WebExtensionPermission::cookies())) {
        completion(toWebExtensionError("cookies.getAllCookieStores()"_s, nullString(), "cookie permission is unavailable"_s));
        return;
    }
    HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>> stores;
    stores.set(controller->configuration().defaultWebsiteDataStore().sessionID(), Vector<WebExtensionTabIdentifier> { });
    for (Ref tab : openTabs()) {
        if (!tab->isOpen() || !tab->extensionHasAccess())
            continue;
        RefPtr page = tab->nativePage();
        if (!page)
            continue;
        auto session = page->websiteDataStore().sessionID();
        if (websiteDataStore(session) != &page->websiteDataStore())
            continue;
        stores.ensure(session, [] { return Vector<WebExtensionTabIdentifier> { }; }).iterator->value.append(tab->identifier());
    }
    completion(WTF::move(stores));
}
}
#endif
