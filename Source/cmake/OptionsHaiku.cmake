include(GNUInstallDirs)
include(VersioningUtils)

SET(PROJECT_VERSION_MAJOR 1)
SET(PROJECT_VERSION_MINOR 10)
SET(PROJECT_VERSION_PATCH 0)
SET(PROJECT_VERSION ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}.${PROJECT_VERSION_PATCH})

add_definitions(-DHAIKU_WEBKIT_VERSION=\"${PROJECT_VERSION}\")
add_definitions(-DBUILDING_HAIKU__=1)

CALCULATE_LIBRARY_VERSIONS_FROM_LIBTOOL_TRIPLE(JAVASCRIPTCORE 25 4 7)

# Force libstdc++ to export std::isinf and friends. This should be fixed on
# Haiku side ultimately.
add_definitions(-D_GLIBCXX_USE_C99_MATH)
# Enable asprintf for logging
add_definitions(-D_GNU_SOURCE)

# Uncomment to enable logs even in release mode
# add_definitions(-DLOG_DISABLED=0 -DERROR_DISABLED=0 -DFATAL_DISABLED=0)

option(ENABLE_WEBKIT_LEGACY "Enable building the Haiku legacy embedding" ON)
SET_AND_EXPOSE_TO_BUILD(USE_UNIX_DOMAIN_SOCKETS ${ENABLE_WEBKIT})

set(USE_ANGLE_EGL OFF)

# To get assertions in release mode, we replace all -DNDEBUG with -UNDEBUG
# (they are automatically added by CMake and there is no "release with asserts"
# build available in WebKit)
#foreach(flag_var
#		CMAKE_CXX_FLAGS CMAKE_CXX_FLAGS_DEBUG CMAKE_CXX_FLAGS_RELEASE
#		CMAKE_CXX_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_RELWITHDEBINFO)
#	if(${flag_var} MATCHES "-DNDEBUG")
#		string(REGEX REPLACE "-DNDEBUG" "-UNDEBUG" ${flag_var} "${${flag_var}}")
#	endif(${flag_var} MATCHES "-DNDEBUG")
#endforeach(flag_var)
#set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -fvisibility-inlines-hidden")

find_package(ICU 74 REQUIRED COMPONENTS data i18n uc)
FIND_PACKAGE(JPEG REQUIRED)
FIND_PACKAGE(LibXml2 2.8.0 REQUIRED)
FIND_PACKAGE(LibXslt 1.1.7 REQUIRED)
find_package(OpenGLES2 REQUIRED)
FIND_PACKAGE(PNG REQUIRED)
FIND_PACKAGE(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(WebP REQUIRED COMPONENTS demux)
FIND_PACKAGE(ZLIB REQUIRED)

set(USE_ICU_UNICODE 1)

set(DATA_INSTALL_DIR "data/WebKit" CACHE PATH "Installation path for data")
set(HEADER_INSTALL_DIR "develop/headers${CMAKE_HAIKU_SECONDARY_ARCH_SUBDIR}" CACHE PATH "Installation path for header files")
set(THEME_BINARY_DIR ${CMAKE_BINARY_DIR}/WebCore/platform/efl/DefaultTheme)
set(WEB_INSPECTOR_DIR "${DATA_INSTALL_DIR}/inspector")
set(WEBINSPECTORUI_DIR "${CMAKE_SOURCE_DIR}/Source/WebInspectorUI")

add_definitions(-DDATA_DIR="${CMAKE_INSTALL_PREFIX}/${DATA_INSTALL_DIR}")

WEBKIT_OPTION_BEGIN()
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ATTACHMENT_ELEMENT PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DARK_MODE_CSS PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_DRAG_SUPPORT PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ENCRYPTED_MEDIA PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_FULLSCREEN_API PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GAMEPAD PUBLIC OFF)
# The host finds the position (Summit: Wi-Fi networks and BeaconDB); see
# UIProcess/haiku/SitePermissionsHaiku.cpp.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_GEOLOCATION PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_CAPTURE PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_SOURCE PUBLIC ON)
# Camera, microphone and screen capture through the Media Kit and app_server
# (platform/mediastream/haiku), WebRTC through libwebrtc as on GTK and WPE
# (USE_LIBWEBRTC below), MediaRecorder writing WebM (VP8/VP9 and Opus).
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_STREAM PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEDIA_RECORDER PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_RTC PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MEMORY_SAMPLER PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MHTML PRIVATE ON)
# OffscreenCanvas, as on GTK and WPE: pages use it for off-thread drawing and
# extensions draw their toolbar icons with it in service workers (Stylus).
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_OFFSCREEN_CANVAS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_OFFSCREEN_CANVAS_IN_WORKERS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_MOUSE_CURSOR_SCALE PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_NOTIFICATIONS PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SAMPLING_PROFILER PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SPEECH_SYNTHESIS PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SPELLCHECK PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VARIATION_FONTS PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_TOUCH_EVENTS PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_VIDEO PUBLIC ON)
# Output through the Media Kit (AudioDestinationHaiku), FFTs with PFFFT.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUDIO PUBLIC ON)
# Password managers extend Web Authentication from their content scripts, which
# they can only do when the page exposes the API. KunanyiOS has no platform
# authenticator of its own; see WebAuthenticatorCoordinatorProxyHaiku.cpp.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEB_AUTHN PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_XSLT PUBLIC ON)

WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_LEGACY_CUSTOM_PROTOCOL_MANAGER PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_REMOTE_INSPECTOR PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_SMOOTH_SCROLLING PRIVATE ON)
# WebGL renders through ANGLE on the system EGL, and its canvases are composited
# as GL textures, so it needs GL compositing (with Skia, for the coordinated
# layer tree). On with it by default; see the checks after the option block.
if (USE_HAIKU_GL_COMPOSITING)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGL PRIVATE ON)
else ()
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_WEBGL PRIVATE OFF)
endif ()

# Skia replaces the app_server drawing path: it rasterises in the web process,
# so there is no IPC per drawing call, and it is what SkiaPaintingEngine needs
# to paint tiles on a worker pool. Off by default until the port's present path
# has moved over; -DUSE_SKIA=ON selects it.
# The coordinated scrolling nodes are part of the coordinated layer tree, and
# they are only declared with async scrolling on, as on GTK. Only
# USE_HAIKU_GL_COMPOSITING can be read here: it is an ordinary cache entry, while
# USE_SKIA does not exist as a variable until the option block is closed.
if (USE_HAIKU_GL_COMPOSITING)
    WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_ASYNC_SCROLLING PRIVATE ON)
endif ()
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SKIA PRIVATE OFF)
# On the Haiku Skia/GL browser, mimalloc improved two uncontended Speedometer
# 3.1 runs by about 21% over the system allocator without a scroll regression.
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_SYSTEM_MALLOC PRIVATE OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_MIMALLOC PRIVATE ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_AVIF PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_LCMS PUBLIC ON)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_JPEGXL PUBLIC OFF)
WEBKIT_OPTION_DEFAULT_PORT_VALUE(USE_WOFF2 PUBLIC ON)

# TODO have a ENABLE_DEVELOPER_MODE option like the GTK build
WEBKIT_OPTION_DEFAULT_PORT_VALUE(ENABLE_LAYOUT_TESTS PRIVATE ON)

WEBKIT_OPTION_END()

# Skia needs HarfBuzz for shaping; the ICU component provides the Unicode
# functions it uses for it. Both come from HaikuPorts (harfbuzz_devel).
if (USE_SKIA)
    find_package(HarfBuzz 2.7.4 REQUIRED COMPONENTS ICU)
    # The Haiku themes need a BView to hand to BControlLook, which Skia does not
    # provide. Adwaita paints through GraphicsContext and is what the other Skia
    # ports use.
    SET_AND_EXPOSE_TO_BUILD(USE_THEME_ADWAITA TRUE)
    # Skia shapes text with HarfBuzz (SkiaHarfBuzzFont), so the HarfBuzz helpers
    # in platform/graphics/harfbuzz have to be compiled in too.
    SET_AND_EXPOSE_TO_BUILD(USE_HARFBUZZ ON)
