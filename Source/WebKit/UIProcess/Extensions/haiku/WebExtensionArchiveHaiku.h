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

#include <cstdint>
#include <expected>
#include <wtf/text/WTFString.h>

namespace WebKit {

struct WebExtensionArchiveLimitsHaiku {
    uint64_t archiveBytes { 128 * 1024 * 1024 };
    uint64_t fileBytes { 64 * 1024 * 1024 };
    uint64_t expandedBytes { 256 * 1024 * 1024 };
    unsigned entries { 10000 };
    unsigned pathBytes { 1024 };
    unsigned pathDepth { 32 };
};

enum class WebExtensionArchiveErrorHaiku : uint8_t {
    InvalidArchive,
    UnsafeEntry,
    ExpansionLimit,
    FileSystem,
};

// A failed extraction and an unconsumed successful result both remove their
// private directory. WebExtension releases it only after manifest parsing,
// then uses its existing m_resourcesAreTemporary destructor ownership.
class WebExtensionTemporaryDirectoryHaiku {
public:
    explicit WebExtensionTemporaryDirectoryHaiku(String&&, String&& verifiedCRXIdentifier = { });
    WebExtensionTemporaryDirectoryHaiku(WebExtensionTemporaryDirectoryHaiku&&);
    WebExtensionTemporaryDirectoryHaiku(const WebExtensionTemporaryDirectoryHaiku&) = delete;
    WebExtensionTemporaryDirectoryHaiku& operator=(const WebExtensionTemporaryDirectoryHaiku&) = delete;
    ~WebExtensionTemporaryDirectoryHaiku();

    const String& path() const { return m_path; }
    String release();
    const String& verifiedCRXIdentifier() const { return m_verifiedCRXIdentifier; }

private:
    String m_path;
    String m_verifiedCRXIdentifier;
};

std::expected<WebExtensionTemporaryDirectoryHaiku, WebExtensionArchiveErrorHaiku> extractWebExtensionArchiveHaiku(const String& archivePath, const WebExtensionArchiveLimitsHaiku& = { }, const String& temporaryParent = { });

} // namespace WebKit

#endif // PLATFORM(HAIKU)
