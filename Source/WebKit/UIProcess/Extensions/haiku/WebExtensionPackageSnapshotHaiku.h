/*
 * Copyright (C) 2026 KunanyiOS contributors.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if PLATFORM(HAIKU)

#include "WebExtensionArchiveHaiku.h"
#include <wtf/Forward.h>
#include <wtf/Vector.h>

namespace WebKit {

enum class WebExtensionPackageErrorHaiku : uint8_t { InvalidPath, FileSystem, SourceChanged, ExpansionLimit, InvalidArchive };

class WebExtensionPackageSnapshotHaiku {
public:
    WebExtensionPackageSnapshotHaiku(String&& path, String&& fingerprint, String&& verifiedCRXIdentifier = { }, bool persistent = false);
    WebExtensionPackageSnapshotHaiku(WebExtensionPackageSnapshotHaiku&&);
    WebExtensionPackageSnapshotHaiku(const WebExtensionPackageSnapshotHaiku&) = delete;
    ~WebExtensionPackageSnapshotHaiku();
    const String& path() const { return m_path; }
    const String& fingerprint() const { return m_fingerprint; }
    String release();
    const String& verifiedCRXIdentifier() const { return m_verifiedCRXIdentifier; }
    // A persistent snapshot lives in the package cache and outlives its owner;
    // neither this object nor the WebExtension created from it removes it.
    bool isPersistent() const { return m_persistent; }
    // Transfer directory ownership and isolate string storage before dispatching
    // this value to another thread. The original no longer owns any resources.
    WebExtensionPackageSnapshotHaiku isolatedCopy() &&;
private:
    String m_path;
    String m_fingerprint;
    String m_verifiedCRXIdentifier;
    bool m_persistent { false };
};

// Copy and hash the exact resources which will be loaded. Contained symbolic
// links are materialized; escapes, cycles and observed concurrent changes fail.
std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> snapshotWebExtensionPackageHaiku(const String& directory, const WebExtensionArchiveLimitsHaiku& = { }, const String& temporaryParent = { });

// Filesystem-only preparation; safe on a worker queue. ZIP/XPI archives are
// extracted and directories are copied into the same owned snapshot format.
// CRX3 signer identity survives extraction, snapshotting and worker dispatch.
// An archive is unpacked once into the package cache (keyed by the archive's
// digest) and reused by later launches after its fingerprint is verified.
std::expected<WebExtensionPackageSnapshotHaiku, WebExtensionPackageErrorHaiku> prepareWebExtensionPackageHaiku(const String& path, const WebExtensionArchiveLimitsHaiku& = { }, const String& temporaryParent = { });


struct WebExtensionMemoryResourceHaiku {
    String path;
    std::span<const uint8_t> bytes;
};
String fingerprintWebExtensionMemoryResourcesHaiku(const String& manifestJSON, Vector<WebExtensionMemoryResourceHaiku>&& data, Vector<WebExtensionMemoryResourceHaiku>&& strings);

} // namespace WebKit

#endif // PLATFORM(HAIKU)