endif ()

if (ENABLE_MEDIA_STREAM AND ENABLE_WEB_RTC)
    SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC TRUE)
else ()
    SET_AND_EXPOSE_TO_BUILD(USE_LIBWEBRTC FALSE)
endif ()

set(USE_3D_GRAPHICS 0)
add_definitions(-DUSE_3D_GRAPHICS=0)

if (USE_AVIF)
    find_package(AVIF 0.9.0)
    if (NOT AVIF_FOUND)
        message(FATAL_ERROR "libavif 0.9.0 is required for USE_AVIF.")
    endif ()
endif ()

if (USE_JPEGXL)
    find_package(JPEGXL 0.7.0)
    if (NOT JPEGXL_FOUND)
        message(FATAL_ERROR "libjxl is required for USE_JPEGXL")
    endif ()
endif ()

if (USE_LCMS)
    find_package(LCMS2)
    if (NOT LCMS2_FOUND)
        message(FATAL_ERROR "libcms2 is required for USE_LCMS.")
    endif ()
endif ()

if (USE_WOFF2)
    find_package(WOFF2 1.0.2 COMPONENTS dec)
    if (NOT WOFF2_FOUND)
       message(FATAL_ERROR "libwoff2dec is required for USE_WOFF2")
    endif ()
endif ()

# WebGL as on GTK and WPE: ANGLE (Source/ThirdParty/ANGLE/PlatformHaiku.cmake)
# translates WebGL to the system GLES through EGL, in the web process (there is
# no GPU process), and GraphicsContextGLTextureMapperANGLE hands each frame to
# the coordinated compositor as a texture of a context shared with it. Without
# GBM there is no buffer sharing, so that texture path is the one used.
if (ENABLE_WEBGL)
    if (NOT USE_HAIKU_GL_COMPOSITING OR NOT USE_SKIA)
        message(FATAL_ERROR "ENABLE_WEBGL needs USE_HAIKU_GL_COMPOSITING and USE_SKIA (the coordinated compositor).")
    endif ()
    find_package(EGL REQUIRED)
    SET_AND_EXPOSE_TO_BUILD(USE_EGL TRUE)
endif ()
SET_AND_EXPOSE_TO_BUILD(USE_ANGLE ${ENABLE_WEBGL})

# GLContext.cpp compiles to nothing without USE(EGL), and Skia's graphics
# context refers to it whether or not anything is accelerated yet.
if (USE_SKIA AND NOT EGL_FOUND)
    find_package(EGL REQUIRED)
    SET_AND_EXPOSE_TO_BUILD(USE_EGL TRUE)
endif ()

SET_AND_EXPOSE_TO_BUILD(USE_INSPECTOR_SOCKET_SERVER ${ENABLE_REMOTE_INSPECTOR})

find_package(OpenSSL REQUIRED)
SET_AND_EXPOSE_TO_BUILD(USE_OPENSSL ON)

