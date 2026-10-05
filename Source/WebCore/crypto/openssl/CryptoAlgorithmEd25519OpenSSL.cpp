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
#include "CryptoAlgorithmEd25519.h"

#include "CryptoKeyOKP.h"
#include "CryptoKeyOKPOpenSSL.h"
#include "ExceptionOr.h"
#include <openssl/evp.h>

namespace WebCore {

// Ed25519 signs the message itself (no separate digest), so the one-shot
// EVP_DigestSign/Verify with no digest algorithm are used.
ExceptionOr<Vector<uint8_t>> CryptoAlgorithmEd25519::platformSign(const CryptoKeyOKP& key, const Vector<uint8_t>& data)
{
    auto privateKey = OpenSSLOKP::privateKey(key.namedCurve(), key.platformKey().span());
    auto context = EvpDigestCtxPtr(EVP_MD_CTX_new());
    if (!privateKey || !context || EVP_DigestSignInit(context.get(), nullptr, nullptr, nullptr, privateKey.get()) != 1)
        return Exception { ExceptionCode::OperationError };
    size_t length = 0;
    if (EVP_DigestSign(context.get(), nullptr, &length, data.span().data(), data.size()) != 1)
        return Exception { ExceptionCode::OperationError };
    Vector<uint8_t> signature(length);
    if (EVP_DigestSign(context.get(), signature.mutableSpan().data(), &length, data.span().data(), data.size()) != 1)
        return Exception { ExceptionCode::OperationError };
    signature.shrink(length);
    return WTF::move(signature);
}

ExceptionOr<bool> CryptoAlgorithmEd25519::platformVerify(const CryptoKeyOKP& key, const Vector<uint8_t>& signature, const Vector<uint8_t>& data)
{
    if (signature.size() != 64)
        return false;
    auto publicKey = OpenSSLOKP::publicKey(key.namedCurve(), key.platformKey().span());
    auto context = EvpDigestCtxPtr(EVP_MD_CTX_new());
    if (!publicKey || !context || EVP_DigestVerifyInit(context.get(), nullptr, nullptr, nullptr, publicKey.get()) != 1)
        return Exception { ExceptionCode::OperationError };
    return EVP_DigestVerify(context.get(), signature.span().data(), signature.size(), data.span().data(), data.size()) == 1;
}

} // namespace WebCore
