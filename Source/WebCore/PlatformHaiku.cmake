include(platform/Haiku.cmake)
include(platform/ImageDecoders.cmake)
include(platform/OpenSSL.cmake)

find_package(SQLite3 REQUIRED)
if (NOT TARGET SQLite3::SQLite3) # CMake < 4.3
    add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
endif ()

# Enable all GNU and BSD extensions
add_definitions(-D_DEFAULT_SOURCE)

if(USE_CURL)
  include(platform/Curl.cmake)
  list(APPEND WebCore_SOURCES
    platform/network/haiku/CookieStorageSessionHaikuCurl.cpp
    platform/network/haiku/CurlSSLHandleHaiku.cpp
    platform/network/curl/CurlCacheEntry.cpp
    platform/network/curl/CurlCacheManager.cpp
  )
else()
  list(APPEND WebCore_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/network/haiku"
  )

  list(APPEND WebCore_SOURCES
    platform/network/haiku/BUrlProtocolHandler.cpp
    platform/network/haiku/CertificateInfo.cpp
    platform/network/haiku/CookieJarHaiku.cpp
    platform/network/haiku/DNSHaiku.cpp
    platform/network/haiku/HaikuFormDataStream.cpp
    platform/network/haiku/ResourceHandleHaiku.cpp
    platform/network/haiku/ResourceRequestHaiku.cpp

    platform/network/haiku/CredentialStorageHaiku.cpp
    platform/network/haiku/NetworkStorageSessionHaiku.cpp
  )

  list(APPEND WebCore_LIBRARIES netservices shared)
  list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/network/haiku/AuthenticationChallenge.h
    platform/network/haiku/CertificateInfo.h
    platform/network/haiku/HaikuFormDataStream.h
    platform/network/haiku/ResourceError.h
    platform/network/haiku/ResourceRequest.h
    platform/network/haiku/ResourceResponse.h
  )

endif()

list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/haiku/RenderingStallHaiku.h
)

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    ${WEBCORE_DIR}/platform/video-codecs
    /system/develop/headers/private/netservices
)

if (ENABLE_WEB_AUTHN)
  # The FIDO response converter needs these DER helpers. They are plain C++
  # despite living beside the Cocoa crypto implementation, and nothing else in
  # that directory is built here.
  list(APPEND WebCore_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/crypto/cocoa"
  )
  list(APPEND WebCore_SOURCES
    crypto/cocoa/CommonCryptoDERUtilities.cpp
    # Listed only for CMake+Cocoa upstream; both are portable C++ and hold the
    # extension-output conversions the JS bindings call.
    Modules/webauthn/AuthenticationExtensionsClientInputs.cpp
    Modules/webauthn/AuthenticationExtensionsClientOutputs.cpp
  )
endif ()

list(APPEND WebCore_INCLUDE_DIRECTORIES
  "${THIRDPARTY_DIR}/ANGLE/"
  "${THIRDPARTY_DIR}/ANGLE/include/KHR"
  "${WEBCORE_DIR}/page/scrolling/coordinatedgraphics"
  "${WEBCORE_DIR}/platform/glib"
  "${WEBCORE_DIR}/platform/haiku"
  "${WEBCORE_DIR}/platform/graphics/egl"
  "${WEBCORE_DIR}/platform/graphics/opengl"
  "${WEBCORE_DIR}/platform/graphics/opentype"
  "${WEBCORE_DIR}/platform/graphics/texmap/coordinated"
  "${WEBCORE_DIR}/platform/mediacapabilities"
  "${FORWARDING_HEADERS_DIR}/JavaScriptCore"
  "${CMAKE_SOURCE_DIR}/Source"
)

