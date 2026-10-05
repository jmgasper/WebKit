include(Headers.cmake)
include(Platform/Curl.cmake)

file(MAKE_DIRECTORY ${DERIVED_SOURCES_HAIKU_API_DIR})

configure_file(UIProcess/API/haiku/WebKitVersion.h.in ${DERIVED_SOURCES_HAIKU_API_DIR}/WebKitVersion.h)

# The port's public header version and the upstream rendering engine version
# are separate. Report the latter from the exact upstream configuration.
set(WebKit_HAIKU_VERSION_CONFIGURATION "${CMAKE_SOURCE_DIR}/Configurations/Version.xcconfig")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${WebKit_HAIKU_VERSION_CONFIGURATION}")
file(READ "${WebKit_HAIKU_VERSION_CONFIGURATION}" WebKit_HAIKU_VERSION_CONTENT)
foreach(WebKit_HAIKU_VERSION_COMPONENT MAJOR MINOR TINY)
    string(REGEX MATCH "(^|\n)${WebKit_HAIKU_VERSION_COMPONENT}_VERSION[ \t]*=[ \t]*([0-9]+);" WebKit_HAIKU_VERSION_MATCH "${WebKit_HAIKU_VERSION_CONTENT}")
    if(NOT WebKit_HAIKU_VERSION_MATCH)
        message(FATAL_ERROR "Missing ${WebKit_HAIKU_VERSION_COMPONENT} engine version in ${WebKit_HAIKU_VERSION_CONFIGURATION}")
    endif()
    set(WebKit_HAIKU_${WebKit_HAIKU_VERSION_COMPONENT}_VERSION "${CMAKE_MATCH_2}")
endforeach()
configure_file(UIProcess/API/haiku/WebKitBuildInfoHaiku.h.in ${DERIVED_SOURCES_HAIKU_API_DIR}/WebKitBuildInfoHaiku.h @ONLY)

