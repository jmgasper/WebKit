/*
 * Copyright (C) 2022-2025 Apple Inc. All rights reserved.
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

#pragma once

#if ENABLE(WK_WEB_EXTENSIONS)

#include "APIHTTPCookieStore.h"
#include "APIObject.h"
#include "APIUserScript.h"
#include "APIUserStyleSheet.h"
#include "MessageReceiver.h"
#include "ProcessActivityGroup.h"
#include "RunJavaScriptResult.h"
#include "WebExtension.h"
#include "WebExtensionAction.h"
#include "WebExtensionAlarm.h"
#include "WebExtensionCommand.h"
#include "WebExtensionContextIdentifier.h"
#include "WebExtensionController.h"
#if PLATFORM(HAIKU)
#include "WebExtensionPrivacySettingsHaiku.h"
#endif
#include "WebExtensionDataType.h"
#include "WebExtensionDeclarativeNetRequestSQLiteStore.h"
#include "WebExtensionDynamicScripts.h"
#include "WebExtensionEventListenerType.h"
#include "WebExtensionFrameIdentifier.h"
#include "WebExtensionFrameParameters.h"
#include "WebExtensionLocalization.h"
#include "WebExtensionMatchPattern.h"
#include "WebExtensionMatchedRuleParameters.h"
#include "WebExtensionMenuItem.h"
#include "WebExtensionMessagePort.h"
#if PLATFORM(HAIKU)
#include "haiku/WebExtensionInstallStateHaiku.h"
#endif
#include "WebExtensionMessageSenderParameters.h"
#include "WebExtensionPagePortMap.h"
#include "WebExtensionPortChannelIdentifier.h"
#include "WebExtensionRegisteredScriptsSQLiteStore.h"
#include "WebExtensionStorageAccessLevel.h"
#include "WebExtensionStorageSQLiteStore.h"
#include "WebExtensionTab.h"
#include "WebExtensionTabIdentifier.h"
#if PLATFORM(HAIKU)
#include "WebExtensionTabScriptExecution.h"
#endif
#include "WebExtensionUtilities.h"
#include "WebExtensionWindow.h"
#include "WebExtensionWindowIdentifier.h"
#include "WebExtensionWindowParameters.h"
#include "WebFrameProxy.h"
#include "WebPageProxyIdentifier.h"
#include "WebProcessProxy.h"
#include <WebCore/ContentRuleListResults.h>
#include <wtf/CompletionHandler.h>
#include <wtf/Deque.h>
#include <wtf/Forward.h>
#include <wtf/HashCountedSet.h>
#include <wtf/HashMap.h>
#include <wtf/HashSet.h>
#include <wtf/Identified.h>
#include <wtf/ListHashSet.h>
#include <wtf/RefPtr.h>
#include <wtf/RetainPtr.h>
#include <wtf/RunLoop.h>
#include <wtf/URLHash.h>
#include <wtf/UUID.h>
#include <wtf/Variant.h>
#include <wtf/Vector.h>
#include <wtf/WeakHashCountedSet.h>
#include <wtf/WeakHashMap.h>
#include <wtf/WeakPtr.h>

#if PLATFORM(HAIKU)
#include <wtf/JSONValues.h>
#include <wtf/SharedTask.h>
class BMessage;
#endif

#if ENABLE(INSPECTOR_EXTENSIONS)
#include "APIInspectorExtension.h"
#include "APIInspectorExtensionClient.h"
#include "InspectorExtensionTypes.h"
#include "WebInspectorUIProxy.h"
#endif

#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
#include "WebExtensionActionClickBehavior.h"
#include "WebExtensionSidebar.h"
#include "WebExtensionSidebarParameters.h"
#endif

#if ENABLE(WK_WEB_EXTENSIONS_BOOKMARKS)
#include "WebExtensionBookmarksParameters.h"
#endif

#if ENABLE(2022_GLIB_API)
#include "WebKitNavigationPolicyDecision.h"
#include "WebKitWebExtensionContext.h"
#include "WebKitWebView.h"
#include <wtf/glib/GWeakPtr.h>
#endif

OBJC_CLASS NSArray;
OBJC_CLASS NSDate;
OBJC_CLASS NSDictionary;
OBJC_CLASS NSMapTable;
OBJC_CLASS NSMutableArray;
OBJC_CLASS NSMutableDictionary;
OBJC_CLASS NSNumber;
OBJC_CLASS NSString;
OBJC_CLASS NSURL;
OBJC_CLASS NSUUID;
OBJC_CLASS WKContentRuleListStore;
OBJC_CLASS WKNavigation;
OBJC_CLASS WKNavigationAction;
OBJC_CLASS WKWebExtensionContext;
OBJC_CLASS _WKWebExtensionContextDelegate;
OBJC_CLASS WKWebView;
OBJC_CLASS WKWebViewConfiguration;
OBJC_PROTOCOL(WKWebExtensionTab);
OBJC_PROTOCOL(WKWebExtensionWindow);

#if PLATFORM(MAC)
OBJC_CLASS NSEvent;
OBJC_CLASS NSMenu;
OBJC_CLASS NSWindow;
OBJC_CLASS WKOpenPanelParameters;
#endif

#if PLATFORM(IOS_FAMILY)
OBJC_CLASS UIWindow;
#endif

namespace PAL {
class SessionID;
}

namespace API {
class ContentRuleList;
class NavigationAction;
class PageConfiguration;
}

namespace WebKit {

struct WebExtensionPermissionState;

#if PLATFORM(HAIKU)
using WebViewClass = WebPageProxy;
using WebViewConfiguration = RefPtr<API::PageConfiguration>;
using NavigationPolicyAction = API::NavigationAction;
#elif ENABLE(2022_GLIB_API)
using WebViewClass = WebKitWebView;
using WebViewConfiguration = GRefPtr<WebKitSettings>;
using NavigationPolicyAction = WebKitNavigationPolicyDecision;
#elif PLATFORM(COCOA)
using WebViewClass = WKWebView;
using WebViewConfiguration = WKWebViewConfiguration *;
using NavigationPolicyAction = WKNavigationAction;
#endif

class ContextMenuContextData;
class ProcessThrottlerActivity;
class WebExtension;
class WebUserContentControllerProxy;
struct WebExtensionContentRuleListBlockedLoadInfo;
struct WebExtensionContextParameters;
struct WebExtensionCookieFilterParameters;
struct WebExtensionCookieParameters;
struct WebExtensionCookieStoreParameters;
struct WebExtensionMenuItemContextParameters;
struct WebExtensionMessageTargetParameters;

#if ENABLE(DNR_ON_RULE_MATCHED_DEBUG)
struct ContentRuleListMatchedRule;
#endif

enum class WebExtensionContextInstallReason : uint8_t {
    None,
    ExtensionInstall,
    ExtensionUpdate,
    BrowserUpdate,
};

#if PLATFORM(HAIKU)
class WebView;
class ExtensionActionPopupHaiku;
#endif

class WebExtensionContext : public API::ObjectImpl<API::Object::Type::WebExtensionContext>, public IPC::MessageReceiver, public Identified<WebExtensionContextIdentifier> {
    WTF_MAKE_NONCOPYABLE(WebExtensionContext);

public:
    template<typename... Args>
    static Ref<WebExtensionContext> create(Args&&... args)
    {
        return adoptRef(*new WebExtensionContext(std::forward<Args>(args)...));
    }

    virtual ~WebExtensionContext();

    void ref() const final { API::ObjectImpl<API::Object::Type::WebExtensionContext>::ref(); }
    void deref() const final { API::ObjectImpl<API::Object::Type::WebExtensionContext>::deref(); }

    static String stateFileName()
    {
#if PLATFORM(COCOA)
        return "State.plist"_s;
#elif PLATFORM(HAIKU)
        return "State.json"_s;
#else
        return "State.ini"_s;
#endif
    };
#if PLATFORM(COCOA)
    static NSMutableDictionary *readStateFromPath(const String&);
#elif ENABLE(2022_GLIB_API)
    static GRefPtr<GKeyFile> readStateFromPath(const String&);
#elif PLATFORM(HAIKU)
    static RefPtr<JSON::Object> readStateFromPath(const String&);
#endif
    static bool readLastBaseURLFromState(const String& filePath, URL& outLastBaseURL);
    static bool readDisplayNameFromState(const String& filePath, String& outDisplayName);

    static bool isURLForAnyExtension(const URL&);

    static WebExtensionContext* NODELETE get(WebExtensionContextIdentifier);

#if PLATFORM(COCOA) || PLATFORM(HAIKU)
    explicit WebExtensionContext(Ref<WebExtension>&&);
#elif ENABLE(2022_GLIB_API)
    explicit WebExtensionContext(WebKitWebExtensionContext*);
#endif

    using PermissionsMap = HashMap<String, WallTime>;
    using PermissionMatchPatternsMap = HashMap<Ref<WebExtensionMatchPattern>, WallTime>;

    using UserScriptVector = Vector<Ref<API::UserScript>>;
    using UserStyleSheetVector = Vector<Ref<API::UserStyleSheet>>;

    using AlarmInfoMap = HashMap<String, double>;

    using DynamicInjectedContentsMap = HashMap<String, WebExtension::InjectedContentData>;

    using PermissionsSet = WebExtension::PermissionsSet;
    using MatchPatternSet = WebExtension::MatchPatternSet;
    using InjectedContentData = WebExtension::InjectedContentData;
    using InjectedContentVector = WebExtension::InjectedContentVector;
    using URLSet = HashSet<URL>;
    using URLVector = Vector<URL>;

    using WeakFrameCountedSet = WeakHashCountedSet<WebFrameProxy>;
    using EventListenerTypeCountedSet = HashCountedSet<WebExtensionEventListenerType>;
    using EventListenerTypeFrameMap = HashMap<WebExtensionEventListenerTypeWorldPair, WeakFrameCountedSet>;
    using EventListenerTypeSet = HashSet<WebExtensionEventListenerType>;
    using ContentWorldTypeSet = HashSet<WebExtensionContentWorldType>;
    using VoidFunctionVector = Vector<Function<void()>>;

    using WindowIdentifierMap = HashMap<WebExtensionWindowIdentifier, Ref<WebExtensionWindow>>;
    using WindowIdentifierVector = Vector<WebExtensionWindowIdentifier>;
    using TabIdentifierMap = HashMap<WebExtensionTabIdentifier, Ref<WebExtensionTab>>;
    using PageTabIdentifierMap = WeakHashMap<WebPageProxy, WebExtensionTabIdentifier>;
    using PopupPageActionMap = WeakHashMap<WebPageProxy, Ref<WebExtensionAction>>;

    using WindowVector = Vector<Ref<WebExtensionWindow>>;
    using TabVector = Vector<Ref<WebExtensionTab>>;
    using TabSet = HashSet<Ref<WebExtensionTab>>;

    using PopulateTabs = WebExtensionWindow::PopulateTabs;
    using WindowTypeFilter = WebExtensionWindow::TypeFilter;

    using WebProcessProxySet = HashSet<Ref<WebProcessProxy>>;

    struct PortQueuedMessage {
        String messageJSON;
        std::optional<WebPageProxyIdentifier> sendingPageProxyIdentifier;
        bool userGesture { false };
    };

    using PortWorldPair = std::pair<WebExtensionContentWorldType, WebExtensionPortChannelIdentifier>;
    using PortCountedSet = HashCountedSet<PortWorldPair>;
    using PortQueuedMessageMap = HashMap<PortWorldPair, Vector<PortQueuedMessage>>;
    using NativePortMap = HashMap<WebExtensionPortChannelIdentifier, Ref<WebExtensionMessagePort>>;

    using PageIdentifierTuple = std::tuple<WebCore::PageIdentifier, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>>;

    using CommandsVector = Vector<Ref<WebExtensionCommand>>;

    using MenuItemVector = Vector<Ref<WebExtensionMenuItem>>;
    using MenuItemMap = HashMap<String, Ref<WebExtensionMenuItem>>;

    using DeclarativeNetRequestValidatedRulesets = std::expected<WebExtension::DeclarativeNetRequestRulesetVector, WebExtensionError>;
    using DeclarativeNetRequestMatchedRuleVector = Vector<WebExtensionMatchedRuleParameters>;

    using UserContentControllerProxySet = WeakHashSet<WebUserContentControllerProxy>;

#if ENABLE(INSPECTOR_EXTENSIONS)
    using InspectorTabVector = Vector<std::pair<Ref<WebInspectorUIProxy>, RefPtr<WebExtensionTab>>>;
#endif

    using ReloadFromOrigin = WebExtensionTab::ReloadFromOrigin;

    enum class EqualityOnly : bool { No, Yes };
    enum class WindowIsClosing : bool { No, Yes };
    enum class UserTriggered : bool { No, Yes };
    enum class SuppressEvents : bool { No, Yes };
    enum class UpdateWindowOrder : bool { No, Yes };
    enum class IgnoreExtensionAccess : bool { No, Yes };
    enum class IncludeExtensionViews : bool { No, Yes };
    enum class GrantOnCompletion : bool { No, Yes };
    enum class IncludePrivilegedIdentifier : bool { No, Yes };

    enum class Error : uint8_t {
        Unknown = 1,
        AlreadyLoaded,
        NotLoaded,
        BaseURLAlreadyInUse,
        NoBackgroundContent,
        BackgroundContentFailedToLoad,
        ScriptExecutionError,
    };

    // Keep in sync with WKWebExtensionContextError values
    enum class APIError : uint8_t {
        Unknown = 1,
        AlreadyLoaded,
        NotLoaded,
        BaseURLAlreadyInUse,
        NoBackgroundContent,
        BackgroundContentFailedToLoad,
        ScriptExecutionError,
    };

    enum class PermissionNotification : uint8_t {
        None = 0,
        PermissionsWereGranted,
        PermissionsWereDenied,
        GrantedPermissionsWereRemoved,
        DeniedPermissionsWereRemoved,
        PermissionMatchPatternsWereGranted,
        PermissionMatchPatternsWereDenied,
        GrantedPermissionMatchPatternsWereRemoved,
        DeniedPermissionMatchPatternsWereRemoved,
    };

#if PLATFORM(HAIKU)
    // Main-thread host observer. Replacing or clearing it cancels queued
    // deliveries; permission and pattern sets describe the recorded change.
    using NativePermissionChangeHandler = SharedTask<void(PermissionNotification, const PermissionsSet&, const MatchPatternSet&)>;
    void setNativePermissionChangeHandler(RefPtr<NativePermissionChangeHandler>&&);

    using NativeActionChangeHandler = SharedTask<void(WebExtensionAction&)>;
    void setNativeActionChangeHandler(RefPtr<NativeActionChangeHandler>&&);
    void nativeActionPropertiesDidChange(WebExtensionAction&);
    void closeNativeActionPopup();
    void closeNativeActionPopupForTab(WebExtensionTab&);
    void validateNativeActionPopup();
    friend class ExtensionActionPopupHaiku;
#endif

    enum class PermissionState : int8_t {
        DeniedExplicitly    = -3,
        DeniedImplicitly    = -2,
        RequestedImplicitly = -1,
        Unknown             = 0,
        RequestedExplicitly = 1,
        GrantedImplicitly   = 2,
        GrantedExplicitly   = 3,
    };

    enum class PermissionStateOptions : uint8_t {
        RequestedWithTabsPermission = 1 << 0, // Request access to a URL if the extension also has the "tabs" permission.
        SkipRequestedPermissions    = 1 << 1, // Don't check requested permissions.
        IncludeOptionalPermissions  = 1 << 2, // Check the optional permissions, and count them as RequestedImplicitly.
    };

    using InstallReason = WebExtensionContextInstallReason;

    enum class WebViewPurpose : uint8_t {
        Any,
        Background,
        Inspector,
        Popup,
        Sidebar,
        Tab,
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
        Offscreen,
#endif
    };

#if ENABLE(INSPECTOR_EXTENSIONS)
    struct InspectorContext {
        std::optional<WebExtensionTabIdentifier> tabIdentifier;
        RefPtr<API::InspectorExtension> extension;
        RetainPtr<WKWebView> backgroundWebView;
        Variant<std::monostate, Ref<ProcessThrottlerActivity>, Ref<ProcessActivityGroup>> activity;
    };
#endif

    WebExtensionContextIdentifier NODELETE privilegedIdentifier() const;

    WebExtensionContextParameters parameters(IncludePrivilegedIdentifier, WebProcessProxy& destinationProcess) const;

    bool operator==(const WebExtensionContext& other) const { return (this == &other); }

    Ref<API::Error> createError(Error, const String& customLocalizedDescription = nullString(), RefPtr<API::Error> underlyingError = nullptr);
    void recordErrorIfNeeded(RefPtr<API::Error> error)
    {
        if (error)
            recordError(*error);
    }

    void recordError(Ref<API::Error>);
    void clearError(Error);

    Vector<Ref<API::Error>> errors();

    bool storageIsPersistent() const { return !m_storageDirectory.isEmpty(); }
    const String& storageDirectory() const LIFETIME_BOUND { return m_storageDirectory; }

    void invalidateStorage();

    Ref<WebExtensionStorageSQLiteStore> storageForType(WebExtensionDataType);

    std::expected<bool, RefPtr<API::Error>> load(WebExtensionController&, String storageDirectory);
    std::expected<bool, RefPtr<API::Error>> unload();
    std::expected<bool, RefPtr<API::Error>> reload();

    bool isLoaded() const { return !!m_extensionController; }

    WebExtension& extension() const { return *m_extension; }
    WebExtensionController* extensionController() const { return m_extensionController.get(); }

    const URL& baseURL() const LIFETIME_BOUND { return m_baseURL; }
    void setBaseURL(URL&&);

    bool isURLForThisExtension(const URL&) const;

    bool hasCustomUniqueIdentifier() const { return m_customUniqueIdentifier; }

    const String& uniqueIdentifier() const LIFETIME_BOUND { return m_uniqueIdentifier; }
    void setUniqueIdentifier(String&&);
#if PLATFORM(HAIKU)
    bool setNativeInstallationMetadata(String identifier, uint64_t order)
    {
        ASSERT(isMainThread());
        if (isLoaded() || identifier.isEmpty() != !order || identifier.length() > 255 || identifier.contains(static_cast<UChar>(0)))
            return false;
        m_nativeInstallationIdentifier = WTF::move(identifier);
        m_nativeInstallationOrder = order;
        return true;
    }
    const String& nativeInstallationIdentifier() const { return m_nativeInstallationIdentifier; }
    uint64_t nativeInstallationOrder() const { return m_nativeInstallationOrder; }
    const WebExtensionPrivacySettingsHaiku& nativePrivacySettings() const { return m_nativePrivacySettings; }
    uint64_t nativePrivacyGeneration() const { return m_nativePrivacyGeneration; }
    std::expected<void, String> restoreNativePrivacySettings();

    void privacyGet(const String& setting, const String& detailsJSON, CompletionHandler<void(String, String)>&&);
    void privacyUpdate(const String& setting, const String& operation, const String& detailsJSON, CompletionHandler<void(bool, String)>&&);

    // The browser host sets the purpose before load; it is consumed once.
    bool setNativeLoadPurpose(WebExtensionLoadPurposeHaiku purpose)
    {
        ASSERT(isMainThread());
        if (isLoaded())
            return false;
        m_nativeLoadPurpose = purpose;
        return true;
    }
#endif

    RefPtr<WebExtensionLocalization> localization();

    RefPtr<API::Data> localizedResourceData(const RefPtr<API::Data>&, const String& mimeType);
    String localizedResourceString(const String&, const String& mimeType);

    bool isInspectable() const { return m_inspectable; }
    void setInspectable(bool);

    HashSet<String> unsupportedAPIs() const { return m_unsupportedAPIs; }
    void setUnsupportedAPIs(HashSet<String>&&);

    InjectedContentVector injectedContents() const;
    bool hasInjectedContentForURL(const URL&);
    bool hasInjectedContent();
    bool safeToInjectContent() const { return isLoaded() && m_safeToInjectContent; }

    bool NODELETE hasContentModificationRules();

    URL optionsPageURL() const;
    URL overrideNewTabPageURL() const;

    const PermissionsMap& grantedPermissions() LIFETIME_BOUND;
    void setGrantedPermissions(PermissionsMap&&);

    const PermissionsMap& deniedPermissions() LIFETIME_BOUND;
    void setDeniedPermissions(PermissionsMap&&);

    const PermissionMatchPatternsMap& grantedPermissionMatchPatterns() LIFETIME_BOUND;
    void setGrantedPermissionMatchPatterns(PermissionMatchPatternsMap&&, EqualityOnly = EqualityOnly::Yes);

    const PermissionMatchPatternsMap& deniedPermissionMatchPatterns() LIFETIME_BOUND;
    void setDeniedPermissionMatchPatterns(PermissionMatchPatternsMap&&, EqualityOnly = EqualityOnly::Yes);

    bool requestedOptionalAccessToAllHosts() const { return m_requestedOptionalAccessToAllHosts; }
    void setRequestedOptionalAccessToAllHosts(bool requested) { m_requestedOptionalAccessToAllHosts = requested; }

    bool hasAccessToPrivateData() const { return m_hasAccessToPrivateData; }
    void setHasAccessToPrivateData(bool);

    bool hasAccessToFileURLs() const { return m_hasAccessToFileURLs; }
    void setHasAccessToFileURLs(bool);

    void grantPermissions(PermissionsSet&&, WallTime expirationDate = WallTime::infinity());
    void denyPermissions(PermissionsSet&&, WallTime expirationDate = WallTime::infinity());

    void grantPermissionMatchPatterns(MatchPatternSet&&, WallTime expirationDate = WallTime::infinity(), EqualityOnly = EqualityOnly::Yes);
    void denyPermissionMatchPatterns(MatchPatternSet&&, WallTime expirationDate = WallTime::infinity(), EqualityOnly = EqualityOnly::Yes);

    bool removeGrantedPermissions(PermissionsSet&);
    bool removeGrantedPermissionMatchPatterns(MatchPatternSet&, EqualityOnly = EqualityOnly::Yes);

    bool removeDeniedPermissions(PermissionsSet&);
    bool removeDeniedPermissionMatchPatterns(MatchPatternSet&, EqualityOnly = EqualityOnly::Yes);

    void requestPermissionMatchPatterns(const MatchPatternSet&, RefPtr<WebExtensionTab> = nullptr, CompletionHandler<void(MatchPatternSet&& neededMatchPatterns, MatchPatternSet&& allowedMatchPatterns, WallTime expirationDate)>&& = nullptr, GrantOnCompletion = GrantOnCompletion::Yes, OptionSet<PermissionStateOptions> = { });
    void requestPermissionToAccessURLs(const URLVector&, RefPtr<WebExtensionTab> = nullptr, CompletionHandler<void(URLSet&& neededURLs, URLSet&& allowedURLs, WallTime expirationDate)>&& = nullptr, GrantOnCompletion = GrantOnCompletion::Yes, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });
    void requestPermissions(const PermissionsSet&, RefPtr<WebExtensionTab> = nullptr, CompletionHandler<void(PermissionsSet&& neededPermissions, PermissionsSet&& allowedPermissions, WallTime expirationDate)>&& = nullptr, GrantOnCompletion = GrantOnCompletion::Yes, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });

    PermissionsMap::KeysConstIteratorRange currentPermissions() { return grantedPermissions().keys(); }
    PermissionMatchPatternsMap::KeysConstIteratorRange currentPermissionMatchPatterns() { return grantedPermissionMatchPatterns().keys(); }

    bool hasAccessToAllURLs();
    bool hasAccessToAllHosts();

    bool needsPermission(const String&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { });
    bool needsPermission(const URL&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });
    bool needsPermission(const WebExtensionMatchPattern&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });

    bool hasPermission(const String& permission, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { });
    bool hasPermission(const URL&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });
    bool hasPermission(const WebExtensionMatchPattern&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });

    bool hasPermissions(PermissionsSet, MatchPatternSet);

    PermissionState permissionState(const String& permission, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { });
    void setPermissionState(PermissionState, const String& permission, WallTime expirationDate = WallTime::infinity());

    PermissionState permissionState(const URL&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });
    void setPermissionState(PermissionState, const URL&, WallTime expirationDate = WallTime::infinity());

    PermissionState permissionState(const WebExtensionMatchPattern&, WebExtensionTab* = nullptr, OptionSet<PermissionStateOptions> = { PermissionStateOptions::RequestedWithTabsPermission });
    void setPermissionState(PermissionState, const WebExtensionMatchPattern&, WallTime expirationDate = WallTime::infinity());

    void clearCachedPermissionStates();

    void removePage(WebPageProxy&);

#if PLATFORM(HAIKU)
    RefPtr<WebPageProxy> nativePage(WebPageProxyIdentifier) const;
    RefPtr<WebExtensionWindow> getOrCreateNativeWindow(uint64_t) const;
    RefPtr<WebExtensionTab> getOrCreateNativeTab(WebPageProxyIdentifier) const;
    void pruneNativeHostObjects() const;
    void nativeBrowserWindowsDidChange(const Vector<uint64_t>& before, const Vector<uint64_t>& after,
        std::optional<uint64_t> previousFocus, std::optional<uint64_t> newFocus);
    // One committed registry transition, flattened in window then tab order.
    struct NativeBrowserTabState {
        WebPageProxyIdentifier page;
        uint64_t window { 0 };
        size_t index { 0 };
        bool active { false };
    };
    void nativeBrowserTabsWillChange(const Vector<NativeBrowserTabState>& before, const Vector<NativeBrowserTabState>& after);
    void nativeBrowserTabsDidChange(const Vector<NativeBrowserTabState>& before, const Vector<NativeBrowserTabState>& after);
    bool canDeliverNativeTabEvent(WebExtensionContextIdentifier loadIdentifier) const;

    // Ask the host browser to change its own tab or window model. The message
    // must be a B_WEBKIT_BROWSER_COMMAND with its command and targets; this adds
    // the calling extension. Completion is always asynchronous and reports the
    // pages of any views the host created. No listener, no answer before the
    // deadline and unloading this extension all fail with an extension error.
    using NativeBrowserCommandResult = std::expected<Vector<WebPageProxyIdentifier>, WebExtensionError>;
    void sendNativeBrowserCommand(BMessage&&, const String& apiName, CompletionHandler<void(NativeBrowserCommandResult&&)>&&);
    // False when the tab or window has no live native counterpart to name.
    bool addNativeBrowserCommandTab(BMessage&, const WebExtensionTab&, const char* viewField = "view", const char* pageField = "page_identifier") const;
    bool addNativeBrowserCommandWindow(BMessage&, const WebExtensionWindow&) const;
#endif
    Ref<WebExtensionWindow> getOrCreateWindow(WKWebExtensionWindow *) const;
    RefPtr<WebExtensionWindow> getWindow(WebExtensionWindowIdentifier, std::optional<WebPageProxyIdentifier> = std::nullopt, IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    void forgetWindow(WebExtensionWindowIdentifier) const;

    Ref<WebExtensionTab> getOrCreateTab(WKWebExtensionTab *) const;
    RefPtr<WebExtensionTab> getTab(WebExtensionTabIdentifier, IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    RefPtr<WebExtensionTab> getTab(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier> = std::nullopt, IncludeExtensionViews = IncludeExtensionViews::No, IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    RefPtr<WebExtensionTab> getCurrentTab(WebPageProxyIdentifier, IncludeExtensionViews = IncludeExtensionViews::Yes, IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    std::expected<Ref<WebExtensionTab>, WebExtensionError> getTabFromIdentifiers(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>) const;
    void forgetTab(WebExtensionTabIdentifier) const;

    bool canOpenNewWindow() const;
    void openNewWindow(const WebExtensionWindowParameters&, CompletionHandler<void(RefPtr<WebExtensionWindow>)>&&);
    void openNewTab(const WebExtensionTabParameters&, CompletionHandler<void(RefPtr<WebExtensionTab>)>&&);

    WindowVector openWindows(IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    TabVector openTabs(IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;

    RefPtr<WebExtensionWindow> focusedWindow(IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;
    RefPtr<WebExtensionWindow> frontmostWindow(IgnoreExtensionAccess = IgnoreExtensionAccess::No) const;

    bool isValidWindow(const WebExtensionWindow&);
    bool isValidTab(const WebExtensionTab&);

    void didOpenWindow(WebExtensionWindow&, UpdateWindowOrder = UpdateWindowOrder::Yes, SuppressEvents = SuppressEvents::No);
    void didCloseWindow(WebExtensionWindow&);
    void didFocusWindow(const WebExtensionWindow*, SuppressEvents = SuppressEvents::No);

    void didOpenTab(WebExtensionTab&, SuppressEvents = SuppressEvents::No);
    void didCloseTab(WebExtensionTab&, WindowIsClosing = WindowIsClosing::No, SuppressEvents = SuppressEvents::No);
    void didActivateTab(const WebExtensionTab&, const WebExtensionTab* previousTab = nullptr);
    void didSelectOrDeselectTabs(const TabSet&);

    void didMoveTab(WebExtensionTab&, size_t oldIndex, const WebExtensionWindow* oldWindow = nullptr);
    void didReplaceTab(WebExtensionTab& oldTab, WebExtensionTab& newTab, SuppressEvents = SuppressEvents::No);
    void didChangeTabProperties(WebExtensionTab&, OptionSet<WebExtensionTab::ChangedProperties> = { });

    void didStartProvisionalLoadForFrame(WebPageProxyIdentifier, const WebExtensionFrameParameters&, WallTime);
    void didCommitLoadForFrame(WebPageProxyIdentifier, const WebExtensionFrameParameters&, WallTime);
    void didFinishLoadForFrame(WebPageProxyIdentifier, const WebExtensionFrameParameters&, WallTime);
    void didFailLoadForFrame(WebPageProxyIdentifier, const WebExtensionFrameParameters&, WallTime);

    void resourceLoadDidSendRequest(WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::ResourceRequest&);
    void resourceLoadDidPerformHTTPRedirection(WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::ResourceResponse&, const WebCore::ResourceRequest&);
    void resourceLoadDidReceiveChallenge(WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::AuthenticationChallenge&);
    void resourceLoadDidReceiveResponse(WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::ResourceResponse&);
    void resourceLoadDidCompleteWithError(WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::ResourceResponse&, const WebCore::ResourceError&);

#if ENABLE(CONTENT_EXTENSIONS)
    void resourceLoadWasBlockedByDeclarativeNetRequest(WebPageProxyIdentifier, const WebExtensionContentRuleListBlockedLoadInfo&);
#endif

#if ENABLE(INSPECTOR_EXTENSIONS)
    void inspectorWillOpen(WebInspectorUIProxy&, WebPageProxy&);
    void inspectorWillClose(WebInspectorUIProxy&, WebPageProxy&);

    void didShowInspectorExtensionPanel(API::InspectorExtension&, const Inspector::ExtensionTabID&, WebCore::FrameIdentifier) const;
    void didHideInspectorExtensionPanel(API::InspectorExtension&, const Inspector::ExtensionTabID&) const;
    void inspectedPageDidNavigate(API::InspectorExtension&, const URL&);
    void inspectorEffectiveAppearanceDidChange(API::InspectorExtension&, Inspector::ExtensionAppearance);
#endif

    WebExtensionAction& defaultAction();
    Ref<WebExtensionAction> getAction(WebExtensionWindow*);
    Ref<WebExtensionAction> getAction(WebExtensionTab*);
    Ref<WebExtensionAction> getOrCreateAction(WebExtensionWindow*);
    Ref<WebExtensionAction> getOrCreateAction(WebExtensionTab*);
    void performAction(WebExtensionTab*, UserTriggered = UserTriggered::No);

#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
    WebExtensionSidebar& defaultSidebar();
    std::optional<Ref<WebExtensionSidebar>> getSidebar(WebExtensionWindow const&);
    std::optional<Ref<WebExtensionSidebar>> getSidebar(WebExtensionTab const&);
    std::optional<Ref<WebExtensionSidebar>> getOrCreateSidebar(WebExtensionWindow&);
    std::optional<Ref<WebExtensionSidebar>> getOrCreateSidebar(WebExtensionTab&);

    // The sidebar object which should be given to the browser for the specified tab. If the extension
    // has specified tab-specific overrides for this tab, then the tab's sidebar; otherwise, the window's.
    std::optional<Ref<WebExtensionSidebar>> sidebarForTab(WebExtensionTab&);
    bool discardSidebarIfUnmodified(WebExtensionSidebar&);
    void addSidebarPage(WebPageProxy&, WebExtensionSidebar&);
    void openSidebar(WebExtensionSidebar&);
    void closeSidebar(WebExtensionSidebar&);
    void notifyDelegateOfSidebarUpdate(WebExtensionSidebar&);
    void notifyDelegateOfSidebarInvalidation(WebExtensionSidebar&);
    bool canProgrammaticallyOpenSidebar();
    bool canProgrammaticallyCloseSidebar();
#endif // ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)

    const CommandsVector& commands();
    WebExtensionCommand* command(const String& identifier);
    void performCommand(WebExtensionCommand&, UserTriggered = UserTriggered::No);
#if PLATFORM(HAIKU)
    void performCommand(WebExtensionCommand&, WebExtensionTab*, UserTriggered);
#endif
    void resetCommands();

#if TARGET_OS_IPHONE
    WebExtensionCommand* commandMatchingKeyCommand(UIKeyCommand *);
    bool performCommand(UIKeyCommand *);
#endif
#if USE(APPKIT)
    WebExtensionCommand* command(NSEvent *);
    bool performCommand(NSEvent *);
#endif

    NSArray *platformMenuItems(const WebExtensionTab&) const;

    const MenuItemVector& mainMenuItems() const LIFETIME_BOUND { return m_mainMenuItems; }
    WebExtensionMenuItem* menuItem(const String& identifier) const;
    void performMenuItem(WebExtensionMenuItem&, const WebExtensionMenuItemContextParameters&, UserTriggered = UserTriggered::No);

    CocoaMenuItem *singleMenuItemOrExtensionItemWithSubmenu(const WebExtensionMenuItemContextParameters&) const;

#if PLATFORM(MAC)
    void addItemsToContextMenu(WebPageProxy&, const ContextMenuContextData&, NSMenu *);
#endif

    void userGesturePerformed(WebExtensionTab&);
    bool NODELETE hasActiveUserGesture(WebExtensionTab&) const;
    void clearUserGesture(WebExtensionTab&);

    bool NODELETE inTestingMode() const;

#if PLATFORM(COCOA)
    void sendTestMessage(const String& message, id argument);
    void sendTestStarted(id argument);
    void sendTestFinished(id argument);
    void reloadBackgroundContentForTesting();
#elif PLATFORM(HAIKU)
    void sendTestMessage(const String& message, Ref<JSON::Value> argument);
    void sendTestStarted(Ref<JSON::Value> argument);
    void sendTestFinished(Ref<JSON::Value> argument);
#endif

    URL backgroundContentURL();
    WebViewClass *backgroundWebView() const { return m_backgroundWebView.get(); }

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && PLATFORM(COCOA)
    WKWebView *offscreenWebView() const { return m_offscreenWebView.get(); }
    bool isOffscreenWebView(WKWebView *webView) const { return webView == m_offscreenWebView; }
#elif ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN) && PLATFORM(HAIKU)
    WebPageProxy* offscreenWebView() const { return m_offscreenWebView.get(); }
    bool isOffscreenWebView(const WebPageProxy& page) const { return &page == m_offscreenWebView.get(); }
    void offscreenDocumentDidFinishLoad(WebPageProxy&);
    void offscreenDocumentDidFail(WebPageProxy&, const String& reason);
#endif
    bool safeToLoadBackgroundContent() const { return m_safeToLoadBackgroundContent; }

    RefPtr<API::Error> backgroundContentLoadError() const { return m_backgroundContentLoadError; }

    const String& backgroundWebViewInspectionName();
    void setBackgroundWebViewInspectionName(const String&);

    bool decidePolicyForNavigationAction(WebViewClass *, NavigationPolicyAction *);
    void didFinishDocumentLoad(WebViewClass *);
    void didFailNavigation(WebViewClass *, RefPtr<API::Error>);
    void webViewWebContentProcessDidTerminate(WebViewClass *);

#if PLATFORM(MAC)
    void runOpenPanel(WKWebView *, WKOpenPanelParameters *, void (^)(NSArray *));
#endif

    void dispatchNativePortMessage(WebExtensionPortChannelIdentifier, const String& messageJSON);
#if PLATFORM(COCOA)
    void sendNativeMessage(const String& applicationID, id message, CompletionHandler<void(std::expected<RetainPtr<id>, WebExtensionError>&&)>&&);
#endif

    void addInjectedContent(WebUserContentControllerProxy&);
    void removeInjectedContent(WebUserContentControllerProxy&);

    void addDeclarativeNetRequestRules(WebUserContentControllerProxy&);

    bool handleContentRuleListNotificationForTab(WebExtensionTab&, const URL&, WebCore::ContentRuleListResults::Result);
    void incrementActionCountForTab(WebExtensionTab&, ssize_t incrementAmount);

#if ENABLE(DNR_ON_RULE_MATCHED_DEBUG)
    void handleContentRuleListMatchedRule(WebExtensionTab&, WebCore::ContentRuleListMatchedRule&);
#endif

    // Returns whether or not there are any matched rules after the purge.
    bool purgeMatchedRulesFromBefore(const WallTime&);

    UserStyleSheetVector& dynamicallyInjectedUserStyleSheets() LIFETIME_BOUND { return m_dynamicallyInjectedUserStyleSheets; };

    // Maps page identifiers appropriately, even with site isolation enabled.
    std::optional<WebCore::PageIdentifier> backgroundPageIdentifier(WebProcessProxy& destinationProcess) const;
    std::optional<WebCore::PageIdentifier> backgroundPageIdentifierInOwnProcess() const;

#if ENABLE(INSPECTOR_EXTENSIONS)
    Vector<PageIdentifierTuple> inspectorBackgroundPageIdentifiers() const;
    Vector<PageIdentifierTuple> inspectorPageIdentifiers() const;
#endif
    Vector<PageIdentifierTuple> popupPageIdentifiers(WebProcessProxy& destinationProcess) const;
    Vector<PageIdentifierTuple> tabPageIdentifiers(WebProcessProxy& destinationProcess) const;

    void addExtensionTabPage(WebPageProxy&, WebExtensionTab&);
    void addPopupPage(WebPageProxy&, WebExtensionAction&);

    void enumerateExtensionPages(NOESCAPE Function<void(WebPageProxy&, bool& stop)>&&);

    WebViewClass *relatedWebView();
    String processDisplayName();
    Vector<String> corsDisablingPatterns();
    void updateCORSDisablingPatternsOnAllExtensionPages();
    WebViewConfiguration webViewConfiguration(WebViewPurpose = WebViewPurpose::Any);

    WebsiteDataStore* websiteDataStore(std::optional<PAL::SessionID> = std::nullopt) const;

#if PLATFORM(HAIKU) && USE(CURL)
    void cookiesDidChange(API::HTTPCookieStore&, const Vector<WebCore::CookieChange>&);
#else
    void cookiesDidChange(API::HTTPCookieStore&);
#endif

    void loadBackgroundContent(CompletionHandler<void(RefPtr<API::Error>)>&&);

    void wakeUpBackgroundContentIfNecessary(Function<void()>&&);
    void wakeUpBackgroundContentIfNecessaryToFireEvents(EventListenerTypeSet&&, Function<void()>&&);

    HashSet<Ref<WebProcessProxy>> processes(WebExtensionEventListenerType type, WebExtensionContentWorldType contentWorldType) const
    {
        return processes(EventListenerTypeSet { type }, contentWorldType);
    }

    HashSet<Ref<WebProcessProxy>> processes(EventListenerTypeSet&& typeSet, WebExtensionContentWorldType contentWorldType) const
    {
        return processes(WTF::move(typeSet), ContentWorldTypeSet { contentWorldType });
    }

    HashSet<Ref<WebProcessProxy>> processes(EventListenerTypeSet&&, ContentWorldTypeSet&&, Function<bool(WebProcessProxy&, WebPageProxy&, WebFrameProxy&)>&& predicate = nullptr) const;

    const UserContentControllerProxySet& NODELETE userContentControllers() const LIFETIME_BOUND;

    template<typename T>
    void sendToProcesses(const WebProcessProxySet&, const T& message) const;

    template<typename T>
    void sendToProcessesForEvent(WebExtensionEventListenerType, const T& message) const;

    template<typename T>
    void sendToProcessesForEvents(EventListenerTypeSet&&, const T& message) const;

    template<typename T>
    void sendToContentScriptProcessesForEvent(WebExtensionEventListenerType, const T& message) const;

#ifdef __OBJC__
    WKWebExtensionContext *wrapper() const { return (WKWebExtensionContext *)API::ObjectImpl<API::Object::Type::WebExtensionContext>::wrapper(); }
#endif

private:
    friend class WebExtensionCommand;
    friend class WebExtensionMessagePort;

    explicit WebExtensionContext();

    int NODELETE toAPIError(WebExtensionContext::Error);

    String stateFilePath() const;
#if PLATFORM(COCOA)
    NSDictionary *currentState() const;
    NSDictionary *readStateFromStorage();
#elif ENABLE(2022_GLIB_API)
    GRefPtr<GKeyFile> currentState() const;
    GRefPtr<GKeyFile> readStateFromStorage();
#elif PLATFORM(HAIKU)
    RefPtr<const JSON::Object> currentState() const;
    RefPtr<JSON::Object> readStateFromStorage();
#endif
    void writeStateToStorage() const;
    URL updatePersistedMetadataDuringLoad();
#if PLATFORM(COCOA)
    void clearTabDelegateMap();
#endif

    void loadStorageAccessLevelsFromStorage();
    void saveStorageAccessLevelsToStorage();

    void determineInstallReasonDuringLoad();
    void moveLocalStorageIfNeeded(const URL& previousBaseURL, CompletionHandler<void()>&&);
    void removeStaleExtensionWebsiteData();

#if PLATFORM(HAIKU)
    void notifyNativePermissionChange(PermissionNotification, const PermissionsSet&, const MatchPatternSet&);
#endif

    void permissionsDidChange(PermissionNotification, const PermissionsSet&);
    void permissionsDidChange(PermissionNotification, const MatchPatternSet&);

    bool removePermissions(PermissionsMap&, PermissionsSet&, WallTime& nextExpirationDate, PermissionNotification = PermissionNotification::None);
    bool removePermissionMatchPatterns(PermissionMatchPatternsMap&, MatchPatternSet&, EqualityOnly, WallTime& nextExpirationDate, PermissionNotification = PermissionNotification::None);

    PermissionsMap& removeExpired(PermissionsMap&, WallTime& nextExpirationDate, PermissionNotification = PermissionNotification::None);
    PermissionMatchPatternsMap& removeExpired(PermissionMatchPatternsMap&, WallTime& nextExpirationDate, PermissionNotification = PermissionNotification::None);

    void populateWindowsAndTabs();

    bool isBackgroundPage(WebCore::FrameIdentifier) const;
    bool isBackgroundPage(WebPageProxyIdentifier) const;
    bool NODELETE backgroundContentIsLoaded() const;

    bool isNotRunningInTestRunner();

    void loadBackgroundWebViewDuringLoad();
    void loadBackgroundWebViewIfNeeded();
    void loadBackgroundWebView();
    void unloadBackgroundWebView();
    void scheduleBackgroundContentToUnload();
    void unloadBackgroundContentIfPossible();

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    void unloadOffscreenWebView();
    void performTasksAfterOffscreenContentLoads();
#endif

    uint64_t loadBackgroundPageListenersVersionNumberFromStorage();
    void loadBackgroundPageListenersFromStorage();
    void saveBackgroundPageListenersToStorage();

    void performTasksAfterBackgroundContentLoads();
    void cancelBackgroundContentTasks();

#ifdef NDEBUG
    // This is method is a no-op in release builds since it has a performance impact with little benefit to release builds.
    void reportWebViewConfigurationErrorIfNeeded(const WebExtensionTab&) const { };
#else
    void reportWebViewConfigurationErrorIfNeeded(const WebExtensionTab&) const;
#endif

#if ENABLE(INSPECTOR_EXTENSIONS)
    URL inspectorBackgroundPageURL() const;

    InspectorTabVector openInspectors(Function<bool(WebExtensionTab&, WebInspectorUIProxy&)>&& = nullptr) const;
    InspectorTabVector loadedInspectors() const;

    bool isInspectorBackgroundPage(WKWebView *) const;

    void loadInspectorBackgroundPagesDuringLoad();
    void unloadInspectorBackgroundPages();

    void loadInspectorBackgroundPagesForPrivateBrowsing();
    void unloadInspectorBackgroundPagesForPrivateBrowsing();

    void loadInspectorBackgroundPage(WebInspectorUIProxy&, WebExtensionTab&);
    void unloadInspectorBackgroundPage(WebInspectorUIProxy&);

    RefPtr<API::InspectorExtension> inspectorExtension(WebPageProxyIdentifier) const;
    RefPtr<WebInspectorUIProxy> NODELETE inspector(const API::InspectorExtension&) const;
    HashSet<Ref<WebProcessProxy>> processes(const API::InspectorExtension&) const;
#endif // ENABLE(INSPECTOR_EXTENSIONS)

    API::ContentWorld& toContentWorld(WebExtensionContentWorldType) const;

    void addInjectedContent() { addInjectedContent(injectedContents()); }
    void addInjectedContent(const InjectedContentVector&);
    void addInjectedContent(const InjectedContentVector&, const MatchPatternSet&);
    void addInjectedContent(const InjectedContentVector&, WebExtensionMatchPattern&);

    void updateInjectedContent() { removeInjectedContent(); addInjectedContent(); }

    void removeInjectedContent();
    void removeInjectedContent(const MatchPatternSet&);
    void removeInjectedContent(WebExtensionMatchPattern&);

    // DeclarativeNetRequest methods.
    // Loading/unloading static rules
    void loadDeclarativeNetRequestRules(CompletionHandler<void(bool)>&&);
    void compileDeclarativeNetRequestRules(NSDictionary *, CompletionHandler<void(bool)>&&);
    void unloadDeclarativeNetRequestState();
#if PLATFORM(HAIKU)
    void clearNativeDeclarativeNetRequestRules();
#endif
    String declarativeNetRequestContentRuleListFilePath();

    // Updating user content controllers with new rules.
    void addDeclarativeNetRequestRulesToPrivateUserContentControllers();
    void removeDeclarativeNetRequestRules();

    // Customizing static rulesets.
    void saveDeclarativeNetRequestRulesetStateToStorage(NSDictionary *rulesetState);
    void loadDeclarativeNetRequestRulesetStateFromStorage();
    void clearDeclarativeNetRequestRulesetState();

    // Displaying action count as badge text.
    bool shouldDisplayBlockedResourceCountAsBadgeText();
    void saveShouldDisplayBlockedResourceCountAsBadgeText(bool);

    // Session and dynamic rules.
    Ref<WebExtensionDeclarativeNetRequestSQLiteStore> declarativeNetRequestDynamicRulesStore();
    Ref<WebExtensionDeclarativeNetRequestSQLiteStore> declarativeNetRequestSessionRulesStore();
    void updateDeclarativeNetRequestRulesInStorage(RefPtr<WebExtensionDeclarativeNetRequestSQLiteStore>, const String& storageType, const String& apiName, Ref<JSON::Array> rulesToAdd, Vector<double> ruleIDsToRemove, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);

    DeclarativeNetRequestMatchedRuleVector matchedRules() { return m_matchedRules; }

    // Registered content scripts methods.
    void loadRegisteredContentScripts();
    void clearRegisteredContentScripts();
    Ref<WebExtensionRegisteredScriptsSQLiteStore> registeredContentScriptsStore();

    // Storage
    void setStorageAccessLevel(WebExtensionDataType, WebExtensionStorageAccessLevel);
    WebExtensionStorageAccessLevel storageAccessLevel(WebExtensionDataType dataType) const { return WebKit::storageAccessLevel(m_storageAccessLevels, dataType); }
    bool isStorageAllowedInUntrustedContexts(WebExtensionDataType dataType) const { return isStorageTypeAllowedInUntrustedContexts(m_storageAccessLevels, dataType); }
    size_t quotaForStorageType(WebExtensionDataType);

    Ref<WebExtensionStorageSQLiteStore> localStorageStore();
    Ref<WebExtensionStorageSQLiteStore> sessionStorageStore();
    Ref<WebExtensionStorageSQLiteStore> syncStorageStore();

    void fetchCookies(WebsiteDataStore&, const URL&, const WebExtensionCookieFilterParameters&, CompletionHandler<void(std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
#if PLATFORM(HAIKU)
    void fetchCookies(WebsiteDataStore&, const URL&, const WebExtensionCookieFilterParameters&, bool requireCookieHostPermission, CompletionHandler<void(std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
    void cookiesSetJSON(const String&, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
#endif

    // Action APIs
    bool isActionMessageAllowed(IPC::Decoder&);
    void actionGetTitle(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void actionSetTitle(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, const String& title, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void actionSetIcon(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, const String& iconsJSON, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void actionGetPopup(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void actionSetPopup(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, const String& popupPath, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void actionOpenPopup(WebPageProxyIdentifier, std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void actionGetBadgeText(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void actionSetBadgeText(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, const String& text, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#if PLATFORM(HAIKU)
    void actionGetBadgeColor(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, bool background, CompletionHandler<void(std::expected<uint32_t, WebExtensionError>&&)>&&);
    void actionSetBadgeColor(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, bool background, const String& cssColor, std::optional<uint32_t> color, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#endif
    void actionGetEnabled(std::optional<WebExtensionWindowIdentifier>, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
    void actionSetEnabled(std::optional<WebExtensionTabIdentifier>, bool enabled, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireActionClickedEventIfNeeded(WebExtensionTab*);

    // Alarms APIs
    bool isAlarmsMessageAllowed(IPC::Decoder&);
    void alarmsCreate(const String& name, Seconds initialInterval, Seconds repeatInterval, CompletionHandler<void()>&&);
    void alarmsGet(const String& name, CompletionHandler<void(std::optional<WebExtensionAlarmParameters>&&)>&&);
    void alarmsClear(const String& name, CompletionHandler<void(bool)>&&);
    void alarmsGetAll(CompletionHandler<void(Vector<WebExtensionAlarmParameters>&&)>&&);
    void alarmsClearAll(CompletionHandler<void(bool)>&&);
    void fireAlarmsEventIfNeeded(const WebExtensionAlarm&);

    // Bookmarks APIs
#if ENABLE(WK_WEB_EXTENSIONS_BOOKMARKS)
    bool isBookmarksMessageAllowed(IPC::Decoder&);
    void bookmarksCreate(const std::optional<String>& parentId, const std::optional<uint64_t>& index, const std::optional<String>& url, const std::optional<String>& title, CompletionHandler<void(std::expected<WebExtensionBookmarksParameters, WebExtensionError>&&)>&&);
    void bookmarksGetTree(CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksGetSubTree(const String& bookmarkId, CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksGet(const Vector<String>& bookmarkId, CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksGetChildren(const String& bookmarkId, CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksGetRecent(uint64_t count, CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksSearch(const std::optional<String>& query, const std::optional<String>& url, const std::optional<String>& title, CompletionHandler<void(std::expected<Vector<WebExtensionBookmarksParameters>, WebExtensionError>&&)>&&);
    void bookmarksUpdate(const String& bookmarkId, const std::optional<String>& url, const std::optional<String>& title, CompletionHandler<void(std::expected<WebExtensionBookmarksParameters, WebExtensionError>&&)>&&);
    void bookmarksMove(const String& bookmarkId, const std::optional<String>& parentId, const std::optional<uint64_t>& index, CompletionHandler<void(std::expected<WebExtensionBookmarksParameters, WebExtensionError>&&)>&&);
    void bookmarksRemove(const String& bookmarkId, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void bookmarksRemoveTree(const String& bookmarkId, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#endif
    // Commands APIs
    bool isCommandsMessageAllowed(IPC::Decoder&);
    void commandsGetAll(CompletionHandler<void(Vector<WebExtensionCommandParameters>)>&&);
#if PLATFORM(HAIKU)
    void commandsUpdate(const String&, const std::optional<String>& description, const std::optional<String>& shortcut, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void commandsReset(const String&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    std::expected<void, WebExtensionError> saveCommandState(Ref<JSON::Object>&&);
#endif
    void fireCommandEventIfNeeded(const WebExtensionCommand&, WebExtensionTab*);
    void fireCommandChangedEventIfNeeded(const WebExtensionCommand&, const String& oldShortcut);

    // Cookies APIs
    bool isCookiesMessageAllowed(IPC::Decoder&);
    void cookiesGet(std::optional<PAL::SessionID>, const String& name, const URL&, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
    void cookiesGetAll(std::optional<PAL::SessionID>, const URL&, const WebExtensionCookieFilterParameters&, CompletionHandler<void(std::expected<Vector<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
    void cookiesSet(std::optional<PAL::SessionID>, const WebExtensionCookieParameters&, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
    void cookiesRemove(std::optional<PAL::SessionID>, const String& name, const URL&, CompletionHandler<void(std::expected<std::optional<WebExtensionCookieParameters>, WebExtensionError>&&)>&&);
    void cookiesGetAllCookieStores(CompletionHandler<void(std::expected<HashMap<PAL::SessionID, Vector<WebExtensionTabIdentifier>>, WebExtensionError>&&)>&&);
#if !PLATFORM(HAIKU)
    void fireCookiesChangedEventIfNeeded();
#endif

    // DeclarativeNetRequest APIs
    bool isDeclarativeNetRequestMessageAllowed(IPC::Decoder&);
    void declarativeNetRequestGetEnabledRulesets(CompletionHandler<void(Vector<String>&&)>&&);
    void declarativeNetRequestUpdateEnabledRulesets(const Vector<String>& rulesetIdentifiersToEnable, const Vector<String>& rulesetIdentifiersToDisable, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void declarativeNetRequestDisplayActionCountAsBadgeText(bool displayActionCountAsBadgeText, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void declarativeNetRequestIncrementActionCount(WebExtensionTabIdentifier, double increment, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    DeclarativeNetRequestValidatedRulesets declarativeNetRequestValidateRulesetIdentifiers(const Vector<String>&);
    size_t declarativeNetRequestEnabledRulesetCount() { return m_enabledStaticRulesetIDs.size(); }
    void declarativeNetRequestToggleRulesets(const Vector<String>& rulesetIdentifiers, bool newValue, NSMutableDictionary *rulesetIdentifiersToEnabledState);
    void declarativeNetRequestGetMatchedRules(std::optional<WebExtensionTabIdentifier>, std::optional<WallTime> minTimeStamp, CompletionHandler<void(std::expected<Vector<WebExtensionMatchedRuleParameters>, WebExtensionError>&&)>&&);
    void declarativeNetRequestGetDynamicRules(Vector<double>&& filter, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void declarativeNetRequestUpdateDynamicRules(String&& rulesToAddJSON, Vector<double>&& ruleIDsToDelete, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void declarativeNetRequestGetSessionRules(Vector<double>&& filter, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void declarativeNetRequestUpdateSessionRules(String&& rulesToAddJSON, Vector<double>&& ruleIDsToDelete, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#if PLATFORM(HAIKU)
    void declarativeNetRequestGetAvailableStaticRuleCount(CompletionHandler<void(double)>&&);
    // Native content rules enforce DNR for the whole browser. An extension with only
    // declarativeNetRequestWithHostAccess gets them when it may access every URL,
    // where host-limited enforcement would change nothing.
    bool nativeDeclarativeNetRequestAllowed();
    std::expected<void, WebExtensionError> validateNativeDeclarativeNetRequestUpdate(const String& apiName, const JSON::Array& rulesToAdd, const Vector<double>& ruleIDsToDelete, bool session);
    void saveDeclarativeNetRequestRulesetStateToStorage(const HashMap<String, bool>&);

    // Downloads APIs
    bool isDownloadsMessageAllowed(IPC::Decoder&);
    void downloadsDownload(WebPageProxyIdentifier, String&& optionsJSON, CompletionHandler<void(std::expected<double, WebExtensionError>&&)>&&);
    void downloadsSearch(String&& queryJSON, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void downloadsCancel(double downloadIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void downloadsErase(String&& queryJSON, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void downloadsShow(double downloadIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void downloadsShowDefaultFolder(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
public:
    void fireDownloadsEventIfNeeded(WebExtensionEventListenerType, const String& argumentJSON);
private:
#endif

#if ENABLE(INSPECTOR_EXTENSIONS)
    // DevTools APIs
    bool isDevToolsMessageAllowed(IPC::Decoder&);
    void devToolsPanelsCreate(WebPageProxyIdentifier, const String& title, const String& iconPath, const String& pagePath, CompletionHandler<void(std::expected<Inspector::ExtensionTabID, WebExtensionError>&&)>&&);
    void devToolsInspectedWindowEval(WebPageProxyIdentifier, const String& scriptSource, const std::optional<URL>& frameURL, CompletionHandler<void(std::expected<RunJavaScriptResult, WebExtensionError>&&)>&&);
    void devToolsInspectedWindowReload(WebPageProxyIdentifier, const std::optional<bool>& ignoreCache);
#endif

    // Event APIs
    void addListener(WebCore::FrameIdentifier, WebExtensionEventListenerType, WebExtensionContentWorldType);
    void removeListener(WebCore::FrameIdentifier, WebExtensionEventListenerType, WebExtensionContentWorldType, uint64_t removedCount);

    // Extension APIs
    void extensionIsAllowedIncognitoAccess(CompletionHandler<void(bool)>&&);
#if PLATFORM(HAIKU)
    void extensionIsAllowedFileSchemeAccess(CompletionHandler<void(bool)>&&);
#endif

    // Runtime Error Reporting
    void didEncounterScriptError(const String& message, const String& sourceURL, uint32_t lineNumber, uint32_t columnNumber, WebExtensionContentWorldType);

    // Menus APIs
    bool isMenusMessageAllowed(IPC::Decoder&);
#if PLATFORM(HAIKU)
    void menusCreateWithCallback(const WebExtensionMenuItemParameters&, bool explicitIdentifier, const std::optional<WTF::UUID>&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void menusUpdateWithCallback(const String&, const WebExtensionMenuItemParameters&, bool callbackSpecified, const std::optional<WTF::UUID>&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void removeMenusClickHandlers(const Vector<WTF::UUID>&);
#endif
    void menusCreate(const WebExtensionMenuItemParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void menusUpdate(const String& identifier, const WebExtensionMenuItemParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void menusRemove(const String& identifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void menusRemoveAll(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireMenusClickedEventIfNeeded(const WebExtensionMenuItem&, bool wasChecked, const WebExtensionMenuItemContextParameters&);

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    // Offscreen APIs
    bool isOffscreenMessageAllowed(IPC::Decoder&);

    void offscreenCreateDocument(const WebExtensionOffscreenDocumentParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void offscreenCloseDocument(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void offscreenHasDocument(CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
#endif

    // Permissions APIs
    void permissionsGetAll(CompletionHandler<void(Vector<String>&& permissions, Vector<String>&& origins)>&&);
#if PLATFORM(HAIKU)
    void permissionsContains(HashSet<String> permissions, HashSet<String> origins, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
    void permissionsRequest(HashSet<String> permissions, HashSet<String> origins, bool userGesture, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
    void permissionsRemove(HashSet<String> permissions, HashSet<String> origins, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
    WebExtensionPermissionState nativePermissionState() const;
    std::expected<void, String> restoreNativePermissionState();
    std::expected<void, WebExtensionError> commitNativePermissionState(PermissionsMap&&, PermissionMatchPatternsMap&&, const String& callingAPI);
#else
    void permissionsContains(HashSet<String> permissions, HashSet<String> origins, CompletionHandler<void(bool)>&&);
    void permissionsRequest(HashSet<String> permissions, HashSet<String> origins, CompletionHandler<void(bool)>&&);
    void permissionsRemove(HashSet<String> permissions, HashSet<String> origins, CompletionHandler<void(bool)>&&);
#endif
    void firePermissionsEventListenerIfNecessary(WebExtensionEventListenerType, const PermissionsSet&, const MatchPatternSet&);

    // Port APIs
    void portPostMessage(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, std::optional<WebKit::WebPageProxyIdentifier>, WebExtensionPortChannelIdentifier, const String& messageJSON, bool userGesture);
    void portRemoved(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebPageProxyIdentifier, WebExtensionPortChannelIdentifier);
    bool pageHasOpenPorts(WebPageProxy&);
    void disconnectPortsForPage(WebPageProxy&);
    void addPorts(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier, HashCountedSet<WebPageProxyIdentifier>&&);
    void removePort(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier, WebPageProxyIdentifier);
    bool addNativePort(WebExtensionMessagePort&);
    void removeNativePort(WebExtensionMessagePort&);
    unsigned openPortCount(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier);
    bool isPortConnected(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier);
    void clearQueuedPortMessages(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier);
    Vector<PortQueuedMessage> portQueuedMessages(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier);
    void firePortMessageEventsIfNeeded(WebExtensionContentWorldType, std::optional<WebPageProxyIdentifier>, WebExtensionPortChannelIdentifier, const String& messageJSON, bool userGesture);
    void fireQueuedPortMessageEventsIfNeeded(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier);
    void sendQueuedNativePortMessagesIfNeeded(WebExtensionPortChannelIdentifier);
    void firePortDisconnectEventIfNeeded(WebExtensionContentWorldType sourceContentWorldType, WebExtensionContentWorldType targetContentWorldType, WebExtensionPortChannelIdentifier, const std::optional<String>& errorMessage = std::nullopt);

    // Runtime APIs
    void runtimeGetBackgroundPage(CompletionHandler<void(std::expected<std::optional<WebCore::PageIdentifier>, WebExtensionError>&&)>&&);
    void runtimeOpenOptionsPage(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void runtimeReload();
    void runtimeSendMessage(const String& extensionID, const String& messageJSON, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void runtimeConnect(const String& extensionID, WebExtensionPortChannelIdentifier, const String& name, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void runtimeSendNativeMessage(const String& applicationID, const String& messageJSON, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void runtimeConnectNative(const String& applicationID, WebExtensionPortChannelIdentifier, WebPageProxyIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void runtimeWebPageSendMessage(const String& extensionID, const String& messageJSON, const WebExtensionMessageSenderParameters&, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void runtimeWebPageConnect(const String& extensionID, WebExtensionPortChannelIdentifier, const String& name, const WebExtensionMessageSenderParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireRuntimeStartupEventIfNeeded();
    void fireRuntimeInstalledEventIfNeeded();

    // Scripting APIs
    bool isScriptingMessageAllowed(IPC::Decoder&);
    void scriptingExecuteScript(const WebExtensionScriptInjectionParameters&, bool userGesture, CompletionHandler<void(std::expected<Vector<WebExtensionScriptInjectionResultParameters>, WebExtensionError>&&)>&&);
    void scriptingInsertCSS(const WebExtensionScriptInjectionParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void scriptingRemoveCSS(const WebExtensionScriptInjectionParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void scriptingRegisterContentScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void scriptingUpdateRegisteredScripts(const Vector<WebExtensionRegisteredScriptParameters>& scripts, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void scriptingGetRegisteredScripts(const Vector<String>&, CompletionHandler<void(std::expected<Vector<WebExtensionRegisteredScriptParameters>, WebExtensionError>&&)>&&);
    void scriptingUnregisterContentScripts(const Vector<String>&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    bool createInjectedContentForScripts(const Vector<WebExtensionRegisteredScriptParameters>&, WebExtensionDynamicScripts::WebExtensionRegisteredScript::FirstTimeRegistration, DynamicInjectedContentsMap&, NSString *callingAPIName, NSString **errorMessage);
    bool createInjectedContentForScripts(const Vector<WebExtensionRegisteredScriptParameters>&, WebExtensionDynamicScripts::WebExtensionRegisteredScript::FirstTimeRegistration, DynamicInjectedContentsMap&, const String& callingAPIName, String* errorMessage);
#if PLATFORM(HAIKU)
    // scripting.executeScript() through the native tab-script executor: results
    // stay structured clones and carry the frame they came from.
    void scriptingExecuteScriptNative(const WebExtensionScriptInjectionParameters&, bool injectImmediately, bool userGesture, CompletionHandler<void(std::expected<Vector<WebExtensionTabScriptFrameResult>, WebExtensionError>&&)>&&);
    void executeNativeScriptSources(WebPageProxy&, API::ContentWorld& contentScriptWorld, bool mainWorld, const WebExtensionTabScriptTarget&, const URL& authorizedURL, WebExtensionDynamicScripts::SourcePairs&&, size_t index, bool userGesture, CompletionHandler<void(WebExtensionTabScriptExecutionResult&&)>&&);
    void insertNativeCSS(WebExtensionTab*, const WebExtensionScriptInjectionParameters&, const String& apiName, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void removeNativeCSS(WebExtensionTab*, const WebExtensionScriptInjectionParameters&, const String& apiName, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#endif

    // Sidebar APIs
#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
    bool isSidebarMessageAllowed(IPC::Decoder&);
    void sidebarOpen(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarIsOpen(const std::optional<WebExtensionWindowIdentifier>, CompletionHandler<void(std::expected<bool, WebExtensionError>&&)>&&);
    void sidebarClose(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarGetLayout(CompletionHandler<void(std::expected<WebExtensionSidebarSide, WebExtensionError>&&)>&&);
    void sidebarToggle(CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarGetOptions(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<WebExtensionSidebarParameters, WebExtensionError>&&)>&&);
    void sidebarSetOptions(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, const std::optional<String>& panelSourcePath, const std::optional<bool> enabled, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarGetTitle(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void sidebarSetTitle(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, const std::optional<String>& title, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarSetIcon(const std::optional<WebExtensionWindowIdentifier>, const std::optional<WebExtensionTabIdentifier>, const String& iconJSON, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarSetActionClickBehavior(WebExtensionActionClickBehavior, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void sidebarGetActionClickBehavior(CompletionHandler<void(std::expected<WebExtensionActionClickBehavior, WebExtensionError>&&)>&&);
#endif

    // Storage APIs
    bool isStorageMessageAllowed(IPC::Decoder&);
    void storageGet(WebPageProxyIdentifier, WebExtensionDataType, const Vector<String>& keys, CompletionHandler<void(std::expected<Vector<String>, WebExtensionError>&&)>&&);
    void storageGetKeys(WebPageProxyIdentifier, WebExtensionDataType, CompletionHandler<void(std::expected<Vector<String>, WebExtensionError>&&)>&&);
    void storageGetBytesInUse(WebPageProxyIdentifier, WebExtensionDataType, const Vector<String>& keys, CompletionHandler<void(std::expected<uint64_t, WebExtensionError>&&)>&&);
    void storageSet(WebPageProxyIdentifier, WebExtensionDataType, const String& dataJSON, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void storageRemove(WebPageProxyIdentifier, WebExtensionDataType, const Vector<String>& keys, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void storageClear(WebPageProxyIdentifier, WebExtensionDataType, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void storageSetAccessLevel(WebPageProxyIdentifier, WebExtensionDataType, WebExtensionStorageAccessLevel, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireStorageChangedEventIfNeeded(HashMap<String, String> oldKeysAndValues, HashMap<String, String> newKeysAndValues, WebExtensionDataType);

    // Tabs APIs
    void tabsCreate(std::optional<WebPageProxyIdentifier>, const WebExtensionTabParameters&, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsUpdate(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, const WebExtensionTabParameters&, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsDuplicate(WebExtensionTabIdentifier, const WebExtensionTabParameters&, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsGet(WebExtensionTabIdentifier, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsGetCurrent(WebPageProxyIdentifier, CompletionHandler<void(std::expected<std::optional<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsQuery(WebPageProxyIdentifier, const WebExtensionTabQueryParameters&, CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsReload(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, ReloadFromOrigin, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsGoBack(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsGoForward(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsDetectLanguage(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void tabsCaptureVisibleTab(WebPageProxyIdentifier, std::optional<WebExtensionWindowIdentifier>, WebExtensionTab::ImageFormat, uint8_t imageQuality, CompletionHandler<void(std::expected<URL, WebExtensionError>&&)>&&);
    void tabsToggleReaderMode(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsSendMessage(WebExtensionTabIdentifier, const String& messageJSON, const WebExtensionMessageTargetParameters&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(std::expected<String, WebExtensionError>&&)>&&);
    void tabsConnect(WebExtensionTabIdentifier, WebExtensionPortChannelIdentifier, String name, const WebExtensionMessageTargetParameters&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsGetZoom(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<double, WebExtensionError>&&)>&&);
    void tabsSetZoom(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, double, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsMove(Vector<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>, double, CompletionHandler<void(std::expected<Vector<WebExtensionTabParameters>, WebExtensionError>&&)>&&);
    void tabsRemove(Vector<WebExtensionTabIdentifier>, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
#if PLATFORM(HAIKU)
    void tabsExecuteScriptNative(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, const WebExtensionTabScriptParameters&, bool userGesture, CompletionHandler<void(std::expected<Vector<RefPtr<WebCore::SerializedScriptValue>>, WebExtensionError>&&)>&&);
#endif
    void tabsExecuteScript(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, const WebExtensionScriptInjectionParameters&, bool userGesture, CompletionHandler<void(std::expected<Vector<WebExtensionScriptInjectionResultParameters>, WebExtensionError>&&)>&&);
    void tabsInsertCSS(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, const WebExtensionScriptInjectionParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void tabsRemoveCSS(WebPageProxyIdentifier, std::optional<WebExtensionTabIdentifier>, const WebExtensionScriptInjectionParameters&, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireTabsCreatedEventIfNeeded(const WebExtensionTabParameters&);
    void fireTabsUpdatedEventIfNeeded(const WebExtensionTabParameters&, const WebExtensionTabParameters& changedParameters);
    void fireTabsReplacedEventIfNeeded(WebExtensionTabIdentifier replacedTabIdentifier, WebExtensionTabIdentifier newTabIdentifier);
    void fireTabsDetachedEventIfNeeded(WebExtensionTabIdentifier, WebExtensionWindowIdentifier oldWindowIdentifier, size_t oldIndex);
    void fireTabsMovedEventIfNeeded(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, size_t oldIndex, size_t newIndex);
    void fireTabsAttachedEventIfNeeded(WebExtensionTabIdentifier, WebExtensionWindowIdentifier newWindowIdentifier, size_t newIndex);
    void fireTabsActivatedEventIfNeeded(WebExtensionTabIdentifier previousActiveTabIdentifier, WebExtensionTabIdentifier newActiveTabIdentifier, WebExtensionWindowIdentifier);
    void fireTabsHighlightedEventIfNeeded(Vector<WebExtensionTabIdentifier>, WebExtensionWindowIdentifier);
    void fireTabsRemovedEventIfNeeded(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, WindowIsClosing);

    // WebNavigation APIs
    bool isWebNavigationMessageAllowed(IPC::Decoder&);
    void webNavigationGetFrame(WebExtensionTabIdentifier, WebExtensionFrameIdentifier, CompletionHandler<void(std::expected<std::optional<WebExtensionFrameParameters>, WebExtensionError>&&)>&&);
    void webNavigationGetAllFrames(WebExtensionTabIdentifier, CompletionHandler<void(std::expected<Vector<WebExtensionFrameParameters>, WebExtensionError>&&)>&&);
    void webNavigationTraverseFrameTreeForFrame(_WKFrameTreeNode *, _WKFrameTreeNode *parentFrame, WebExtensionTab*, Vector<WebExtensionFrameParameters>&);
    std::optional<WebExtensionFrameParameters> webNavigationFindFrameIdentifierInFrameTree(_WKFrameTreeNode *, _WKFrameTreeNode *parentFrame, WebExtensionTab*, WebExtensionFrameIdentifier);

    // Windows APIs
    void windowsCreate(const WebExtensionWindowParameters&, CompletionHandler<void(std::expected<std::optional<WebExtensionWindowParameters>, WebExtensionError>&&)>&&);
    void windowsGet(WebPageProxyIdentifier, WebExtensionWindowIdentifier, OptionSet<WindowTypeFilter>, PopulateTabs, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&&);
    void windowsGetLastFocused(OptionSet<WindowTypeFilter>, PopulateTabs, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&&);
    void windowsGetAll(OptionSet<WindowTypeFilter>, PopulateTabs, CompletionHandler<void(std::expected<Vector<WebExtensionWindowParameters>, WebExtensionError>&&)>&&);
    void windowsUpdate(WebExtensionWindowIdentifier, WebExtensionWindowParameters, CompletionHandler<void(std::expected<WebExtensionWindowParameters, WebExtensionError>&&)>&&);
    void windowsRemove(WebExtensionWindowIdentifier, CompletionHandler<void(std::expected<void, WebExtensionError>&&)>&&);
    void fireWindowsEventIfNeeded(WebExtensionEventListenerType, std::optional<WebExtensionWindowParameters>);

    // webRequest support.
    bool hasPermissionToSendWebRequestEvent(WebExtensionTab*, const URL& resourceURL, const ResourceLoadInfo&);
#if PLATFORM(HAIKU)
public:
    // Blocking webRequest (UIProcess/Extensions/haiku/WebExtensionContextWebRequestBlockingHaiku.cpp).
    bool hasBlockingWebRequestListenersHaiku(WebExtensionEventListenerType) const;
    bool canBlockWebRequestHaiku(WebPageProxyIdentifier, const URL& resourceURL, const ResourceLoadInfo&, uint8_t phase);
    // Invokes this extension's blocking listeners for phase and replies with
    // (listener order, JSON reply) pairs. Replies are untrusted and undecoded.
    void dispatchBlockingWebRequestHaiku(uint8_t phase, WebPageProxyIdentifier, const ResourceLoadInfo&, const WebCore::ResourceRequest&, const std::optional<WebCore::ResourceResponse>&, const std::optional<WebCore::AuthenticationChallenge>&, CompletionHandler<void(Vector<std::pair<uint64_t, String>>&&)>&&);

private:
    void addBlockingWebRequestListenerHaiku(WebCore::FrameIdentifier, WebExtensionEventListenerType);
    void removeBlockingWebRequestListenerHaiku(WebCore::FrameIdentifier, WebExtensionEventListenerType, uint64_t removedCount);
#endif

    // IPC::MessageReceiver.
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    bool isLoaded(IPC::Decoder&) const { return isLoaded(); }
    bool isLoadedAndPrivilegedMessage(IPC::Decoder& message) const { return isLoaded() && isPrivilegedMessage(message); }
    bool NODELETE isPrivilegedMessage(IPC::Decoder&) const;

    mutable Markable<WebExtensionContextIdentifier> m_privilegedIdentifier;

    String m_storageDirectory;

#if PLATFORM(COCOA)
    RetainPtr<NSMutableDictionary> m_state;
#elif ENABLE(2022_GLIB_API)
    GRefPtr<GKeyFile> m_state;
#elif PLATFORM(HAIKU)
    RefPtr<JSON::Object> m_state;
    RefPtr<NativePermissionChangeHandler> m_nativePermissionChangeHandler;
    uint64_t m_nativePermissionChangeHandlerGeneration { 0 };
    RefPtr<ExtensionActionPopupHaiku> m_nativeActionPopup;
    RefPtr<NativeActionChangeHandler> m_nativeActionChangeHandler;
    uint64_t m_nativeActionChangeHandlerGeneration { 0 };
#endif
    Vector<Ref<API::Error>> m_errors;

    RefPtr<WebExtension> m_extension;
    WeakPtr<WebExtensionController> m_extensionController;

    URL m_baseURL;
    String m_uniqueIdentifier = WTF::UUID::createVersion4().toString();
    bool m_customUniqueIdentifier { false };

    RefPtr<WebExtensionLocalization> m_localization;

    bool m_inspectable { false };

    HashSet<String> m_unsupportedAPIs;

    RefPtr<API::ContentWorld> m_contentScriptWorld;

#if PLATFORM(HAIKU)
    bool m_nativePermissionStateInitialized { false };
    bool m_nativeGrantedPermissionsWereSet { false };
    bool m_nativeGrantedPatternsWereSet { false };
    bool m_nativePermissionTransaction { false };
    uint64_t m_nativePermissionRevocationGeneration { 0 };
    PermissionsSet m_nativeRemovedPermissionsBeforeLoad;
    Vector<std::pair<Ref<WebExtensionMatchPattern>, EqualityOnly>> m_nativeRemovedPatternsBeforeLoad;
#endif
    PermissionsMap m_grantedPermissions;
    PermissionsMap m_deniedPermissions;

    WallTime m_nextGrantedPermissionsExpirationDate { WallTime::nan() };
    WallTime m_nextDeniedPermissionsExpirationDate { WallTime::nan() };

    PermissionMatchPatternsMap m_grantedPermissionMatchPatterns;
    PermissionMatchPatternsMap m_deniedPermissionMatchPatterns;

    WallTime m_nextGrantedPermissionMatchPatternsExpirationDate { WallTime::nan() };
    WallTime m_nextDeniedPermissionMatchPatternsExpirationDate { WallTime::nan() };

    ListHashSet<URL> m_cachedPermissionURLs;
    HashMap<URL, PermissionState> m_cachedPermissionStates;

    size_t m_pendingPermissionRequests { 0 };

    bool m_requestedOptionalAccessToAllHosts { false };
    bool m_hasAccessToPrivateData { false };
    bool m_hasAccessToFileURLs { false };

    VoidFunctionVector m_actionsToPerformAfterBackgroundContentLoads;
    EventListenerTypeCountedSet m_backgroundContentEventListeners;
    EventListenerTypeFrameMap m_eventListenerFrames;

#if PLATFORM(HAIKU)
    HashMap<WebExtensionEventListenerType, WeakFrameCountedSet> m_blockingWebRequestListenerFramesHaiku;
    WebExtensionLoadPurposeHaiku m_nativeLoadPurpose { WebExtensionLoadPurposeHaiku::UserInitiated };
    String m_nativeInstallationIdentifier;
    uint64_t m_nativeInstallationOrder { 0 };
    WebExtensionPrivacySettingsHaiku m_nativePrivacySettings;
    uint64_t m_nativePrivacyGeneration { 0 };
#endif
    bool m_shouldFireStartupEvent { false };
    InstallReason m_installReason { InstallReason::None };
    String m_previousVersion;

#if PLATFORM(COCOA)
    RetainPtr<WKWebView> m_backgroundWebView;

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    RetainPtr<WKWebView> m_offscreenWebView;
    Variant<std::monostate, Ref<ProcessThrottlerActivity>, Ref<ProcessActivityGroup>> m_offscreenWebViewActivity;
    Vector<CompletionHandler<void(std::expected<void, WebExtensionError>&&)>> m_offscreenDocumentLoadCompletionHandlers;
#if PLATFORM(MAC)
    RetainPtr<NSWindow> m_offscreenWebViewWindow;
#elif PLATFORM(IOS_FAMILY)
    RetainPtr<UIWindow> m_offscreenWebViewWindow;
#endif
#endif

    RetainPtr<_WKWebExtensionContextDelegate> m_delegate;
#elif ENABLE(2022_GLIB_API)
    GRefPtr<WebKitWebView> m_backgroundWebView;
    GWeakPtr<WebKitWebExtensionContext> m_delegate;
#elif PLATFORM(HAIKU)
    RefPtr<WebPageProxy> m_backgroundWebView;
    RefPtr<WebView> m_backgroundPageClient;
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    // chrome.offscreen: at most one hidden extension document, owned like the background page.
    RefPtr<WebPageProxy> m_offscreenWebView;
    RefPtr<WebView> m_offscreenPageClient;
    Variant<std::monostate, Ref<ProcessThrottlerActivity>, Ref<ProcessActivityGroup>> m_offscreenWebViewActivity;
    Vector<CompletionHandler<void(std::expected<void, WebExtensionError>&&)>> m_offscreenDocumentLoadCompletionHandlers;
#endif
#endif
    Variant<std::monostate, Ref<ProcessThrottlerActivity>, Ref<ProcessActivityGroup>> m_backgroundWebViewActivity;
    RefPtr<API::Error> m_backgroundContentLoadError;

    String m_backgroundWebViewInspectionName;

    std::unique_ptr<RunLoop::Timer> m_unloadBackgroundWebViewTimer;
    MonotonicTime m_lastBackgroundPortActivityTime;
    bool m_backgroundContentIsLoaded { false };
    bool m_backgroundContentHasLoadedOnce { false };
    bool m_safeToLoadBackgroundContent { false };

#if ENABLE(INSPECTOR_EXTENSIONS)
    WeakHashMap<WebInspectorUIProxy, InspectorContext> m_inspectorContextMap;
#endif

    bool m_safeToInjectContent { false };

    HashMap<Ref<WebExtensionMatchPattern>, UserScriptVector> m_injectedScriptsPerPatternMap;
    HashMap<Ref<WebExtensionMatchPattern>, UserStyleSheetVector> m_injectedStyleSheetsPerPatternMap;

    uint64_t m_registeredContentScriptsGeneration { 0 };
    HashMap<String, Ref<WebExtensionDynamicScripts::WebExtensionRegisteredScript>> m_registeredScriptsMap;
    RefPtr<WebExtensionRegisteredScriptsSQLiteStore> m_registeredContentScriptsStorage;

    UserStyleSheetVector m_dynamicallyInjectedUserStyleSheets;

    HashMap<String, Ref<WebExtensionAlarm>> m_alarmMap;
    WeakHashMap<WebExtensionWindow, Ref<WebExtensionAction>> m_actionWindowMap;
    WeakHashMap<WebExtensionTab, Ref<WebExtensionAction>> m_actionTabMap;
    RefPtr<WebExtensionAction> m_defaultAction;

#if ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)
    WeakHashMap<WebExtensionWindow, Ref<WebExtensionSidebar>> m_sidebarWindowMap;
    WeakHashMap<WebExtensionTab, Ref<WebExtensionSidebar>> m_sidebarTabMap;
    WeakHashMap<WebPageProxy, WeakPtr<WebExtensionSidebar>> m_sidebarPageMap;
    RefPtr<WebExtensionSidebar> m_defaultSidebar;
    WebExtensionActionClickBehavior m_actionClickBehavior { WebExtensionActionClickBehavior::OpenPopup };
#endif // ENABLE(WK_WEB_EXTENSIONS_SIDEBAR)

    PortCountedSet m_ports;
    WebExtensionPagePortMap m_pagePortMap;
    PortQueuedMessageMap m_portQueuedMessages;
    NativePortMap m_nativePortMap;

    mutable WindowIdentifierMap m_windowMap;
#if PLATFORM(HAIKU)
    HashMap<uint64_t, WebExtensionWindowIdentifier> m_nativeWindowEventIdentifiers;
    // The public identity last reported for each registered page, kept until
    // its removal event: the wrapper itself may already have been pruned.
    HashMap<WebPageProxyIdentifier, std::pair<WebExtensionTabIdentifier, WebExtensionWindowIdentifier>> m_nativeTabEventIdentifiers;
#endif
    mutable WindowIdentifierVector m_windowOrderVector;
    mutable std::optional<WebExtensionWindowIdentifier> m_focusedWindowIdentifier;

    mutable TabIdentifierMap m_tabMap;
    PageTabIdentifierMap m_extensionPageTabMap;
    PopupPageActionMap m_popupPageActionMap;

#if PLATFORM(COCOA)
    RetainPtr<NSMapTable> m_tabDelegateToIdentifierMap;
#endif

    CommandsVector m_commands;
    bool m_populatedCommands { false };

    String m_declarativeNetRequestContentRuleListFilePath;
#if PLATFORM(HAIKU)
    uint64_t m_nativeDNRLoadGeneration { 0 };
    RefPtr<API::ContentRuleList> m_nativeDNRRules;
    String m_nativeDNRRuleFile;
    size_t m_nativeDNRStaticRuleCount { 0 };
#endif
    DeclarativeNetRequestMatchedRuleVector m_matchedRules;
    RefPtr<WebExtensionDeclarativeNetRequestSQLiteStore> m_declarativeNetRequestDynamicRulesStore;
    RefPtr<WebExtensionDeclarativeNetRequestSQLiteStore> m_declarativeNetRequestSessionRulesStore;
    HashSet<String> m_enabledStaticRulesetIDs;
    HashSet<double> m_sessionRulesIDs;
    HashSet<double> m_dynamicRulesIDs;

#if PLATFORM(HAIKU)
    HashMap<String, WTF::UUID> m_menuClickHandlerIdentifiers;
#endif
    MenuItemMap m_menuItems;
    MenuItemVector m_mainMenuItems;

    WebExtensionStorageAccessLevelMap m_storageAccessLevels;

    RefPtr<WebExtensionStorageSQLiteStore> m_localStorageStore;
    RefPtr<WebExtensionStorageSQLiteStore> m_sessionStorageStore;
    RefPtr<WebExtensionStorageSQLiteStore> m_syncStorageStore;

    struct TestMessage {
        String message;
#if PLATFORM(COCOA)
        RetainPtr<id> argument;
#elif PLATFORM(HAIKU)
        Ref<JSON::Value> argument;
#endif
    };

#if !PLATFORM(HAIKU)
    size_t m_testMessageListenersCount { 0 };
    size_t m_testStartedListenersCount { 0 };
    size_t m_testFinishedListenersCount { 0 };
#endif

    Deque<TestMessage> m_testMessageQueue;
    Deque<TestMessage> m_testStartedQueue;
    Deque<TestMessage> m_testFinishedQueue;

    bool NODELETE hasTestEventListeners(WebExtensionEventListenerType);
    void sendQueuedTestMessagesIfNeeded(WebExtensionEventListenerType);
#if PLATFORM(COCOA)
    void addTestMessageToQueue(const String& message, id argument, WebExtensionEventListenerType);
#elif PLATFORM(HAIKU)
    void addTestMessageToQueue(const String& message, Ref<JSON::Value> argument, WebExtensionEventListenerType);
#endif
};

template<typename T>
void WebExtensionContext::sendToProcesses(const WebProcessProxySet& processes, const T& message) const
{
    if (!isLoaded())
        return;

    for (auto& process : processes)
        process->send(T(message), identifier());
}

template<typename T>
void WebExtensionContext::sendToProcessesForEvent(WebExtensionEventListenerType type, const T& message) const
{
    sendToProcesses(processes(type, WebExtensionContentWorldType::Main), message);
}

template<typename T>
void WebExtensionContext::sendToProcessesForEvents(EventListenerTypeSet&& typeSet, const T& message) const
{
    sendToProcesses(processes(WTF::move(typeSet), WebExtensionContentWorldType::Main), message);
}

template<typename T>
void WebExtensionContext::sendToContentScriptProcessesForEvent(WebExtensionEventListenerType type, const T& message) const
{
    sendToProcesses(processes(type, WebExtensionContentWorldType::ContentScript), message);
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
