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

#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)

#include <wtf/JSONValues.h>
#include <wtf/ASCIICType.h>

namespace WebKit {

enum class WebExtensionLoadPurposeHaiku : uint8_t { UserInitiated, BrowserStartup, BrowserUpdate, PrivateBrowsing };
enum class WebExtensionInstallReasonHaiku : uint8_t { None, Install, Update, BrowserUpdate };
struct WebExtensionInstallDecisionHaiku {
    WebExtensionInstallReasonHaiku reason { WebExtensionInstallReasonHaiku::None };
    String previousVersion;
    bool fireStartup { false };
    bool clearCachedState { false };
};

inline WebExtensionInstallDecisionHaiku updateWebExtensionInstallStateHaiku(JSON::Object& state, const String& version, const String& fingerprint, WebExtensionLoadPurposeHaiku purpose)
{
    constexpr auto versionKey = "LastSeenVersion"_s;
    constexpr auto fingerprintKey = "LastSeenResourceFingerprint"_s;
    constexpr auto formatKey = "ResourceFingerprintFormat"_s;
    auto previousVersion = state.getString(versionKey);
    auto previousFingerprint = state.getString(fingerprintKey);
    bool validFingerprint = state.getDouble(formatKey) == 1 && previousFingerprint.length() == 64;
    if (validFingerprint) {
        for (auto character : StringView(previousFingerprint).codeUnits()) {
            if (!isASCIIHexDigit(character)) {
                validFingerprint = false;
                break;
            }
        }
    }
    bool previouslyInstalled = !previousVersion.isEmpty() || validFingerprint;
    bool versionChanged = !previousVersion.isEmpty() && previousVersion != version;
    bool contentsChanged = validFingerprint && previousFingerprint != fingerprint;
    WebExtensionInstallDecisionHaiku result;
    result.previousVersion = previousVersion;
    result.clearCachedState = versionChanged || contentsChanged || !validFingerprint;
    if (purpose != WebExtensionLoadPurposeHaiku::PrivateBrowsing) {
        if (!previouslyInstalled)
            result.reason = WebExtensionInstallReasonHaiku::Install;
        else if (versionChanged || contentsChanged)
            result.reason = WebExtensionInstallReasonHaiku::Update;
        else if (purpose == WebExtensionLoadPurposeHaiku::BrowserUpdate)
            result.reason = WebExtensionInstallReasonHaiku::BrowserUpdate;
        result.fireStartup = previouslyInstalled && (purpose == WebExtensionLoadPurposeHaiku::BrowserStartup || purpose == WebExtensionLoadPurposeHaiku::BrowserUpdate);
    }
    state.setString(versionKey, version);
    state.setString(fingerprintKey, fingerprint);
    state.setInteger(formatKey, 1);
    return result;
}

} // namespace WebKit

#endif // ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
