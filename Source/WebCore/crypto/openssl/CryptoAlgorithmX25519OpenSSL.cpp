/*
 * Copyright (C) 2026 KunanyiOS contributors.
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
#include "CryptoAlgorithmX25519.h"

#include "CryptoKeyOKP.h"
#include "CryptoKeyOKPOpenSSL.h"
#include <openssl/evp.h>

namespace WebCore {

// X25519 (RFC 7748). OpenSSL refuses an all-zero shared secret (a small-order
// peer key), which WebCrypto also requires to be an error.
std::optional<Vector<uint8_t>> CryptoAlgorithmX25519::platformDeriveBits(const CryptoKeyOKP& baseKey, const CryptoKeyOKP& publicKey)
{
    auto privateKey = OpenSSLOKP::privateKey(baseKey.namedCurve(), baseKey.platformKey().span());
    auto peerKey = OpenSSLOKP::publicKey(publicKey.namedCurve(), publicKey.platformKey().span());
    if (!privateKey || !peerKey)
        return std::nullopt;
    auto context = EvpPKeyCtxPtr(EVP_PKEY_CTX_new(privateKey.get(), nullptr));
    if (!context || EVP_PKEY_derive_init(context.get()) != 1 || EVP_PKEY_derive_set_peer(context.get(), peerKey.get()) != 1)
        return std::nullopt;
    size_t length = 0;
    if (EVP_PKEY_derive(context.get(), nullptr, &length) != 1)
        return std::nullopt;
    Vector<uint8_t> secret(length);
    if (EVP_PKEY_derive(context.get(), secret.mutableSpan().data(), &length) != 1)
        return std::nullopt;
    secret.shrink(length);
    return secret;
}

} // namespace WebCore