list(APPEND WebCore_SOURCES
  accessibility/playstation/AXObjectCachePlayStation.cpp
  accessibility/playstation/AccessibilityObjectPlayStation.cpp

  bindings/js/CachedScriptSourceProviderHaiku.cpp

  editing/glib/EditorGLib.cpp

  page/haiku/DragControllerHaiku.cpp
  page/haiku/EventHandlerHaiku.cpp

  page/PointerLockController.cpp

  platform/Cursor.cpp

  platform/haiku/CursorHaiku.cpp
  platform/haiku/DragImageHaiku.cpp
  platform/haiku/DragDataHaiku.cpp
  platform/haiku/ErrorsHaiku.cpp
  platform/haiku/FileSystemHaiku.cpp
  platform/haiku/KeyedDecoderHaiku.cpp
  platform/haiku/KeyedEncoderHaiku.cpp
  platform/haiku/KURLHaiku.cpp
  platform/haiku/LoggingHaiku.cpp
  platform/haiku/MIMETypeRegistryHaiku.cpp
  platform/haiku/MainThreadSharedTimerHaiku.cpp
  platform/haiku/PasteboardHaiku.cpp
  platform/haiku/PlatformKeyboardEventHaiku.cpp
  platform/haiku/PlatformMouseEventHaiku.cpp
  platform/haiku/PlatformScreenHaiku.cpp
  platform/haiku/PopupMenuHaiku.cpp
  platform/haiku/SearchPopupMenuHaiku.cpp
  platform/haiku/SharedMemoryHaiku.cpp
  platform/haiku/SharedTimerHaiku.cpp
  platform/haiku/SoundHaiku.cpp
  platform/haiku/TemporaryLinkStubs.cpp
  platform/haiku/UserAgentHaiku.cpp
  platform/haiku/WidgetHaiku.cpp


  platform/graphics/PlatformDisplay.cpp
  platform/graphics/WOFFFileFormat.cpp

  platform/graphics/haiku/AffineTransformHaiku.cpp
  platform/graphics/haiku/ColorHaiku.cpp
  platform/graphics/haiku/SystemFontDatabaseHaiku.cpp
  platform/graphics/haiku/FloatPointHaiku.cpp
  platform/graphics/haiku/FloatRectHaiku.cpp
  platform/graphics/haiku/FloatSizeHaiku.cpp
  platform/graphics/haiku/IntPointHaiku.cpp
  platform/graphics/haiku/IntRectHaiku.cpp
  platform/graphics/haiku/IntSizeHaiku.cpp
  platform/graphics/haiku/IconHaiku.cpp
  platform/graphics/haiku/FragmentedMP4ParserHaiku.cpp
  platform/graphics/haiku/MediaDecoderSelectionHaiku.cpp
  platform/graphics/haiku/MediaPlayerPrivateHaiku.cpp
  platform/graphics/haiku/MediaPlayerPrivateHaikuMSE.cpp
  platform/graphics/haiku/PlatformMediaEngineConfigurationFactoryHaiku.cpp
)

if (USE_SKIA)
  # Skia brings its own graphics context, fonts, paths, gradients, images and
  # image buffers; see platform/SourcesSkia.txt. Only the Haiku pieces it has no
  # equivalent for stay above.
  include(platform/Skia.cmake)
  # The Haiku themes draw with BControlLook, which needs a BView to draw into.
  # Adwaita is the theme the other Skia ports use and paints through
  # GraphicsContext alone, so it works whatever is behind the context.
  include(platform/Adwaita.cmake)