if (ENABLE_INSPECTOR)
    set(WEB_INSPECTOR_DIR "${DATA_INSTALL_DIR}/inspector")
    add_definitions(-DWEB_INSPECTOR_DIR=\"${CMAKE_BINARY_DIR}/${WEB_INSPECTOR_DIR}\")
    add_definitions(-DWEB_INSPECTOR_INSTALL_DIR=\"${CMAKE_INSTALL_PREFIX}/${WEB_INSPECTOR_DIR}\")
endif ()

# Packaging data
if(CMAKE_HAIKU_SECONDARY_ARCH)
    # Building for secondary architecture, setup the suffix
    set(PACKAGE_SUFFIX _${CMAKE_HAIKU_SECONDARY_ARCH})
endif()

# Optimize binary size for release builds by removing dead sections
if (CMAKE_BUILD_TYPE STREQUAL Release AND CMAKE_COMPILER_IS_GNUCC)
	string(APPEND CMAKE_C_FLAGS   " -ffunction-sections -fdata-sections")
	string(APPEND CMAKE_CXX_FLAGS " -ffunction-sections -fdata-sections")
	string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--gc-sections")
endif ()

# Haiku's runtime_loader binds every symbol at load time (no lazy binding),
# and libWebKit alone has a quarter of a million symbol relocations. With a
# GNU hash table the lookups that miss an image are mostly settled by its
# bloom filter: a web process or the browser reaches its first frame about
# 230 ms sooner on the X399. The SysV table stays for older loaders.
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,--hash-style=both")
string(APPEND CMAKE_EXE_LINKER_FLAGS " -Wl,--hash-style=both")
# A library's calls to its own functions are bound inside it when it is
# linked, not looked up at every load: libWebKit's symbol relocations go
# from 355,762 to 111,373 and libJavaScriptCore's from 52,362 to 8,541, and a
# web process loads its libraries in 162 ms instead of 263. Nothing outside
# replaces the libraries' functions (data symbols are still looked up).
string(APPEND CMAKE_SHARED_LINKER_FLAGS " -Wl,-Bsymbolic-functions")

# Haiku actually make use of rtti in several places, so we can't really disable
# it, unlike on other platforms...
string(APPEND CMAKE_CXX_FLAGS_DEBUG   " -frtti")
string(APPEND CMAKE_CXX_FLAGS_RELEASE " -frtti")

string(TOLOWER ${CMAKE_HOST_SYSTEM_PROCESSOR} LOWERCASE_CMAKE_HOST_SYSTEM_PROCESSOR)
if (CMAKE_COMPILER_IS_GNUCC AND "${LOWERCASE_CMAKE_HOST_SYSTEM_PROCESSOR}" STREQUAL "x86")
    # i686 is the official requirement for Haiku, but WebKit really wants to use SSE2, so use
	# a 'newer' architecture here.
	string(APPEND CMAKE_C_FLAGS   " -march=pentium4")
	string(APPEND CMAKE_CXX_FLAGS " -march=pentium4")
endif()

# GPU compositing (docs/gpu-compositing.md in the Summit tree). The WebProcess
# composites GraphicsLayerTextureMapper layers with OpenGL ES 2 into an
# offscreen EGL surface and reads the frame back into the shared bitmap that
# the software path already sends to the UI process. This only builds the
# code: compositing is selected at runtime and software stays the default.
#
# Only USE_TEXTURE_MAPPER is needed. USE_GRAPHICS_LAYER_TEXTURE_MAPPER is left
# off on purpose: no WebCore code tests it, and in WebKit it only plumbs the
# native-window based LayerTreeHostTextureMapper, which Haiku does not use.
# USE_COORDINATED_GRAPHICS needed a rasteriser it could paint tiles with, which
# is why it stayed off while the port drew through app_server. With USE_SKIA it
# has one, and it brings what the GL path was missing: a scroll becomes a
# translate of tiles that are already painted, and SkiaPaintingEngine paints
# those tiles on a worker pool instead of on the one web process thread.
# Off by default until the GL path has been validated. Changing it rewrites cmakeconfig.h and
# rebuilds everything, so it is built in its own directory:
#   SUMMIT_ENGINE_BUILD_NAME=ModernGL SUMMIT_ENGINE_CMAKE_EXTRA="-DUSE_HAIKU_GL_COMPOSITING=ON ..."
option(USE_HAIKU_GL_COMPOSITING "Build TextureMapper (OpenGL ES 2) compositing for Haiku" OFF)
if (USE_HAIKU_GL_COMPOSITING)
    # OpenGLES2 is already required above. FindEGL.cmake defines no imported
    # target; it sets EGL_INCLUDE_DIRS and EGL_LIBRARIES.
    find_package(EGL REQUIRED)
    SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER ON)
    SET_AND_EXPOSE_TO_BUILD(USE_COORDINATED_GRAPHICS ${USE_SKIA})
    if (USE_SKIA)
        # The coordinated renderers reach GL and EGL through libepoxy, as the
        # GTK and WPE ports do. It is in HaikuPorts (libepoxy_devel).
        find_package(Epoxy 1.5.4 REQUIRED)
        SET_AND_EXPOSE_TO_BUILD(USE_LIBEPOXY TRUE)
    endif ()
else ()
    SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER OFF)
    SET_AND_EXPOSE_TO_BUILD(USE_COORDINATED_GRAPHICS OFF)