list(APPEND WebKit_SOURCES
    NetworkProcess/NetworkDataTaskDataURL.cpp
    NetworkProcess/Classifier/WebResourceLoadStatisticsStore.cpp

    Platform/IPC/unix/ArgumentCodersUnix.cpp
    Platform/IPC/haiku/ConnectionHaiku.cpp
    Platform/IPC/haiku/MessagePacketHaiku.cpp
    Platform/IPC/haiku/IPCEventHaiku.cpp
    Platform/IPC/haiku/IPCSemaphoreHaiku.cpp
    Platform/IPC/unix/IPCUtilitiesUnix.cpp
    Platform/IPC/haiku/SocketMonitorHaiku.cpp
    Platform/classifier/ResourceLoadStatisticsClassifier.cpp
    Platform/haiku/LoggingHaiku.cpp
    Platform/haiku/ModuleHaiku.cpp
    Platform/haiku/ReadWriteLockHaiku.cpp
    Platform/haiku/StoragePathsHaiku.cpp
    Shared/unix/AuxiliaryProcessMain.cpp
    Shared/haiku/ProcessExecutablePathHaiku.cpp
    Shared/haiku/NativeWebEventsHaiku.cpp
    Shared/haiku/WebMemorySamplerHaiku.cpp

    UIProcess/DefaultUndoController.cpp
    UIProcess/API/haiku/WebKitContext.cpp
    UIProcess/API/haiku/WebKitInfo.cpp
    UIProcess/API/haiku/WebKitInspector.cpp
    UIProcess/API/haiku/APIContentRuleListStoreHaiku.cpp
    UIProcess/API/haiku/WebKitView.cpp
    UIProcess/API/haiku/WebKitViewEmbedding.cpp
    UIProcess/Extensions/haiku/WebExtensionArchiveHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionCRXHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionDeclarativeNetRequestURLFilter.cpp
    UIProcess/Extensions/haiku/WebExtensionDeclarativeNetRequestRulesHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextDeclarativeNetRequestHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionPackageSnapshotHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionStagingDirectoryHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextPermissionsHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextActionsHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionMessagePortHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionActionHaiku.cpp
    UIProcess/Extensions/WebExtensionCommand.cpp
    UIProcess/Extensions/haiku/WebExtensionCommandHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextCommandsHaiku.cpp
    UIProcess/Extensions/haiku/ExtensionActionPopupHaiku.cpp
    Shared/Extensions/WebExtensionCommandShortcut.cpp
    Shared/Extensions/WebExtensionCommandSettings.cpp
    Shared/Extensions/WebExtensionPermissionState.cpp
    Shared/Extensions/WebExtensionActionDetails.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPIActionHaiku.cpp
    WebProcess/Extensions/API/WebExtensionAPIAction.cpp
    Shared/Extensions/WebExtensionCookieStoreIdentifier.cpp
    Shared/Extensions/WebExtensionCookieQueryParser.cpp
    Shared/Extensions/WebExtensionCookieWriteParser.cpp
    WebProcess/Extensions/Bindings/JSWebExtensionCookieParameters.cpp
    WebProcess/Extensions/API/WebExtensionAPICookies.cpp
    Shared/Extensions/WebExtensionMenuItemParser.cpp
    Shared/Extensions/WebExtensionMenuItemTree.cpp
    UIProcess/Extensions/WebExtensionMenuItem.cpp
    UIProcess/Extensions/haiku/WebExtensionMenuItemHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPIMenusHaiku.cpp
    WebProcess/Extensions/Bindings/JSWebExtensionMenuItemParameters.cpp
    WebProcess/Extensions/API/WebExtensionAPIMenus.cpp
    WebProcess/Extensions/API/WebExtensionMenuClickHandlerStore.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPICookiesHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionCookieHostAccess.cpp
    UIProcess/Extensions/haiku/WebExtensionActiveTabGrant.cpp
    Shared/Extensions/WebExtensionWindowQueryParser.cpp
    WebProcess/Extensions/Bindings/JSWebExtensionWindowParameters.cpp
    WebProcess/Extensions/API/WebExtensionAPIWindows.cpp
    WebProcess/Extensions/API/WebExtensionAPIWindowsEvent.cpp
    UIProcess/Extensions/haiku/WebExtensionContextWindowsHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPIWindowsHaiku.cpp
    Shared/Extensions/WebExtensionTabQueryParser.cpp
    Shared/Extensions/WebExtensionTabScriptParser.cpp
    Shared/Extensions/WebExtensionTabScriptTarget.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPITabsHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPITabScriptHaiku.cpp
    WebProcess/Extensions/API/WebExtensionAPITabs.cpp
    WebProcess/Extensions/WebExtensionTabScriptExecution.cpp
    WebProcess/Extensions/API/WebExtensionAPIExtension.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPIExtensionHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextAPIPermissionsHaiku.cpp
    WebProcess/Extensions/API/WebExtensionAPIPermissions.cpp
    WebProcess/Extensions/API/WebExtensionAPICommands.cpp
    UIProcess/Extensions/haiku/WebExtensionContextTestHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextBackgroundHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionControllerAPITestHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionControllerConfigurationHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionControllerHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionTabHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionWindowHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextTabsHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionContextWebNavigationHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionStateHaiku.cpp
    UIProcess/Extensions/haiku/WebExtensionURLSchemeHandlerHaiku.cpp
    UIProcess/Launcher/haiku/ProcessLauncherHaiku.cpp
    UIProcess/LegacySessionStateCodingNone.cpp
    UIProcess/WebsiteData/haiku/WebsiteDataStoreHaiku.cpp
    UIProcess/haiku/DownloadRegistryHaiku.cpp
    UIProcess/haiku/ExtensionPermissionPromptHaiku.cpp
    UIProcess/haiku/ExtensionPackageRegistryHaiku.cpp
    UIProcess/haiku/DrawingAreaProxyHaiku.cpp
    UIProcess/haiku/MemoryPressureMonitorHaiku.cpp
    UIProcess/haiku/TextCheckerHaiku.cpp
    UIProcess/haiku/WebPageProxyHaiku.cpp
    UIProcess/haiku/WebPopupMenuProxyHaiku.cpp
    UIProcess/haiku/WebProcessPoolHaiku.cpp
    UIProcess/haiku/WebView.cpp
    UIProcess/haiku/OpenPanelHaiku.cpp
    UIProcess/haiku/BrowserTabRegistryHaiku.cpp
    UIProcess/haiku/BrowserCommandBrokerHaiku.cpp
    UIProcess/haiku/WebViewContextHaiku.cpp
    UIProcess/haiku/SitePermissionsHaiku.cpp

    WebProcess/Extensions/Bindings/JSWebExtensionWrapper.cpp
    WebProcess/InjectedBundle/haiku/InjectedBundleHaiku.cpp
    WebProcess/WebPage/haiku/WebPageHaiku.cpp
    WebProcess/haiku/WebProcessHaiku.cpp
    WebProcess/haiku/WebProcessMainHaiku.cpp
)