else ()
  list(APPEND WebCore_SOURCES
  platform/graphics/haiku/BitmapImageHaiku.cpp
  platform/graphics/haiku/ComplexTextControllerHaiku.cpp
  platform/graphics/haiku/FontCacheHaiku.cpp
  platform/graphics/haiku/FontCustomPlatformData.cpp
  platform/graphics/opentype/OpenTypeUtilities.cpp
  platform/graphics/haiku/FontDescriptionHaiku.cpp
  platform/graphics/haiku/FontPlatformDataHaiku.cpp
  platform/graphics/haiku/FontHaiku.cpp
  platform/graphics/haiku/GlyphPageTreeNodeHaiku.cpp
  platform/graphics/haiku/GradientHaiku.cpp
  platform/graphics/haiku/GraphicsContextHaiku.cpp
  platform/graphics/haiku/ImageBufferHaiku.cpp
  platform/graphics/haiku/ImageHaiku.cpp
  platform/graphics/haiku/NativeImageHaiku.cpp
  platform/graphics/haiku/PathHaiku.cpp
  platform/graphics/haiku/ShareableBitmapHaiku.cpp
  platform/graphics/haiku/SimpleFontDataHaiku.cpp
  platform/graphics/haiku/TileHaiku.cpp
  platform/graphics/haiku/TiledBackingStoreHaiku.cpp

  platform/haiku/ScrollbarThemeHaiku.cpp
  platform/haiku/ThemeHaiku.cpp
  rendering/haiku/RenderThemeHaiku.cpp

  platform/image-decoders/haiku/ImageDecoderHaiku.cpp
  )
endif ()

list(APPEND WebCore_SOURCES
  platform/haiku/RenderingStallHaiku.cpp

  platform/network/haiku/NetworkStateNotifierHaiku.cpp

  platform/mock/GeolocationClientMock.cpp

  platform/text/Hyphenation.cpp
  platform/text/LocaleICU.cpp

  platform/text/haiku/StringHaiku.cpp

  platform/unix/LoggingUnix.cpp
  platform/unix/SharedMemoryUnix.cpp


  inspector/LegacyWebSocketInspectorInstrumentation.cpp
)

if (ENABLE_WEB_AUDIO)
    list(APPEND WebCore_SOURCES
        platform/audio/haiku/AudioBusHaiku.cpp
        platform/audio/haiku/AudioDestinationHaiku.cpp
        platform/audio/haiku/AudioFileReaderHaiku.cpp
    )
endif ()

# PannerNode's HRTF impulse responses, built into the library (AudioBusHaiku.cpp).
if (ENABLE_WEB_AUDIO)
    set(HAIKU_HRTF_HEADER "${WebCore_DERIVED_SOURCES_DIR}/HaikuHRTFComposite.h")
    set(HAIKU_HRTF_WAV "${WEBCORE_DIR}/platform/audio/resources/Composite.wav")
    if (NOT EXISTS "${HAIKU_HRTF_HEADER}" OR "${HAIKU_HRTF_WAV}" IS_NEWER_THAN "${HAIKU_HRTF_HEADER}")
        file(READ "${HAIKU_HRTF_WAV}" _haiku_hrtf HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _haiku_hrtf "${_haiku_hrtf}")
        file(WRITE "${HAIKU_HRTF_HEADER}" "static const unsigned char hrtfCompositeWav[] = {${_haiku_hrtf}};\n")
    endif ()
endif ()

# Web Audio's FFTs (analysers, convolution, HRTF panning, periodic waves) use
# PFFFT from the libwebrtc tree; without it the stub asserts on every call.
if (ENABLE_WEB_AUDIO AND USE_LIBWEBRTC)
    list(APPEND WebCore_SOURCES platform/audio/haiku/FFTFrameHaiku.cpp)
else ()
    list(APPEND WebCore_SOURCES platform/audio/FFTFrameStub.cpp)
endif ()

list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
    "${WEBCORE_DIR}/platform/audio/haiku"
)

if (ENABLE_VIDEO)
    list(APPEND WebCore_SOURCES
        platform/graphics/haiku/VideoFrameHaiku.cpp
    )
endif ()

# Camera, microphone and screen capture through the Media Kit and app_server,
# and the player for MediaStreams.
if (ENABLE_MEDIA_STREAM)
    list(APPEND WebCore_SOURCES
        platform/graphics/haiku/MediaPlayerPrivateMediaStreamHaiku.cpp
        platform/mediastream/haiku/AudioMediaStreamTrackRendererHaiku.cpp
        platform/mediastream/haiku/HaikuCaptureNode.cpp
        platform/mediastream/haiku/HaikuCaptureSources.cpp
        platform/mediastream/haiku/MediaStreamTrackAudioSourceProviderHaiku.cpp
    )
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/mediastream/haiku"
    )
    # WebProcess frees the devices in use before it exits.
    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/mediastream/haiku/HaikuCaptureNode.h
    )
