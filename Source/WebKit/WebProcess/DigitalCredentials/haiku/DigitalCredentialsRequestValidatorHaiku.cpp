/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */

// The Digital Credentials coordinator is built with Web Authentication. Its
// request validator is implemented against Apple's Identity Document Services;
// KunanyiOS has no mobile document (ISO 18013) wallet, so no request can be
// satisfied here and none validates. The coordinator then tells the page it
// has no matching credential, which is the outcome on any device without a
// wallet. Passkeys do not go through this path.

#include "config.h"
#include "DigitalCredentialsRequestValidatorBridge.h"

#if ENABLE(WEB_AUTHN) && PLATFORM(HAIKU)

namespace WebKit {
namespace DigitalCredentials {

Vector<WebCore::ValidatedMobileDocumentRequest> validateRequests(const WebCore::SecurityOrigin&, const WebCore::Document&, const Vector<WebCore::UnvalidatedDigitalCredentialRequest>&)
{
    return { };
}

} // namespace DigitalCredentials
} // namespace WebKit

#endif // ENABLE(WEB_AUTHN) && PLATFORM(HAIKU)