if (NOT USE_COORDINATED_GRAPHICS)
    # The port's own drawing area and its compositor. Coordinated graphics
    # replaces both, and GraphicsLayerTextureMapper, which LayerTreeHostHaiku
    # is built on, is not compiled beside GraphicsLayerCoordinated.
    list(APPEND WebKit_SOURCES
        WebProcess/WebPage/haiku/DrawingAreaHaiku.cpp
        WebProcess/WebPage/haiku/LayerTreeHostHaiku.cpp
    )
endif ()

if (USE_COORDINATED_GRAPHICS)
    # RenderProcessInfo.h and RendererBufferFormat.h are plain WTF structs that
    # the coordinated renderers all include, and they sit in Shared/glib beside
    # the GLib argument coders. Nothing else in there is picked up: the rest is
    # named for GLib.
    list(APPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBKIT_DIR}/Shared/glib"
    )

    # The coordinated drawing area paints the page into tiles, which
    # SkiaPaintingEngine can paint on a worker pool, and composites them through
    # TextureMapper. DrawingAreaCoordinatedGraphics is the plain one, as used by
    # the Windows and PlayStation ports; GTK and WPE use a GLib variant instead.
    list(APPEND WebKit_SOURCES
        UIProcess/skia/BackingStoreSkia.cpp

        UIProcess/CoordinatedGraphics/DrawingAreaProxyCoordinatedGraphics.cpp
        UIProcess/haiku/AcceleratedBackingStore.cpp

        WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface.cpp
        WebProcess/WebPage/CoordinatedGraphics/CompositingRunLoop.cpp
        WebProcess/WebPage/CoordinatedGraphics/CoordinatedSceneState.cpp
        WebProcess/WebPage/CoordinatedGraphics/DrawingAreaCoordinatedGraphicsGLib.cpp
        WebProcess/WebPage/CoordinatedGraphics/FrameRenderer.cpp
        WebProcess/WebPage/CoordinatedGraphics/LayerTreeHost.cpp
        WebProcess/WebPage/CoordinatedGraphics/NonCompositedFrameRenderer.cpp
        WebProcess/WebPage/CoordinatedGraphics/ScrollbarsControllerCoordinated.cpp
        WebProcess/WebPage/CoordinatedGraphics/ScrollingCoordinatorCoordinated.cpp
        WebProcess/WebPage/CoordinatedGraphics/ThreadedCompositor.cpp
    )

    list(APPEND WebKit_MESSAGES_IN_FILES
        UIProcess/glib/AcceleratedBackingStore
        WebProcess/WebPage/CoordinatedGraphics/AcceleratedSurface
    )
endif ()

# Media capture runs in the web process, as on GTK and WPE: the UI process
# asks it for the devices and to check a request's constraints
# (UserMediaCaptureManager, the GLib files, which are portable C++).
if (ENABLE_MEDIA_STREAM)
    list(APPEND WebKit_SOURCES
        UIProcess/glib/UserMediaPermissionRequestManagerProxyGLib.cpp
        WebProcess/glib/UserMediaCaptureManager.cpp
    )
    list(APPEND WebKit_MESSAGES_IN_FILES
        WebProcess/glib/UserMediaCaptureManager
    )
    list(APPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBKIT_DIR}/WebProcess/glib"
    )