endif ()

# WebRTC through libwebrtc (Source/ThirdParty/libwebrtc, built for Haiku by
# its CMakeLists.txt), with libyuv for picture conversions, and libvpx, Opus
# and libwebm from the same tree for MediaRecorder.
if (USE_LIBWEBRTC)
    list(APPEND WebCore_SOURCES
        platform/mediastream/haiku/HaikuVoiceProcessing.cpp
        platform/mediastream/libwebrtc/haiku/LibWebRTCH264DecoderHaiku.cpp
        platform/mediastream/libwebrtc/haiku/LibWebRTCHaiku.cpp
    )
    if (ENABLE_MEDIA_RECORDER)
        list(APPEND WebCore_SOURCES
            platform/mediarecorder/MediaRecorderPrivateHaiku.cpp
        )
    endif ()
    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/mediastream/libwebrtc/haiku"
    )
    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/mediastream/libwebrtc/haiku/LibWebRTCProviderHaiku.h
    )
    list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES
        "${THIRDPARTY_DIR}/libwebrtc/Source/third_party/libyuv/include"
        "${THIRDPARTY_DIR}/libwebrtc/Source/third_party/libwebm"
        "${THIRDPARTY_DIR}/libwebrtc/Source/third_party/opus/src/include"
        "${THIRDPARTY_DIR}/libwebrtc/Source/third_party/pffft/src"
    )
endif ()

# Adwaita.cmake already lists this one, and listing it twice concatenates the
# stylesheet into the user agent sheet twice.
if (NOT USE_SKIA)
  list(APPEND WebCore_USER_AGENT_STYLE_SHEETS
      ${WebCore_DERIVED_SOURCES_DIR}/ModernMediaControls.css
  )
endif ()

if (WTF_USE_COORDINATED_GRAPHICS)
    list(APPEND WebCore_SOURCES
        platform/graphics/texmap/coordinated/CoordinatedTile.cpp
    )
else()

endif()

set(WebCore_USER_AGENT_SCRIPTS
    ${WebCore_DERIVED_SOURCES_DIR}/ModernMediaControls.js
)

if (NOT USE_SKIA)
  set(WebCore_USER_AGENT_SCRIPTS_DEPENDENCIES ${WEBCORE_DIR}/rendering/haiku/RenderThemeHaiku.cpp)
endif ()

list(APPEND WebCore_LIBRARIES
  be
  #bnetapi
  bsd
  execinfo
  #${LIBGCRYPT_LIBRARIES}
  ${JPEG_LIBRARY}
  network
  ${PNG_LIBRARY}
  psl
  shared
  #${LIBTASN1_LIBRARIES}
  textencoding
  translation
  ${WEBP_LIBRARIES}
  ${LIBXSLT_LIBRARIES}
)

list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES
  ${GNUTLS_INCLUDE_DIRS}
  ${ICU_INCLUDE_DIRS}
  ${LIBXML2_INCLUDE_DIR}
  ${LIBXSLT_INCLUDE_DIR}
  ${SQLITE_INCLUDE_DIR}
  ${WEBP_INCLUDE_DIRS}
  ${ZLIB_INCLUDE_DIRS}
  #/system/develop/headers/private/netservices
)

if (ENABLE_WEB_AUDIO)
    #    list(APPEND WebCore_INCLUDE_DIRECTORIES
    #    "${WEBCORE_DIR}/platform/graphics/gstreamer"
    #)

    list(APPEND WebCore_LIBRARIES
        media
    )
endif ()

if (ENABLE_VIDEO)
    list(APPEND WebCore_LIBRARIES
        media
    )
