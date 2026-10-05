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

#include "MessageReceiver.h"
#include "WebExtensionContext.h"
#include "WebExtensionContextParameters.h"
#include <WebCore/DOMWrapperWorld.h>
#include <WebCore/FrameIdentifier.h>
#include <WebCore/PageIdentifier.h>
#include <wtf/Forward.h>
#include <wtf/TZoneMalloc.h>
#include <wtf/WeakHashSet.h>
#include <wtf/WeakPtr.h>

OBJC_CLASS JSValue;
OBJC_CLASS NSDictionary;

namespace WebCore { struct CookieChange; }

namespace WebKit {

class WebExtensionAPINamespace;
class WebExtensionMenuClickHandlerStore;
class WebExtensionAPIStorage;
class WebExtensionAPIWebPageNamespace;
class WebExtensionControllerProxy;
class WebExtensionMatchPattern;
class WebFrame;

struct WebExtensionAlarmParameters;
struct WebExtensionFrameParameters;
struct WebExtensionTabParameters;
struct WebExtensionWindowParameters;

class WebExtensionContextProxy final : public RefCounted<WebExtensionContextProxy>, public IPC::MessageReceiver {
    WTF_MAKE_TZONE_ALLOCATED(WebExtensionContextProxy);
    WTF_MAKE_NONCOPYABLE(WebExtensionContextProxy);

public:
    static RefPtr<WebExtensionContextProxy> NODELETE get(WebExtensionContextIdentifier);
    static Ref<WebExtensionContextProxy> getOrCreate(const WebExtensionContextParameters&, WebExtensionControllerProxy&, WebPage* = nullptr);

    ~WebExtensionContextProxy();

    void ref() const final { RefCounted::ref(); }
    void deref() const final { RefCounted::deref(); }

    using WeakFrameSet = WeakHashSet<WebFrame>;
    using TabWindowIdentifierPair = std::pair<std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>>;
    using WeakPageTabWindowMap = WeakHashMap<WebPage, TabWindowIdentifierPair>;
    using PermissionsMap = WebExtensionContext::PermissionsMap;

    WebExtensionContextIdentifier identifier() const { return m_privilegedIdentifier ? *m_privilegedIdentifier : m_unprivilegedIdentifier; }
    WebExtensionContextIdentifier unprivilegedIdentifier() const { return m_unprivilegedIdentifier; }
    Markable<WebExtensionContextIdentifier> privilegedIdentifier() const { return m_privilegedIdentifier; }

    WebExtensionControllerProxy* NODELETE extensionControllerProxy() const;

    bool operator==(const WebExtensionContextProxy& other) const { return (this == &other); }

    const URL& baseURL() const LIFETIME_BOUND { return m_baseURL; }
    const String& uniqueIdentifier() const LIFETIME_BOUND { return m_uniqueIdentifier; }

    bool isURLForThisExtension(const URL&) const;

    RefPtr<JSON::Object> manifest() const { return m_manifest ? protect(m_manifest)->asObject() : nullptr; }

    double manifestVersion() const { return m_manifestVersion; }
    bool supportsManifestVersion(double version) const { return manifestVersion() >= version; }
    RefPtr<WebExtensionLocalization> localization() const { return m_localization; }

    bool isStorageAllowedInUntrustedContexts(WebExtensionDataType dataType) const { return isStorageTypeAllowedInUntrustedContexts(m_storageAccessLevels, dataType); }

    PAL::SessionID defaultSessionID() const { return m_defaultSessionID; }

    bool NODELETE inTestingMode() const;

    bool hasDOMWrapperWorld(WebExtensionContentWorldType contentWorldType) const { return contentWorldType != WebExtensionContentWorldType::ContentScript || hasContentScriptWorld(); }
    Ref<WebCore::DOMWrapperWorld> toDOMWrapperWorld(WebExtensionContentWorldType) const;

    static WebCore::DOMWrapperWorld& mainWorldSingleton() { return WebCore::mainThreadNormalWorldSingleton(); }

    bool hasContentScriptWorld() const { return !!m_contentScriptWorld; }
#if PLATFORM(HAIKU)
    bool matchesContentScriptWorld(const WebCore::DOMWrapperWorld&) const;
    std::optional<ContentWorldIdentifier> contentScriptWorldIdentifier() const { return m_contentScriptWorldIdentifier; }
#endif
    WebCore::DOMWrapperWorld& contentScriptWorld() const { RELEASE_ASSERT(hasContentScriptWorld()); return *m_contentScriptWorld; }
    void setContentScriptWorld(WebCore::DOMWrapperWorld&);

    void addFrameWithExtensionContent(WebFrame&);
#if PLATFORM(HAIKU)
    void addMenusClickHandlerStore(WebExtensionMenuClickHandlerStore&);
#endif

