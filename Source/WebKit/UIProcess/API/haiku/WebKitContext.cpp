/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

#include "config.h"
#include "WebKitContext.h"

#include "APIPageConfiguration.h"
#include "BrowserCommandBrokerHaiku.h"
#include "BrowserTabRegistryHaiku.h"
#include <cmath>
#include "DownloadRegistryHaiku.h"
#include "ExtensionPermissionPromptHaiku.h"
#include "ExtensionPackageRegistryHaiku.h"
#include <limits>
#include "WebPageProxy.h"
#include "WebViewPrivate.h"
#include "WebViewStateHaiku.h"
#include <MessageRunner.h>
#include "WebKitView.h"
#include "WebKitEmbedding.h"
#include "WebsiteDataRecord.h"
#include "WebsiteDataStore.h"
#include "WebsiteDataType.h"
#include <Application.h>
#include <cstdlib>
#include <wtf/FileSystem.h>
#include <wtf/RunLoop.h>
#include <wtf/Threading.h>
#include "WebViewContextHaiku.h"
#include "SitePermissionsHaiku.h"

BWebKitContext::BWebKitContext(const char* profilePath, bool privateBrowsing)
    : m_private(privateBrowsing)
{
    m_status = BWebKitInitialize();
    if (m_status == B_OK)
        m_context = WebKit::WebViewContextHaiku::create(profilePath, privateBrowsing, m_status);
}

BWebKitContext::BWebKitContext(const std::shared_ptr<BWebKitContext>& extensionHost)
    : m_private(true)
{
    m_status = BWebKitInitialize();
    if (m_status != B_OK)
        return;
    if (!extensionHost || extensionHost->InitCheck() != B_OK || extensionHost->IsPrivate()) {
        m_status = B_BAD_VALUE;
        return;
    }
    // Like the other constructor, on the application thread (WebKit's main).
    m_context = WebKit::WebViewContextHaiku::createPrivate(extensionHost->m_context);
    if (!m_context)
        m_status = B_ERROR;
}

BWebKitContext::~BWebKitContext() = default;

status_t BWebKitContext::InitCheck() const { return m_status; }
bool BWebKitContext::IsPrivate() const { return m_private; }