endif ()

if (WTF_USE_3D_GRAPHICS)
    list(APPEND WebCore_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/graphics/opengl"
        "${WEBCORE_DIR}/platform/graphics/surfaces"
        "${WEBCORE_DIR}/platform/graphics/surfaces/glx"
        "${WEBCORE_DIR}/platform/graphics/texmap"
    )

    if (WTF_USE_EGL)
        list(APPEND WebCore_INCLUDE_DIRECTORIES
            ${EGL_INCLUDE_DIR}
            "${WEBCORE_DIR}/platform/graphics/surfaces/egl"
    )
    endif ()

    list(APPEND WebCore_SOURCES
        platform/graphics/opengl/Extensions3DOpenGLCommon.cpp
        platform/graphics/opengl/GLPlatformContext.cpp
        platform/graphics/opengl/GLPlatformSurface.cpp
        platform/graphics/opengl/GraphicsContext3DOpenGLCommon.cpp
        platform/graphics/opengl/TemporaryOpenGLSetting.cpp

        platform/graphics/surfaces/GLTransportSurface.cpp
        platform/graphics/surfaces/GraphicsSurface.cpp
    )

    if (WTF_USE_EGL)
        list(APPEND WebCore_SOURCES
            platform/graphics/surfaces/egl/EGLConfigSelector.cpp
            platform/graphics/surfaces/egl/EGLContext.cpp
            platform/graphics/surfaces/egl/EGLHelper.cpp
            platform/graphics/surfaces/egl/EGLSurface.cpp
            platform/graphics/surfaces/egl/EGLXSurface.cpp
        )
    else ()
        list(APPEND WebCore_SOURCES
            platform/graphics/surfaces/glx/GLXContext.cpp
            platform/graphics/surfaces/glx/GLXSurface.cpp
        )
    endif ()

    if (WTF_USE_OPENGL_ES_2)
        list(APPEND WebCore_SOURCES
            platform/graphics/opengl/Extensions3DOpenGLES.cpp
            platform/graphics/opengl/GraphicsContext3DOpenGLES.cpp
        )
    else ()
        list(APPEND WebCore_SOURCES
            platform/graphics/opengl/Extensions3DOpenGL.cpp
            platform/graphics/opengl/GraphicsContext3DOpenGL.cpp
        )
    endif ()

    if (WTF_USE_EGL)
        list(APPEND WebCore_LIBRARIES
            ${EGL_LIBRARY}
        )
    endif ()
endif ()

if (ENABLE_WEB_AUDIO)
    #list(APPEND WebCore_INCLUDE_DIRECTORIES
    #    "${WEBCORE_DIR}/platform/audio/gstreamer"
    #)
    set(WEB_AUDIO_DIR ${CMAKE_INSTALL_PREFIX}/${DATA_INSTALL_DIR}/webaudio/resources)
    file(GLOB WEB_AUDIO_DATA "${WEBCORE_DIR}/platform/audio/resources/*.wav")
    install(FILES ${WEB_AUDIO_DATA} DESTINATION ${WEB_AUDIO_DIR})
    add_definitions(-DUNINSTALLED_AUDIO_RESOURCES_DIR="${WEBCORE_DIR}/platform/audio/resources")
endif ()

# Directory listing template for FTP/file/gopher directory listings.
install(FILES "${WEBCORE_DIR}/platform/haiku/resources/Directory Listing Template.html"
    DESTINATION ${CMAKE_INSTALL_PREFIX}/${DATA_INSTALL_DIR})

if (ENABLE_SPELLCHECK)
    list(APPEND WebCore_INCLUDE_DIRECTORIES
        ${ENCHANT_INCLUDE_DIRS}
    )
    list(APPEND WebCore_LIBRARIES
        ${ENCHANT_LIBRARIES}
    )
endif ()

