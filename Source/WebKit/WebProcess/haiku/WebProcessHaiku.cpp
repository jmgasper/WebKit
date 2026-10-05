/*
 * Copyright (C) 2014,2019 Haiku, Inc.
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
 * THIS SOFTWARE IS PROVIDED BY MOTOROLA INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MOTOROLA INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "WebProcess.h"

#include "WebProcessCreationParameters.h"
#include "WebProcessDataStoreParameters.h"
#include <Application.h>
#if USE(COORDINATED_GRAPHICS)
#include <WebCore/PlatformDisplayHaiku.h>
#include "StartupTraceHaiku.h"
#endif
#include <wtf/Assertions.h>

#if ENABLE(VIDEO)
namespace WebCore {
// Defined in MediaPlayerPrivateHaiku.cpp; WebCore is linked into this library.
void prepareMediaEngineTypesHaiku();
}
#endif

namespace WebKit {

// The native app_server connection supplies fonts and drawing services.
// Mach sandbox extensions and CoreText's static registry do not exist here.
#if ENABLE(SANDBOX_EXTENSIONS) || HAVE(STATIC_FONT_REGISTRY)
#error Haiku needs native registry and sandbox hook implementations before enabling these features.
#endif

void WebProcess::platformInitializeProcess(const AuxiliaryProcessInitializationParameters&)
{
    // WebProcessMainHaiku establishes this connection before WebKit starts.
    // Native fonts, bitmaps and the main RunLoop require a valid BApplication
    // owned by this thread; continuing without it cannot render a page.
    RELEASE_ASSERT(be_app && be_app->InitCheck() == B_OK);
    // BApplication::Thread() is not assigned until Run(), which follows this
    // initialization. The creator must still own its initial looper lock.
    RELEASE_ASSERT(be_app->LockingThread() == find_thread(nullptr));
}

void WebProcess::platformInitializeWebProcess(WebProcessCreationParameters& parameters)
{
    SUMMIT_STARTUP_TRACE("initializing the web process");
    applyProcessCreationParameters(WTF::move(parameters.auxiliaryProcessParameters));
#if ENABLE(VIDEO)
    // The Media Kit's list of file formats, read on a thread of its own
    // before a page asks for it (MediaPlayerPrivateHaiku.cpp).
    WebCore::prepareMediaEngineTypesHaiku();
#endif
#if USE(COORDINATED_GRAPHICS)
    // The coordinated drawing area asks for the shared display as soon as a page
    // is created, and sharedDisplay() asserts rather than making one. The GLib
    // and PlayStation ports set theirs here too. LayerTreeHostHaiku used to
    // create it on the way into GL compositing, and it is not built any more.
    if (!WebCore::PlatformDisplay::sharedDisplayIfExists()) {
        auto display = WebCore::PlatformDisplayHaiku::create();
        if (!display) {
            // Coordinated graphics has no software path: sharedDisplay() asserts.
            WTFLogAlways("Summit: no EGL display in the web process. Coordinated graphics needs one; "
                "on this machine EGL comes from the private Mesa, so LIBRARY_PATH must include its prefix.");
        }
        if (display)
            WebCore::PlatformDisplay::setSharedDisplay(WTF::move(display));
        SUMMIT_STARTUP_TRACE("EGL display ready");
    }
#endif
}

void WebProcess::platformSetWebsiteDataStoreParameters(WebProcessDataStoreParameters&& parameters)
{
    // The common caller installs the session and media storage settings before
    // this hook, then connects to that session's NetworkProcess. Curl cookies,
    // HSTS, caches and general website storage live in that process. Haiku has
    // no additional native storage manager or sandbox grants to configure here.
    RELEASE_ASSERT(m_sessionID && *m_sessionID == parameters.sessionID);
}

void WebProcess::platformTerminate()
{
    // WebProcessMainHaiku owns the BApplication through the native event loop.
}

void WebProcess::platformSetCacheModel(CacheModel)
{
    // Common WebProcess::setCacheModel configures MemoryCache and the back /
    // forward cache; the NetworkProcess separately owns Curl's disk cache.
}

void WebProcess::grantAccessToAssetServices(Vector<SandboxExtensionHandle>&& handles)
{
    // This shared IPC hook grants access to Cocoa asset services. The Haiku UI
    // process never sends such handles, and app_server requires no such grant.
    RELEASE_ASSERT(handles.isEmpty());
}

void WebProcess::revokeAccessToAssetServices()
{
    RELEASE_ASSERT(m_assetServicesExtensions.isEmpty());
}

#if !ENABLE(REMOVE_XPC_AND_MACH_SANDBOX_EXTENSIONS_IN_WEBCONTENT)
void WebProcess::switchFromStaticFontRegistryToUserFontRegistry(Vector<SandboxExtensionHandle>&& handles)
{
    // BApplication already exposes app_server's installed fonts to FontCache;
    // there is no separate static registry or fontd Mach connection to switch.
    RELEASE_ASSERT(handles.isEmpty());
}
#endif

} // namespace WebKit
