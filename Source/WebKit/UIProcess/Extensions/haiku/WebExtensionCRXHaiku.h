/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#pragma once
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace WebKit {
enum class WebExtensionCRXErrorHaiku : uint8_t { InvalidHeader, UnsupportedVersion, LimitExceeded, InvalidSignature, MissingDeveloperProof };
struct WebExtensionVerifiedCRXHaiku {
    size_t archiveOffset;
    std::string identifier;
};
// Verifies every CRX3 proof over the exact signed header and ZIP bytes, and
// requires a developer key whose SHA-256 prefix matches the signed ID. This
// authenticates package integrity/identity, not Chrome Web Store provenance.
std::expected<WebExtensionVerifiedCRXHaiku, WebExtensionCRXErrorHaiku> verifyWebExtensionCRXHaiku(std::span<const uint8_t>);
}
