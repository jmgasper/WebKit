/*
 * Copyright 2014,2019 Haiku, Inc.
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

#include "config.h"
#include "WebProcessMain.h"

#include "AuxiliaryProcessMain.h"
#include "StartupTraceHaiku.h"
#include "WebProcess.h"
#include <wtf/WTFProcess.h>

#include <Application.h>
#include <cstdio>
#include <cstring>
#include <netdb.h>
namespace WebKit {
using namespace WebCore;

class WebProcessMainHaiku final : public AuxiliaryProcessMainBase<WebProcess> {
public:
    bool platformInitialize() override
    {
        SUMMIT_STARTUP_TRACE("web process main");
        // The media loaders of this process fetch with libcurl, whose
        // resolver threads call getaddrinfo() several at a time. libnetwork
        // parses nsswitch.conf on the first lookup and its scanner calls
        // exit() when another thread rewrites the path under it (see
        // NetworkProcessHaiku.cpp): pages with media took the web process
        // down. The first lookup is made here, before those threads exist.
        struct addrinfo* result = nullptr;
        if (!getaddrinfo("localhost", nullptr, nullptr, &result) && result)
            freeaddrinfo(result);

        // The one-argument constructor exits with status zero on failure,
        // preventing the auxiliary process entry point from reporting it.
        status_t status;
        m_application = std::make_unique<BApplication>("application/x-vnd.haiku-webkit.webprocess", &status);
        if (status != B_OK) {
            std::fprintf(stderr, "WebProcess: cannot initialize native application: %s (%ld)\n", std::strerror(status), static_cast<long>(status));
            return false;
        }
        return true;
    }

    void platformRunEventLoop() override
    {
        // WTF attaches its main-thread handler to this BApplication. The
        // application must drive that looper and maintain app_server access
        // for native fonts, images and offscreen drawing.
        m_application->Run();
    }

private:
    std::unique_ptr<BApplication> m_application;
};

int WebProcessMain(int argc, char** argv)
{
    // Haiku's exit() also destroys main-thread pthread data. WebCore keeps
    // process-wide timers and caches alive until process termination; their
    // TLS teardown can recreate ThreadGlobalData during font destruction.
    // Use the same native process exit path as an IPC connection shutdown,
    // after the normal auxiliary-process event loop has finished.
    exitProcess(AuxiliaryProcessMain<WebProcessMainHaiku>(argc, argv));
}

} // namespace WebKit