endif ()

# WebRTC: libwebrtc in the web process; its sockets live in the network
# process (NetworkRTCProvider), as on GTK and WPE.
if (USE_LIBWEBRTC)
    list(APPEND WebKit_SOURCES
        NetworkProcess/webrtc/LibWebRTCSocketClient.cpp
        NetworkProcess/webrtc/NetworkRTCMonitor.cpp
        NetworkProcess/webrtc/NetworkRTCProvider.cpp
        NetworkProcess/webrtc/NetworkRTCSharedMonitor.cpp
    )
    list(APPEND WebKit_SYSTEM_INCLUDE_DIRECTORIES
        "${THIRDPARTY_DIR}/libwebrtc/Source/"
        "${THIRDPARTY_DIR}/libwebrtc/Source/webrtc"
        "${THIRDPARTY_DIR}/libwebrtc/Source/third_party/abseil-cpp"
    )
endif ()

if (ENABLE_WEB_AUTHN)
    # This port answers Web Authentication itself. The authenticators upstream
    # builds (Touch ID, USB HID and NFC security keys, and their mocks) need
    # platform device access that KunanyiOS does not provide, and their manager
    # and panel go with them. The WebProcess side is portable and stays.
    list(APPEND WebKit_UNIFIED_SOURCE_EXCLUDES
        "UIProcess/WebAuthentication/WebAuthenticatorCoordinatorProxy\\.cpp"
        "UIProcess/WebAuthentication/WebAuthenticationRequestData\\.cpp"
        "UIProcess/WebAuthentication/AuthenticatorTransportService\\.cpp"
        "UIProcess/WebAuthentication/Authenticator\\.cpp"
        "UIProcess/WebAuthentication/RelatedOriginsValidator\\.cpp"
        "UIProcess/WebAuthentication/fido/"
        "UIProcess/WebAuthentication/Mock/"
        "UIProcess/API/APIWebAuthenticationPanel\\.cpp"
        "UIProcess/API/APIWebAuthenticationAssertionResponse\\.cpp"
    )
    list(APPEND WebKit_SOURCES
        UIProcess/WebAuthentication/haiku/WebAuthenticatorCoordinatorProxyHaiku.cpp
        WebProcess/DigitalCredentials/haiku/DigitalCredentialsRequestValidatorHaiku.cpp
    )
    list(APPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBKIT_DIR}/UIProcess/WebAuthentication"
        "${WEBKIT_DIR}/UIProcess/WebAuthentication/haiku"
        "${WEBKIT_DIR}/WebProcess/WebAuthentication"
        "${WEBKIT_DIR}/WebProcess/DigitalCredentials"
        "${WEBKIT_DIR}/WebProcess/cocoa/IdentityDocumentServices"
    )
    list(APPEND WebKit_MESSAGES_IN_FILES
        UIProcess/WebAuthentication/WebAuthenticatorCoordinatorProxy
    )
endif ()