status_t BWebKitContext::PrepareExtension(const char* path, const BMessenger& reply, uint64 identifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!path || !identifier || identifier == std::numeric_limits<uint64>::max())
        return B_BAD_VALUE;
    if (!reply.IsValid() || !reply.IsTargetLocal())
        return B_BAD_VALUE;
    auto copiedPath = String::fromUTF8(path).isolatedCopy();
    if (copiedPath.isEmpty() || !copiedPath.startsWith('/'))
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto operation = [context = m_context, path = WTF::move(copiedPath), reply, identifier]() mutable {
        context->extensionPackages()->prepare(WTF::move(path), reply, identifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    return B_NOT_SUPPORTED;
#endif
}

void BWebKitContext::CancelExtensionPreparation(uint64 identifier)
{
#if ENABLE(WK_WEB_EXTENSIONS)
    if (m_status == B_OK) {
        auto operation = [context = m_context, identifier] {
            context->extensionPackages()->cancel(identifier);
        };
        if (RunLoop::isMain())
            operation();
        else
            RunLoop::mainSingleton().dispatch(WTF::move(operation));
    }
#else
    UNUSED_PARAM(identifier);
#endif
}

void BWebKitContext::DiscardPreparedExtension(const char* token, const BMessenger& reply, uint64 identifier)
{
#if ENABLE(WK_WEB_EXTENSIONS)
    if (m_status == B_OK && token && (!reply.IsValid() || reply.IsTargetLocal())) {
        auto copiedToken = String::fromUTF8(token).isolatedCopy();
        auto operation = [context = m_context, token = WTF::move(copiedToken), reply, identifier] {
            context->extensionPackages()->discard(token, reply, identifier);
        };
        if (RunLoop::isMain())
            operation();
        else
            RunLoop::mainSingleton().dispatch(WTF::move(operation));
    }
#else
    UNUSED_PARAM(token);
    UNUSED_PARAM(reply);
    UNUSED_PARAM(identifier);
#endif
}

bool BWebKitContext::HasPendingExtensionPreparations() const
{
#if ENABLE(WK_WEB_EXTENSIONS)
    return m_context && m_context->extensionPackages()->hasPendingPreparations();
#else
    return false;
#endif
}

status_t BWebKitContext::LoadPreparedExtension(const char* token, const BWebKitExtensionLoadOptions& options, const BMessenger& reply, uint64 identifier)
{
    return LoadPreparedExtension(token, options, BWebKitExtensionInstallation { }, reply, identifier);
}

status_t BWebKitContext::LoadPreparedExtension(const char* token, const BWebKitExtensionLoadOptions& options,
    const BWebKitExtensionInstallation& installation, const BMessenger& reply, uint64 identifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!token || !*token || !identifier || identifier == std::numeric_limits<uint64>::max()
        || !reply.IsValid() || !reply.IsTargetLocal())
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto copiedToken = String::fromUTF8(token).isolatedCopy();
    if (copiedToken.isEmpty())
        return B_BAD_VALUE;
    auto operation = [context = m_context, token = WTF::move(copiedToken), options, installation, reply, identifier] {
        context->extensionPackages()->load(token, options, installation, reply, identifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    UNUSED_PARAM(options);
    UNUSED_PARAM(installation);
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::SetExtensionPrivateAccess(const char* extensionIdentifier, bool allowed)
{
    if (m_status != B_OK)
        return m_status;
    if (!extensionIdentifier || !*extensionIdentifier)
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto name = String::fromUTF8(extensionIdentifier).isolatedCopy();
    if (name.isEmpty())
        return B_BAD_VALUE;
    auto operation = [context = m_context, name = WTF::move(name), allowed] {
        context->extensionPackages()->setPrivateAccess(name, allowed);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    UNUSED_PARAM(allowed);
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::PreloadExtensionAction(const char* extensionIdentifier, const BMessenger& window,
    uint64 loadIdentifier, uint64 pageIdentifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!extensionIdentifier || !*extensionIdentifier || !window.IsValid() || !window.IsTargetLocal() || !loadIdentifier || !pageIdentifier)
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto name = String::fromUTF8(extensionIdentifier).isolatedCopy();
    if (name.isEmpty())
        return B_BAD_VALUE;
    auto operation = [context = m_context, name = WTF::move(name), window, loadIdentifier, pageIdentifier] {
        context->extensionPackages()->preloadAction(name, window, loadIdentifier, pageIdentifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    return B_NOT_SUPPORTED;
#endif
}

BWebKitView* BWebKitContext::CreateExtensionView(BRect frame, const char* name, const char* extensionIdentifier,
    const BMessenger& target, status_t* error)
{
    auto fail = [&](status_t status) -> BWebKitView* {
        if (error)
            *error = status;
        return nullptr;
    };
    if (m_status != B_OK)
        return fail(m_status);
    if (!RunLoop::isMain())
        return fail(B_BAD_THREAD_ID);
    if (!extensionIdentifier || !*extensionIdentifier || !target.IsValid() || !target.IsTargetLocal()
        || !frame.IsValid() || !std::isfinite(frame.left) || !std::isfinite(frame.top)
        || !std::isfinite(frame.right) || !std::isfinite(frame.bottom) || std::ceil(frame.Width()) + 1 > 8192 || std::ceil(frame.Height()) + 1 > 8192)
        return fail(B_BAD_VALUE);
#if ENABLE(WK_WEB_EXTENSIONS)
    auto configuration = m_context->extensionPackages()->viewConfiguration(String::fromUTF8(extensionIdentifier));
    if (!configuration)
        return fail(B_NAME_NOT_FOUND);
    auto state = std::make_shared<WebKit::WebViewStateHaiku>(target);
    state->context = m_context;
    state->status = B_OK;
    state->geometry.size = { static_cast<int>(std::ceil(frame.Width()) + 1), static_cast<int>(std::ceil(frame.Height()) + 1) };
    state->client = WebKit::WebView::createForExtensionPage(state, configuration.releaseNonNull());
    auto* view = new BWebKitView(frame, name, std::move(state));
    if (error)
        *error = view->InitCheck();
    return view;
#else
    UNUSED_PARAM(name);
    return fail(B_NOT_SUPPORTED);
#endif
}

status_t BWebKitContext::UnloadExtension(const char* extensionIdentifier, const BMessenger& reply, uint64 identifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!extensionIdentifier || !*extensionIdentifier || !identifier || identifier == std::numeric_limits<uint64>::max()
        || !reply.IsValid() || !reply.IsTargetLocal())
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto copiedIdentifier = String::fromUTF8(extensionIdentifier).isolatedCopy();
    if (copiedIdentifier.isEmpty())
        return B_BAD_VALUE;
    auto operation = [context = m_context, extensionIdentifier = WTF::move(copiedIdentifier), reply, identifier] {
        context->extensionPackages()->unload(extensionIdentifier, reply, identifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::SetExtensionActionListener(const BMessenger& listener)
{
    if (m_status != B_OK)
        return m_status;
    if (!RunLoop::isMain())
        return B_BAD_THREAD_ID;
#if ENABLE(WK_WEB_EXTENSIONS)
    return m_context->extensionPackages()->setActionListener(listener);
#else
    UNUSED_PARAM(listener);
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::GetExtensionActions(const BMessenger& window, const BMessenger& reply, uint64 identifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!window.IsValid() || !window.IsTargetLocal() || !reply.IsValid() || !reply.IsTargetLocal()
        || !identifier || identifier == std::numeric_limits<uint64>::max())
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto operation = [context = m_context, window, reply, identifier] {
        context->extensionPackages()->actions(window, reply, identifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::ActivateExtensionAction(const char* extensionIdentifier, const BMessenger& window,
    uint64 loadIdentifier, uint64 pageIdentifier, const BMessenger& reply, uint64 identifier)
{
    if (m_status != B_OK)
        return m_status;
    if (!extensionIdentifier || !*extensionIdentifier || !window.IsValid() || !window.IsTargetLocal()
        || !reply.IsValid() || !reply.IsTargetLocal() || !identifier || identifier == std::numeric_limits<uint64>::max()
        || !loadIdentifier || !pageIdentifier)
        return B_BAD_VALUE;
#if ENABLE(WK_WEB_EXTENSIONS)
    auto name = String::fromUTF8(extensionIdentifier).isolatedCopy();
    if (name.isEmpty())
        return B_BAD_VALUE;
    auto operation = [context = m_context, name = WTF::move(name), window, loadIdentifier, pageIdentifier, reply, identifier] {
        context->extensionPackages()->activateAction(name, window, loadIdentifier, pageIdentifier, reply, identifier);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
    return B_OK;
#else
    return B_NOT_SUPPORTED;
#endif
}

status_t BWebKitContext::SetExtensionPermissionListener(const BMessenger& listener)
{
    if (m_status != B_OK)
        return m_status;
    if (!be_app || be_app->Thread() != find_thread(nullptr))
        return B_BAD_THREAD_ID;
    auto prompts = m_context->permissionPrompts();
    return prompts ? prompts->setListener(listener) : B_NOT_SUPPORTED;
}

void BWebKitContext::RespondToExtensionPermissionRequest(const char* identifier, bool allowed)
{
    if (m_status != B_OK || !identifier)
        return;
    auto copiedIdentifier = String::fromUTF8(identifier).isolatedCopy();
    WTF::RunLoop::mainSingleton().dispatch([context = m_context, identifier = WTF::move(copiedIdentifier), allowed] {
        if (auto prompts = context->permissionPrompts())
            prompts->respond(identifier, allowed);
    });
}

void BWebKitContext::CancelExtensionPermissionRequests()
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([context = m_context] {
        if (auto prompts = context->permissionPrompts())
            prompts->cancelAll();
    });
}

status_t BWebKitContext::SetDownloadListener(const BMessenger& listener)
{
    if (m_status != B_OK)
        return m_status;
    if (!be_app || be_app->Thread() != find_thread(nullptr))
        return B_BAD_THREAD_ID;
    return m_context->downloads()->setListener(listener);
}

status_t BWebKitContext::SetDownloadDirectory(const char* directory)
{
    if (m_status != B_OK)
        return m_status;
    if (!be_app || be_app->Thread() != find_thread(nullptr))
        return B_BAD_THREAD_ID;
    return m_context->downloads()->setDirectory(directory);
}

void BWebKitContext::CancelDownload(uint64 identifier)
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([context = m_context, identifier] {
        context->downloads()->cancel(identifier);
    });
}

void BWebKitContext::CancelAllDownloads()
{
    if (m_status != B_OK)
        return;
    WTF::RunLoop::mainSingleton().dispatch([context = m_context] {
        context->downloads()->cancelAll();
    });
}

bool BWebKitContext::HasPendingDownloads() const
{
    return m_context && m_context->downloads()->hasPendingDownloads();
}

status_t BWebKitContext::SetBrowserCommandListener(const BMessenger& listener)
{
    if (m_status != B_OK)
        return m_status;
    if (!be_app || be_app->Thread() != find_thread(nullptr))
        return B_BAD_THREAD_ID;
    auto commands = m_context->browserCommands();
    return commands ? commands->setListener(listener) : B_NOT_SUPPORTED;
}

void BWebKitContext::RespondToBrowserCommand(uint64 identifier, status_t status,
    const std::vector<BWebKitView*>& createdViews, const char* error)
{
    if (m_status != B_OK || !identifier)
        return;
    std::vector<std::shared_ptr<WebKit::WebViewStateHaiku>> states;
    states.reserve(createdViews.size());
    for (auto* view : createdViews) {
        if (!view) {
            // A host that lost its new view must not report a successful tab.
            status = status == B_OK ? B_BAD_VALUE : status;
            continue;
        }
        states.push_back(view->m_state);
    }
    auto description = error ? String::fromUTF8(error).isolatedCopy() : String();
    // Always queue, even on the application thread: a preceding
    // SetBrowserWindowTabs submission must reach the registry first.
    WTF::RunLoop::mainSingleton().dispatch([context = m_context, states = std::move(states), identifier, status, description = WTF::move(description)]() mutable {
        auto commands = context->browserCommands();
        if (!commands)
            return;
        WebKit::BrowserCommandBrokerHaiku::Result result { status, WTF::move(description), { } };
        for (auto& state : states) {
            if (state->context != context || state->closed || !state->client || !state->client->page() || state->client->page()->isClosed()) {
                if (result.status == B_OK)
                    result.status = state->context != context ? B_BAD_VALUE : B_CANCELED;
                continue;
            }
            result.createdPages.append(state->client->page()->identifier());
        }
        if (result.status != B_OK)
            result.createdPages.clear();
        commands->respond(identifier, WTF::move(result));
    });
}

void BWebKitContext::SetBrowserWindowTabs(const BMessenger& window, const std::vector<BWebKitView*>& views,
    BWebKitView* activeView, bool focused, const BMessenger& reply, uint64 identifier)
{
    auto respond = [reply, identifier](status_t error, uint64 windowIdentifier, int32 tabCount, int32 activeIndex) {
        if (!reply.IsValid())
            return;
        BMessage result(B_WEBKIT_BROWSER_WINDOW_UPDATED);
        result.AddUInt64("identifier", identifier);
        result.AddInt32("error", error);
        if (error == B_OK) {
            result.AddUInt64("window_identifier", windowIdentifier);
            result.AddInt32("tab_count", tabCount);
            result.AddInt32("active_index", activeIndex);
        }
        auto status = reply.SendMessage(&result, static_cast<BHandler*>(nullptr), 0);
        if (status == B_WOULD_BLOCK || status == B_TIMED_OUT)
            BMessageRunner::StartSending(reply, &result, 10000, 1);
    };
    if (m_status != B_OK) {
        respond(m_status, 0, 0, -1);
        return;
    }
    if (views.size() > static_cast<size_t>(std::numeric_limits<int32>::max())) {
        respond(B_BAD_VALUE, 0, 0, -1);
        return;
    }
    std::vector<std::shared_ptr<WebKit::WebViewStateHaiku>> states;
    states.reserve(views.size());
    int32 activeIndex = -1;
    for (size_t index = 0; index < views.size(); ++index) {
        auto* view = views[index];
        if (!view) {
            respond(B_BAD_VALUE, 0, 0, -1);
            return;
        }
        states.push_back(view->m_state);
        if (view == activeView)
            activeIndex = static_cast<int32>(index);
    }
    if (activeView && activeIndex == -1) {
        respond(B_BAD_VALUE, 0, 0, -1);
        return;
    }
    WTF::RunLoop::mainSingleton().dispatch([context = m_context, states = std::move(states), window, focused, activeIndex, respond] {
        Vector<WebKit::WebPageProxyIdentifier> pages;
        for (auto& state : states) {
            if (state->context != context) {
                respond(B_BAD_VALUE, 0, 0, -1);
                return;
            }
            if (state->closed || !state->client || !state->client->page() || state->client->page()->isClosed()) {
                respond(B_CANCELED, 0, 0, -1);
                return;
            }
            pages.append(state->client->page()->identifier());
        }
        auto activePage = activeIndex >= 0 ? std::optional { pages[activeIndex] } : std::nullopt;
        auto result = context->browserTabs()->update(window, pages, activePage, focused);
        respond(result ? B_OK : result.error(), result ? *result : 0, static_cast<int32>(pages.size()), activeIndex);
    });
}

void BWebKitContext::AllowServerCertificate(const char* host, const char* sha256)
{
    if (m_status != B_OK || !host || !*host || !sha256 || !*sha256)
        return;
    auto operation = [context = m_context, host = String::fromUTF8(host).isolatedCopy(),
        sha256 = String::fromUTF8(sha256).isolatedCopy()] {
        context->allowServerCertificate(host, sha256);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
}

void BWebKitContext::ForgetServerCertificates(const char* host)
{
    if (m_status != B_OK)
        return;
    auto operation = [context = m_context, host = String::fromUTF8(host ? host : "").isolatedCopy()] {
        context->forgetServerCertificates(host);
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
}

// Runs operation on the WebKit main thread with the context's site permissions.
template<typename Operation>
static void withSitePermissionsHaiku(const std::shared_ptr<WebKit::WebViewContextHaiku>& context, Operation&& operation)
{
    auto run = [context, operation = std::forward<Operation>(operation)]() mutable {
        if (auto permissions = context->sitePermissions())
            operation(*permissions);
    };
    if (RunLoop::isMain())
        run();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(run));
}

status_t BWebKitContext::SetPermissionListener(const BMessenger& listener)
{
    if (m_status != B_OK)
        return m_status;
    withSitePermissionsHaiku(m_context, [listener](WebKit::SitePermissionsHaiku& permissions) {
        permissions.setListener(listener);
    });
    return B_OK;
}

void BWebKitContext::RespondToPermissionRequest(uint64 identifier, bool allowed)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [identifier, allowed](WebKit::SitePermissionsHaiku& permissions) {
        permissions.respond(identifier, allowed);
    });
}

void BWebKitContext::RespondToMediaPermissionRequest(uint64 identifier, bool allowed, const char* deviceId)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [identifier, allowed, deviceId = String::fromUTF8(deviceId ? deviceId : "").isolatedCopy()](WebKit::SitePermissionsHaiku& permissions) {
        permissions.respond(identifier, allowed, deviceId);
    });
}

void BWebKitContext::SetSitePermission(const char* permission, const char* origin, int32 state)
{
    if (m_status != B_OK || !permission || !origin || !*origin)
        return;
    auto kind = WebKit::SitePermissionsHaiku::kindForName(String::fromUTF8(permission));
    if (!kind)
        return;
    std::optional<bool> allowed;
    if (state >= 0)
        allowed = state > 0;
    withSitePermissionsHaiku(m_context, [kind = *kind, origin = String::fromUTF8(origin).isolatedCopy(), allowed](WebKit::SitePermissionsHaiku& permissions) {
        permissions.setDecision(kind, origin, allowed);
    });
}

void BWebKitContext::NotificationClicked(uint64 identifier)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [identifier](WebKit::SitePermissionsHaiku& permissions) {
        permissions.notificationClicked(identifier);
    });
}

void BWebKitContext::NotificationClosed(uint64 identifier)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [identifier](WebKit::SitePermissionsHaiku& permissions) {
        permissions.notificationClosed(identifier);
    });
}

void BWebKitContext::SetGeolocationPosition(double latitude, double longitude, double accuracy, double timestamp)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [=](WebKit::SitePermissionsHaiku& permissions) {
        permissions.setPosition(latitude, longitude, accuracy, timestamp);
    });
}

void BWebKitContext::GeolocationUnavailable(const char* message)
{
    if (m_status != B_OK)
        return;
    withSitePermissionsHaiku(m_context, [message = String::fromUTF8(message ? message : "").isolatedCopy()](WebKit::SitePermissionsHaiku& permissions) {
        permissions.positionUnavailable(message);
    });
}

void BWebKitContext::RemoveWebsiteData(uint32 types, const BMessenger& reply, uint64 identifier, bool includeExtensions)
{
    auto respond = [reply, identifier](const char* error) {
        BMessage message(B_WEBKIT_WEBSITE_DATA_REMOVED);
        message.AddUInt64("identifier", identifier);
        if (error)
            message.AddString("error", error);
        reply.SendMessage(&message, static_cast<BHandler*>(nullptr), 0);
    };
    if (m_status != B_OK) {
        respond("The website data could not be opened");
        return;
    }
    auto operation = [context = m_context, types, includeExtensions, respond] {
        using WebKit::WebsiteDataType;
        // Cookies and caches are removed whole; storage is removed per origin
        // so that extensions' own origins can be kept.
        OptionSet<WebsiteDataType> whole;
        OptionSet<WebsiteDataType> byOrigin;
        if (types & B_WEBKIT_DATA_COOKIES)
            whole.add(WebsiteDataType::Cookies);
        if (types & B_WEBKIT_DATA_CACHE) {
            whole.add({ WebsiteDataType::DiskCache, WebsiteDataType::MemoryCache });
            // The scripts' compiled code (CachedScriptSourceProviderHaiku.cpp).
            const char* bytecode = getenv("WEBKIT_HAIKU_BYTECODE_CACHE");
            if (context->dataStore().isPersistent() && bytecode && *bytecode == '/') {
                Thread::create("Bytecode cache removal"_s, [path = String::fromUTF8(bytecode)] {
                    FileSystem::deleteNonEmptyDirectory(path);
                })->detach();
            }
        }
        if (types & B_WEBKIT_DATA_LOCAL_STORAGE)
            byOrigin.add({ WebsiteDataType::LocalStorage, WebsiteDataType::SessionStorage });
        if (types & B_WEBKIT_DATA_INDEXED_DB)
            byOrigin.add(WebsiteDataType::IndexedDBDatabases);
        if (types & B_WEBKIT_DATA_SERVICE_WORKERS)
            byOrigin.add({ WebsiteDataType::ServiceWorkerRegistrations, WebsiteDataType::DOMCache });
        if (includeExtensions) {
            whole.add(byOrigin);
            byOrigin = { };
        }
        Ref store = context->dataStore();
        auto pending = std::make_shared<unsigned>(2);
        auto finished = [pending, respond] {
            if (!--*pending)
                respond(nullptr);
        };
        if (whole)
            store->removeData(whole, WallTime::fromRawSeconds(0), [finished] { finished(); });
        else
            finished();
        if (!byOrigin) {
            finished();
            return;
        }
        store->fetchData(byOrigin, { }, [store, byOrigin, finished](Vector<WebKit::WebsiteDataRecord> records) mutable {
            Vector<WebKit::WebsiteDataRecord> removable;
            for (auto& record : records) {
                record.origins.removeIf([](const WebCore::SecurityOriginData& origin) {
                    return origin.protocol() == "webkit-extension"_s;
                });
                if (!record.origins.isEmpty())
                    removable.append(WTF::move(record));
            }
            if (removable.isEmpty()) {
                finished();
                return;
            }
            store->removeData(byOrigin, removable, WTF::move(finished));
        });
    };
    if (RunLoop::isMain())
        operation();
    else
        RunLoop::mainSingleton().dispatch(WTF::move(operation));
}