if (ENABLE_ACCESSIBILITY)
    list(APPEND WebCore_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/accessibility/atk"
        ${ATK_INCLUDE_DIRS}
    )
    list(APPEND WebCore_LIBRARIES
        ${ATK_LIBRARIES}
    )
endif ()

if (USE_TEXTURE_MAPPER)
    # GraphicsLayerTextureMapper.cpp (added by TextureMapper.cmake while
    # USE_COORDINATED_GRAPHICS is off) defines GraphicsLayer::create(), as does
    # GraphicsLayerHaiku.cpp. Exactly one of the two may be compiled.
    include(platform/TextureMapper.cmake)

    if (USE_COORDINATED_GRAPHICS)
        # The coordinated layer tree, its tiles, and SkiaPaintingEngine.
        include(platform/CoordinatedGraphics.cmake)
        # ScrollbarsControllerCoordinated derives from this one.
        list(APPEND WebCore_SOURCES
            platform/generic/ScrollbarsControllerGeneric.cpp
        )
    endif ()

else ()
    list(APPEND WebCore_SOURCES
        platform/graphics/haiku/GraphicsLayerHaiku.cpp
    )
endif ()

# GraphicsContextSkia and the accelerated ImageBuffer backend call into
# GLContext even when they rasterise on the CPU, so Skia needs these whether or
# not TextureMapper is compositing.
if (USE_TEXTURE_MAPPER OR USE_SKIA)
    list(APPEND WebCore_SOURCES
        platform/graphics/egl/GLContext.cpp
        platform/graphics/egl/GLDisplay.cpp

        platform/graphics/haiku/PlatformDisplayHaiku.cpp
    )

    # WebGL: the ANGLE display on top of PlatformDisplayHaiku's EGL display.
    if (ENABLE_WEBGL)
        list(APPEND WebCore_SOURCES
            platform/graphics/angle/PlatformDisplayANGLE.cpp
        )
    endif ()

    # SkiaGPUAtlas uploads through BitmapTexture and synchronises with GLFence,
    # so both are needed even without TextureMapper compositing. BitmapTexture
    # only wants TextureMapperFlags.h from the texmap directory.
    list(APPEND WebCore_SOURCES
        platform/graphics/egl/BitmapTexture.cpp
        platform/graphics/egl/BitmapTexturePool.cpp
        platform/graphics/egl/GLContextWrapper.cpp
        platform/graphics/egl/GLFence.cpp
        platform/graphics/egl/GLFenceEGL.cpp
        platform/graphics/egl/GLFenceGL.cpp
    )

    list(APPEND WebCore_PRIVATE_INCLUDE_DIRECTORIES
        "${WEBCORE_DIR}/platform/graphics/texmap"
    )


    list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
        platform/graphics/haiku/PlatformDisplayHaiku.h
    )

    list(APPEND WebCore_SYSTEM_INCLUDE_DIRECTORIES
        ${EGL_INCLUDE_DIRS}
        ${OpenGLES2_INCLUDE_DIRS}
    )

    # FindEGL.cmake provides variables only; FindOpenGLES2.cmake provides the
    # OpenGL::GLES imported target.
    list(APPEND WebCore_LIBRARIES
        ${EGL_LIBRARIES}
        OpenGL::GLES
    )
endif ()

list(APPEND WebCore_PRIVATE_FRAMEWORK_HEADERS
    platform/haiku/PopupMenuHaiku.h
    platform/haiku/SearchPopupMenuHaiku.h

    html/InputTypeNames.h
    platform/DateTimeChooser.h
    platform/DateTimeChooserClient.h
    platform/DateTimeChooserParameters.h

    platform/graphics/haiku/ImageBufferDataHaiku.h
    platform/graphics/haiku/PaintStatisticsHaiku.h
    platform/graphics/Image.h

    inspector/LegacyWebSocketInspectorInstrumentation.h
)

set(CSS_VALUE_PLATFORM_DEFINES "HAVE_OS_DARK_MODE_SUPPORT=1")