list(APPEND WebKit_PRIVATE_INCLUDE_DIRECTORIES
    "${DERIVED_SOURCES_HAIKU_API_DIR}"
    "${WEBKIT_DIR}/NetworkProcess/unix"
    "${WEBKIT_DIR}/Platform"
    "${WEBKIT_DIR}/Platform/IPC/unix"
    "${WEBKIT_DIR}/Platform/IPC/haiku"
    "${WEBKIT_DIR}/Platform/haiku"
    "${WEBKIT_DIR}/Platform/generic"
    "${WEBKIT_DIR}/Platform/classifier"
    "${WEBKIT_DIR}/Shared/API/c/haiku"
    "${WEBKIT_DIR}/Shared/CoordinatedGraphics"
    "${WEBKIT_DIR}/Shared/unix"
    "${WEBKIT_DIR}/Shared/haiku"
    "${WEBKIT_DIR}/UIProcess/API/C/CoordinatedGraphics"
    "${WEBKIT_DIR}/UIProcess/API/C/haiku"
    "${WEBKIT_DIR}/UIProcess/API/haiku"
    "${WEBKIT_DIR}/UIProcess/haiku"
    "${WEBKIT_DIR}/UIProcess/CoordinatedGraphics"
    "${WEBKIT_DIR}/WebProcess/unix"
    "${WEBKIT_DIR}/WebProcess/WebCoreSupport/haiku"
    "${WEBKIT_DIR}/WebProcess/WebPage/CoordinatedGraphics"
    "${WEBKIT_DIR}/WebProcess/WebPage/haiku"
    ${LIBXML2_INCLUDE_DIR}
    ${LIBXSLT_INCLUDE_DIRS}
    ${SQLITE_INCLUDE_DIRS}
    ${WTF_DIR}
    "${DERIVED_SOURCES_WEBCORE_DIR}"
)

list(APPEND WebKit_LIBRARIES
    ${CMAKE_DL_LIBS}
    # BFilePanel, for <input type=file> (OpenPanelHaiku.cpp).
    tracker
    ${FONTCONFIG_LIBRARIES}
    ${FREETYPE2_LIBRARIES}
    ${JPEG_LIBRARIES}
    ${LIBXML2_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${PNG_LIBRARIES}
    ${SQLITE_LIBRARIES}
)

if (USE_TEXTURE_MAPPER)
    # LayerTreeHostHaiku.cpp calls OpenGL ES 2 directly.
    list(APPEND WebKit_SYSTEM_INCLUDE_DIRECTORIES
        ${EGL_INCLUDE_DIRS}
        ${OpenGLES2_INCLUDE_DIRS}
    )
    list(APPEND WebKit_LIBRARIES
        ${EGL_LIBRARIES}
        OpenGL::GLES
    )
endif ()

# Server certificates are identified by their SHA-256 and described from their
# DER data when the user is asked whether to trust one (WebView.cpp).
list(APPEND WebKit_LIBRARIES OpenSSL::Crypto)

if (ENABLE_WK_WEB_EXTENSIONS)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBZIP REQUIRED IMPORTED_TARGET libzip>=1.11)
    list(APPEND WebKit_LIBRARIES PkgConfig::LIBZIP OpenSSL::Crypto)
endif ()

if (USE_SKIA)
    # Brings the Skia coders, including the one for FontPlatformSerializedData,
    # which is a different struct under Skia than the Haiku one below.
    include(Platform/Skia.cmake)
else ()
    list(APPEND WebKit_SERIALIZATION_IN_FILES
        Shared/haiku/WebCoreFontHaiku.serialization.in
    )
endif ()

list(APPEND WebProcess_SOURCES
    WebProcess/EntryPoint/unix/WebProcessMain.cpp
)

find_program(HAIKU_RESOURCE_COMPILER rc REQUIRED)
find_program(HAIKU_RESOURCE_LINKER xres REQUIRED)
set_property(TARGET WebProcess APPEND PROPERTY LINK_DEPENDS "${WEBKIT_DIR}/WebProcess/haiku/WebProcess.rdef")
add_custom_command(TARGET WebProcess POST_BUILD
    COMMAND ${HAIKU_RESOURCE_COMPILER} -o "$<TARGET_FILE_DIR:WebProcess>/WebProcess.rsrc" "${WEBKIT_DIR}/WebProcess/haiku/WebProcess.rdef"
    COMMAND ${HAIKU_RESOURCE_LINKER} -o "$<TARGET_FILE:WebProcess>" "$<TARGET_FILE_DIR:WebProcess>/WebProcess.rsrc"
    VERBATIM
)

list(APPEND NetworkProcess_SOURCES
    NetworkProcess/EntryPoint/unix/NetworkProcessMain.cpp
)

list(APPEND WebProcess_LIBRARIES
    ${LIBXML2_LIBRARIES}
    ${LIBXSLT_LIBRARIES}
    ${OPENGL_LIBRARIES}
    ${SQLITE_LIBRARIES}
)