    void didEncounterScriptError(const String& message, const String& sourceURL, unsigned lineNumber, unsigned columnNumber, WebExtensionContentWorldType);

    std::optional<WebExtensionTabIdentifier> NODELETE tabIdentifier(WebPage&) const;

    RefPtr<WebPage> NODELETE backgroundPage() const;
    void NODELETE setBackgroundPage(WebPage&);

    bool isUnsupportedAPI(const String& propertyPath, const ASCIILiteral& propertyName) const;

    bool hasPermission(const String& permission) const;

#if ENABLE(INSPECTOR_EXTENSIONS)
    void addInspectorPage(WebPage&, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);

    void addInspectorBackgroundPage(WebPage&, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);
    bool NODELETE isInspectorBackgroundPage(WebPage&) const;

    Inspector::ExtensionAppearance inspectorAppearance() const { return m_inspectorAppearance; }
    void setInspectorAppearance(Inspector::ExtensionAppearance appearance) { m_inspectorAppearance = appearance; }
#endif

    Vector<Ref<WebPage>> popupPages(std::optional<WebExtensionTabIdentifier> = std::nullopt, std::optional<WebExtensionWindowIdentifier> = std::nullopt) const;
    void addPopupPage(WebPage&, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);

    Vector<Ref<WebPage>> tabPages(std::optional<WebExtensionTabIdentifier> = std::nullopt, std::optional<WebExtensionWindowIdentifier> = std::nullopt) const;
    void addTabPage(WebPage&, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    void NODELETE setOffscreenPage(WebPage&);
    bool NODELETE isOffscreenPage(WebPage&) const;
#endif

    void enumerateFramesAndNamespaceObjects(NOESCAPE const Function<void(WebFrame&, WebExtensionAPINamespace&)>&, Ref<WebCore::DOMWrapperWorld>&& = mainWorldSingleton());
    void enumerateFramesAndWebPageNamespaceObjects(NOESCAPE const Function<void(WebFrame&, WebExtensionAPIWebPageNamespace&)>&);

    void enumerateNamespaceObjects(NOESCAPE const Function<void(WebExtensionAPINamespace&)>& function)
    {
        enumerateFramesAndNamespaceObjects([&](auto&, auto& namespaceObject) {
            function(namespaceObject);
        });
    }

#if PLATFORM(HAIKU)
    // Blocking listeners that already received an event through the blocking
    // dispatch are skipped when the observation of the same load/phase arrives.
    void noteBlockingWebRequestDeliveryHaiku(uint64_t resourceLoadID, uint8_t phase);
    bool consumeBlockingWebRequestDeliveryHaiku(uint64_t resourceLoadID, uint8_t phase);
    void forgetBlockingWebRequestDeliveriesHaiku(uint64_t resourceLoadID);

#endif
private:
    explicit WebExtensionContextProxy(const WebExtensionContextParameters&);

    static RefPtr<WebExtensionLocalization> parseLocalization(RefPtr<API::Data>, const URL& baseURL);

    // Action
    void dispatchActionClickedEvent(const std::optional<WebExtensionTabParameters>&);

    // Alarms
    void dispatchAlarmsEvent(const WebExtensionAlarmParameters&);

    // Commands
    void dispatchCommandsCommandEvent(const String& identifier, const std::optional<WebExtensionTabParameters>&);
    void dispatchCommandsChangedEvent(const String& identifier, const String& oldShortcut, const String& newShortcut);

    // Cookies
#if PLATFORM(HAIKU) && USE(CURL)
    void dispatchCookiesChangedEvent(PAL::SessionID, const Vector<WebCore::CookieChange>&);
#else
    void dispatchCookiesChangedEvent();
#endif

#if ENABLE(DNR_ON_RULE_MATCHED_DEBUG)
    // DeclarativeNetRequest
    void dispatchOnRuleMatchedDebugEvent(const WebCore::ContentRuleListMatchedRule&);
#endif

#if ENABLE(INSPECTOR_EXTENSIONS)
    // DevTools
    void addInspectorPageIdentifier(WebCore::PageIdentifier, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);
    void addInspectorBackgroundPageIdentifier(WebCore::PageIdentifier, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);
    void dispatchDevToolsExtensionPanelShownEvent(Inspector::ExtensionTabID, WebCore::FrameIdentifier);
    void dispatchDevToolsExtensionPanelHiddenEvent(Inspector::ExtensionTabID);
    void dispatchDevToolsNetworkNavigatedEvent(const URL&);
    void dispatchDevToolsPanelsThemeChangedEvent(Inspector::ExtensionAppearance);
#endif

    // Extension
    void setBackgroundPageIdentifier(WebCore::PageIdentifier);
    void addPopupPageIdentifier(WebCore::PageIdentifier, std::optional<WebExtensionTabIdentifier>, std::optional<WebExtensionWindowIdentifier>);
    void addTabPageIdentifier(WebCore::PageIdentifier, WebExtensionTabIdentifier, std::optional<WebExtensionWindowIdentifier>);

#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    // Offscreen
    void setOffscreenPageIdentifier(WebCore::PageIdentifier);
#endif

    // Menus
#if PLATFORM(HAIKU)
    void dispatchMenusClickedEvent(const WebExtensionMenuItemParameters&, bool wasChecked, const WebExtensionMenuItemContextParameters&, const std::optional<WebExtensionTabParameters>&, const std::optional<WTF::UUID>&);
    void removeMenusClickHandlers(const Vector<WTF::UUID>&);
    void clearMenusClickHandlerStores();
#else
    void dispatchMenusClickedEvent(const WebExtensionMenuItemParameters&, bool wasChecked, const WebExtensionMenuItemContextParameters&, const std::optional<WebExtensionTabParameters>&);
#endif

#if PLATFORM(HAIKU)
    void dispatchPrivacyChanged(const String& setting, const String& detailsJSON);
#endif

#if PLATFORM(HAIKU)
    // Downloads
    void dispatchDownloadsEvent(WebExtensionEventListenerType, const String& argumentJSON);
#endif

    // Permissions
    void updateGrantedPermissions(PermissionsMap&&);
    void dispatchPermissionsEvent(WebExtensionEventListenerType, HashSet<String> permissions, HashSet<String> origins);

    // Port
    void dispatchPortMessageEvent(std::optional<WebPageProxyIdentifier>, WebExtensionPortChannelIdentifier, const String& messageJSON, bool userGesture);
    void dispatchPortDisconnectEvent(WebExtensionPortChannelIdentifier, const std::optional<String>& errorMessage);

    // Runtime
    void internalDispatchRuntimeMessageEvent(WebExtensionContentWorldType, const String& messageJSON, const std::optional<WebExtensionMessageTargetParameters>&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(String&& replyJSON)>&&, bool* hadListener = nullptr);
    void internalDispatchRuntimeConnectEvent(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier, const String& name, const std::optional<WebExtensionMessageTargetParameters>&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(HashCountedSet<WebPageProxyIdentifier>&&)>&&);
    void dispatchRuntimeMessageEvent(WebExtensionContentWorldType, const String& messageJSON, const std::optional<WebExtensionMessageTargetParameters>&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(String&& replyJSON)>&&);
    void dispatchRuntimeConnectEvent(WebExtensionContentWorldType, WebExtensionPortChannelIdentifier, const String& name, const std::optional<WebExtensionMessageTargetParameters>&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(HashCountedSet<WebPageProxyIdentifier>&&)>&&);
    void dispatchRuntimeInstalledEvent(WebExtensionContext::InstallReason, String previousVersion);
    void dispatchRuntimeStartupEvent();
    bool matchesTarget(WebFrame&, const std::optional<WebExtensionMessageTargetParameters>&);

    // Storage
    void setStorageAccessLevel(WebExtensionDataType, WebExtensionStorageAccessLevel);
    void dispatchStorageChangedEvent(const Vector<String>& onChangedJSON, WebExtensionDataType, WebExtensionContentWorldType);

    // Tabs
#if PLATFORM(HAIKU)
    void dispatchTabsMessageEvent(WebExtensionContentWorldType, const String& messageJSON, const WebExtensionMessageTargetParameters&, const WebExtensionMessageSenderParameters&, bool userGesture, CompletionHandler<void(String&& replyJSON, bool hadListener)>&&);
#endif
    void dispatchTabsCreatedEvent(const WebExtensionTabParameters&);
    void dispatchTabsUpdatedEvent(const WebExtensionTabParameters&, const WebExtensionTabParameters& changedParameters);
    void dispatchTabsReplacedEvent(WebExtensionTabIdentifier replacedTabIdentifier, WebExtensionTabIdentifier newTabIdentifier);
    void dispatchTabsDetachedEvent(WebExtensionTabIdentifier, WebExtensionWindowIdentifier oldWindowIdentifier, uint64_t oldIndex);
    void dispatchTabsMovedEvent(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, uint64_t oldIndex, uint64_t newIndex);
    void dispatchTabsAttachedEvent(WebExtensionTabIdentifier, WebExtensionWindowIdentifier newWindowIdentifier, uint64_t newIndex);
    void dispatchTabsActivatedEvent(WebExtensionTabIdentifier previousActiveTabIdentifier, WebExtensionTabIdentifier newActiveTabIdentifier, WebExtensionWindowIdentifier);
    void dispatchTabsHighlightedEvent(const Vector<WebExtensionTabIdentifier>&, WebExtensionWindowIdentifier);
    void dispatchTabsRemovedEvent(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, WebExtensionContext::WindowIsClosing);

    // Test
    void dispatchTestMessageEvent(const String& message, const String& argumentJSON, WebExtensionContentWorldType);
    void dispatchTestStartedEvent(const String& argumentJSON, WebExtensionContentWorldType);
    void dispatchTestFinishedEvent(const String& argumentJSON, WebExtensionContentWorldType);

    // Web Navigation
    void dispatchWebNavigationEvent(WebExtensionEventListenerType, WebExtensionTabIdentifier, const WebExtensionFrameParameters&, WallTime);

    // Web Request
    void resourceLoadDidSendRequest(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const WebCore::ResourceRequest&, const ResourceLoadInfo&, const std::optional<IPC::FormDataReference>&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
    void resourceLoadDidBlockBeforeRequest(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const ResourceLoadInfo&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
    void resourceLoadDidPerformHTTPRedirection(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const WebCore::ResourceResponse&, const ResourceLoadInfo&, const WebCore::ResourceRequest& newRequest, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
    void resourceLoadDidReceiveChallenge(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const WebCore::AuthenticationChallenge&, const ResourceLoadInfo&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
    void resourceLoadDidReceiveResponse(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const WebCore::ResourceResponse&, const ResourceLoadInfo&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
    void resourceLoadDidCompleteWithError(WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const WebCore::ResourceResponse&, const WebCore::ResourceError&, const ResourceLoadInfo&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier);
#if PLATFORM(HAIKU)
    // Blocking webRequest: invokes only blocking listeners and replies with one
    // JSON BlockingResponse per listener (empty for no decision).
    void dispatchBlockingWebRequestHaiku(uint8_t phase, WebExtensionTabIdentifier, WebExtensionWindowIdentifier, const ResourceLoadInfo&, const WebCore::ResourceRequest&, const std::optional<IPC::FormDataReference>&, const std::optional<WebCore::ResourceResponse>&, const std::optional<WebCore::AuthenticationChallenge>&, std::optional<WebCore::FrameIdentifier> mainFrameIdentifier, CompletionHandler<void(Vector<String>&&)>&&);
#endif

    // Windows
    void dispatchWindowsEvent(WebExtensionEventListenerType, const std::optional<WebExtensionWindowParameters>&);

    // IPC::MessageReceiver
    void didReceiveMessage(IPC::Connection&, IPC::Decoder&) override;

    WebExtensionContextIdentifier m_unprivilegedIdentifier;
    Markable<WebExtensionContextIdentifier> m_privilegedIdentifier;
    WeakPtr<WebExtensionControllerProxy> m_extensionControllerProxy;
    URL m_baseURL;
    String m_uniqueIdentifier;
    HashSet<String> m_unsupportedAPIs;
    RefPtr<WebExtensionLocalization> m_localization;
    RefPtr<JSON::Value> m_manifest;
    double m_manifestVersion { 0 };
    WebExtensionStorageAccessLevelMap m_storageAccessLevels;
    PAL::SessionID m_defaultSessionID { PAL::SessionID::defaultSessionID() };
    mutable PermissionsMap m_grantedPermissions;
    mutable WallTime m_nextGrantedPermissionsExpirationDate { WallTime::nan() };
    RefPtr<WebCore::DOMWrapperWorld> m_contentScriptWorld;
#if PLATFORM(HAIKU)
    std::optional<ContentWorldIdentifier> m_contentScriptWorldIdentifier;
#endif
    WeakFrameSet m_extensionContentFrames;
#if PLATFORM(HAIKU)
    WeakHashSet<WebExtensionMenuClickHandlerStore> m_menuClickHandlerStores;
#endif
    WeakPtr<WebPage> m_backgroundPage;
#if PLATFORM(HAIKU)
    Vector<std::pair<uint64_t, uint8_t>> m_blockingWebRequestDeliveriesHaiku;
#endif
#if ENABLE(WK_WEB_EXTENSIONS_OFFSCREEN)
    WeakPtr<WebPage> m_offscreenPage;
#endif
#if ENABLE(INSPECTOR_EXTENSIONS)
    WeakPageTabWindowMap m_inspectorPageMap;
    WeakPageTabWindowMap m_inspectorBackgroundPageMap;
    Inspector::ExtensionAppearance m_inspectorAppearance { Inspector::ExtensionAppearance::Light };
#endif
    WeakPageTabWindowMap m_popupPageMap;
    WeakPageTabWindowMap m_tabPageMap;
};

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS)