endif ()
SET_AND_EXPOSE_TO_BUILD(USE_TEXTURE_MAPPER_GL OFF)
# Skia's graphics context and image buffers call GLContext whatever they are
# rasterising with, and compositing needs it outright, so EGL is only turned off
# for the plain app_server build.
if (NOT USE_SKIA AND NOT USE_HAIKU_GL_COMPOSITING)
    SET_AND_EXPOSE_TO_BUILD(USE_EGL FALSE)
endif ()
SET_AND_EXPOSE_TO_BUILD(USE_TILED_BACKING_STORE OFF)
SET_AND_EXPOSE_TO_BUILD(HAVE_OS_DARK_MODE_SUPPORT 1)

set(DERIVED_SOURCES_DIR "${CMAKE_BINARY_DIR}/DerivedSources")
set(DERIVED_SOURCES_WEBKIT_DIR ${DERIVED_SOURCES_DIR}/WebKit)
set(DERIVED_SOURCES_HAIKU_API_DIR ${DERIVED_SOURCES_WEBKIT_DIR}/haiku)

set(FORWARDING_HEADERS_DIR ${DERIVED_SOURCES_DIR}/ForwardingHeaders)

SET_AND_EXPOSE_TO_BUILD(ENABLE_GRAPHICS_CONTEXT_3D FALSE)

if (ENABLE_SPELLCHECK)
    find_package(Enchant REQUIRED)
endif ()

if (ENABLE_ACCESSIBILITY)
    find_package(ATK REQUIRED)
else ()
	SET_AND_EXPOSE_TO_BUILD(HAVE_ACCESSIBILITY FALSE)
endif ()

if (ENABLE_INDEXED_DATABASE)
    set(USE_LEVELDB 1)
    add_definitions(-DUSE_LEVELDB=1)
endif ()

# Shared JavaScriptCore owns WTF and bmalloc for all engine consumers. Include
# every object so WTF APIs used only by WebCore or WebKit are exported too.
set(bmalloc_LIBRARY_TYPE OBJECT)
set(WTF_LIBRARY_TYPE OBJECT)
set(JavaScriptCore_LIBRARY_TYPE SHARED)
set(SHOULD_INSTALL_JS_SHELL ON)

SET_AND_EXPOSE_TO_BUILD(USE_CURL ON)
if (USE_CURL)
    find_package(CURL 7.77.0 REQUIRED)
    find_package(LibPSL 0.20.2 REQUIRED)
endif ()

# Default to hidden visibility
#set(CMAKE_CXX_VISIBILITY_PRESET hidden)
#set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# Always link libgnu (memmem, ...)
find_library(GNU_LIBRARY gnu)

set(CMAKE_C_STANDARD_LIBRARIES
	"${CMAKE_C_STANDARD_LIBRARIES} ${GNU_LIBRARY}"
)
set(CMAKE_CXX_STANDARD_LIBRARIES
	"${CMAKE_CXX_STANDARD_LIBRARIES} ${GNU_LIBRARY}"
)

# chrome.offscreen (MV3 offscreen documents). WebKit implements the API behind this
# flag; the Haiku port provides the UI-process half in
# UIProcess/Extensions/haiku/WebExtensionContextAPIOffscreenHaiku.cpp. It must be a
# global definition: WebCore's generated Settings declare webExtensionOffscreenEnabled
# under the same condition.
if (ENABLE_WK_WEB_EXTENSIONS)
    add_definitions(-DENABLE_WK_WEB_EXTENSIONS_OFFSCREEN=1)
endif ()