# Haiku's runtime_loader binds every symbol when it loads an image and looks
# each one up in the images breadth-first, in load order, the executable's
# own list first. libbe, libstdc++ and libroot (SysV hash tables only) came
# last and look up most of their own symbols, and nearly every library looks
# up symbols in libroot or libstdc++: right after libWebKit they are searched
# first. A model of the lookups halves the work of a web process start
# (tools/pi/lookup-scenarios.py); none of these libraries defines a symbol
# another one in the list does, so nothing binds differently.
list(INSERT WebProcess_LIBRARIES 1 JavaScriptCore be stdc++ root)
list(INSERT NetworkProcess_LIBRARIES 1 JavaScriptCore be stdc++ root)

# CMake drops the JavaScriptCore above for the one libWebKit brings in, at
# the end of the line, so it was loaded after a dozen libraries. libWebKit
# looks up 49,000 symbols in it (the method tables of its JS wrappers), each
# one through libbe, libstdc++ and libroot's SysV tables first: more than half
# of a web process's symbol lookup work. Link options come before the objects
# and libraries, so these two are loaded first.
cmake_language(DEFER CALL target_link_options WebProcess BEFORE PRIVATE
    $<TARGET_LINKER_FILE:WebKit> $<TARGET_LINKER_FILE:JavaScriptCore>)
cmake_language(DEFER CALL target_link_options NetworkProcess BEFORE PRIVATE
    $<TARGET_LINKER_FILE:WebKit> $<TARGET_LINKER_FILE:JavaScriptCore>)

# libWebKit also binds its own data symbols when it is linked (after the
# -Bsymbolic-functions of OptionsHaiku.cmake, so this one wins): the 43,000
# lookups of its own type_info objects, vtables and statics at every load
# become relative relocations (111k symbol relocations -> 69k), and a bare web
# process loads in 111 ms instead of 136 on the X399. The binding changes
# only where an image searched before libWebKit defines the same symbol, and
# libWebKit comes right after the executable in all three processes; the
# executables define only Haiku's ABI markers, the WebCore::Headroom
# constants and some libstdc++ shared_ptr type_info objects, none of which
# depends on being one object.
cmake_language(DEFER CALL target_link_options WebKit PRIVATE -Wl,-Bsymbolic)

add_custom_target(forwarding-headerHaiku
    COMMAND ${PERL_EXECUTABLE} ${WEBKIT_DIR}/Scripts/generate-forwarding-headers.pl ${WEBKIT_DIR} ${DERIVED_SOURCES_WEBKIT_DIR}/include haiku
    COMMAND ${PERL_EXECUTABLE} ${WEBKIT_DIR}/Scripts/generate-forwarding-headers.pl ${WEBKIT_DIR} ${DERIVED_SOURCES_WEBKIT_DIR}/include CoordinatedGraphics
)

set(WEBKIT_EXTRA_DEPENDENCIES
    forwarding-headerHaiku
)

add_definitions(
    -DLIBEXECDIR=\"${EXEC_INSTALL_DIR}\"
    -DWEBPROCESSNAME=\"WebProcess\"
    -DPLUGINPROCESSNAME=\"PluginProcess\"
    -DNETWORKPROCESSNAME=\"NetworkProcess\"
)

