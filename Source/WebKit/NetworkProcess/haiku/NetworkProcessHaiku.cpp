/*
 * Copyright (C) 2019 Haiku, Inc.
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
#include "NetworkProcess.h"

#include "NetworkProcessCreationParameters.h"
#include <WebCore/NetworkStorageSession.h>
#include <WebCore/NotImplemented.h>
#include <netdb.h>
#include <sys/socket.h>

namespace WebCore
{
	class NetworkStorageSession;
}

namespace WebKit {

using namespace WebCore;

void NetworkProcess::platformInitializeNetworkProcess(const NetworkProcessCreationParameters& parameters)
{
    UNUSED_PARAM(parameters);
    // libnetwork parses /etc/nsswitch.conf on the first name lookup, in
    // whichever thread makes it, with a flex scanner that calls exit() when a
    // read of the file fails. With a session of tabs starting at once that
    // first lookup came from one of a dozen libcurl resolver threads while
    // sockets and cache files were being opened and closed all around it, and
    // the read did fail (crash report: "input in flex scanner failed", then a
    // segfault in the main thread's IPC after libbe's statics had gone). The
    // parse is done here instead, on the main thread, before any other
    // thread exists: "localhost" is answered from /etc/hosts, no network needed.
    struct addrinfo* result = nullptr;
    if (!getaddrinfo("localhost", nullptr, nullptr, &result) && result)
        freeaddrinfo(result);
}

void NetworkProcess::allowSpecificHTTPSCertificateForHost(const CertificateInfo& certificateInfo, const String& host)
{
    notImplemented();
}

void NetworkProcess::platformTerminate()
{
    notImplemented();
}

void NetworkProcess::platformPrepareToSuspend(CompletionHandler<void()>&& completionHandler)
{
    notImplemented();
    completionHandler();
}

void NetworkProcess::clearDiskCache(WallTime modifiedSince, CompletionHandler<void()>&& completionHandler)
{
    notImplemented();
}

} // namespace WebKit