if (ENABLE_WK_WEB_EXTENSIONS)
    list(APPEND WebKit_CPP_BINDINGS_IN_FILES
        WebProcess/Extensions/Interfaces/WebExtensionAPIDeclarativeNetRequest
        WebProcess/Extensions/Interfaces/WebExtensionAPIDownloads
        WebProcess/Extensions/Interfaces/WebExtensionAPIIdle
        WebProcess/Extensions/Interfaces/WebExtensionAPIManagement
        WebProcess/Extensions/Interfaces/WebExtensionAPINotificationsHaiku
        WebProcess/Extensions/Interfaces/WebExtensionAPIOffscreen
        WebProcess/Extensions/Interfaces/WebExtensionAPIPrivacy
        WebProcess/Extensions/Interfaces/WebExtensionAPIPrivacyCategory
        WebProcess/Extensions/Interfaces/WebExtensionAPIPrivacySetting
        WebProcess/Extensions/Interfaces/WebExtensionAPIScripting
    )
    # The scripting and declarativeNetRequest namespaces use the C++ bindings here, not the Objective-C ones.
    list(REMOVE_ITEM WebKit_BINDINGS_IN_FILES
        WebProcess/Extensions/Interfaces/WebExtensionAPIDeclarativeNetRequest
        WebProcess/Extensions/Interfaces/WebExtensionAPIOffscreen
        WebProcess/Extensions/Interfaces/WebExtensionAPIScripting
    )
    list(APPEND WebKit_SOURCES
        UIProcess/Extensions/haiku/WebExtensionContextAPIDeclarativeNetRequestHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPIDownloadsHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionDownloadsHaiku.cpp
        WebProcess/Extensions/API/WebExtensionAPIDeclarativeNetRequest.cpp
        WebProcess/Extensions/API/WebExtensionAPIDownloads.cpp
        WebProcess/Extensions/API/WebExtensionAPIIdle.cpp
        WebProcess/Extensions/API/WebExtensionAPIManagement.cpp
        WebProcess/Extensions/API/WebExtensionAPINotificationsHaiku.cpp
        WebProcess/Extensions/API/WebExtensionAPIOffscreen.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPIOffscreenHaiku.cpp
        WebProcess/Extensions/API/WebExtensionAPIPrivacy.cpp
        Shared/Extensions/WebExtensionPrivacySettingsHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPIPrivacyHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionPrivacyControllerHaiku.cpp
        Shared/Extensions/WebExtensionTabCommandParser.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPINativeMessagingHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPIScriptingHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextAPITabCommandsHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextBrowserCommandsHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionTabCommandsHaiku.cpp
        WebProcess/Extensions/API/WebExtensionAPIScripting.cpp
    )
endif ()

set(WebKit_FORWARDING_HEADERS_DIRECTORIES
   Shared/API/c
   Shared/API/c/haiku
   UIProcess/API/C
   Platform/IPC/unix
   WebProcess/InjectedBundle/API/c
)

list(APPEND WebKit_PUBLIC_FRAMEWORK_HEADERS
    Shared/API/c/haiku/WKBaseHaiku.h
    UIProcess/API/haiku/WebKitContext.h
    UIProcess/API/haiku/WebKitEmbedding.h
    UIProcess/API/haiku/WebKitExtensionPermission.h
    UIProcess/API/haiku/WebKitInfo.h
    UIProcess/API/haiku/WebKitInspector.h
    UIProcess/API/haiku/WebKitView.h
)

# Keep private native lock/TLS ABI headers confined to the compatibility unit.
# Native MutexLocker collides with the WTF alias in the WebKit precompiled header.
set_source_files_properties(Platform/haiku/ReadWriteLockHaiku.cpp PROPERTIES
    SKIP_PRECOMPILE_HEADERS ON
    INCLUDE_DIRECTORIES "/system/develop/headers/private/shared;/system/develop/headers/private/libroot;/system/develop/headers/private/system"
)

# Blocking webRequest (onBeforeRequest/onBeforeSendHeaders/onHeadersReceived/onAuthRequired).
if (ENABLE_WK_WEB_EXTENSIONS)
    list(APPEND WebKit_SOURCES
        Shared/Extensions/WebExtensionWebRequestDecisionHaiku.cpp
        Shared/Extensions/WebExtensionWebRequestMergeHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionContextWebRequestBlockingHaiku.cpp
        UIProcess/Extensions/haiku/WebExtensionControllerWebRequestBlockingHaiku.cpp
    )
    list(APPEND WebKit_SERIALIZATION_IN_FILES
        Shared/Extensions/WebExtensionWebRequestGateResultHaiku.serialization.in
    )
endif ()
