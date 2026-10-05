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
#include "CryptoKeyOKP.h"

#include "CryptoKeyOKPOpenSSL.h"
#include "CryptoKeyPair.h"
#include "ExceptionOr.h"
#include "JsonWebKey.h"
#include "OpenSSLCryptoUniquePtr.h"
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <wtf/text/Base64.h>

namespace WebCore {

// Ed25519 and X25519 keys (RFC 8032, RFC 7748) with OpenSSL 3. The key object
// keeps the raw 32-byte key, as the other ports do; OpenSSL makes EVP keys of
// it when one is needed and parses and writes the SPKI and PKCS #8 forms.

int OpenSSLOKP::keyType(CryptoKeyOKP::NamedCurve curve)
{
    return curve == CryptoKeyOKP::NamedCurve::Ed25519 ? EVP_PKEY_ED25519 : EVP_PKEY_X25519;
}

EvpPKeyPtr OpenSSLOKP::privateKey(CryptoKeyOKP::NamedCurve curve, std::span<const uint8_t> key)
{
    return EvpPKeyPtr(EVP_PKEY_new_raw_private_key(keyType(curve), nullptr, key.data(), key.size()));
}

EvpPKeyPtr OpenSSLOKP::publicKey(CryptoKeyOKP::NamedCurve curve, std::span<const uint8_t> key)
{
    return EvpPKeyPtr(EVP_PKEY_new_raw_public_key(keyType(curve), nullptr, key.data(), key.size()));
}

std::optional<Vector<uint8_t>> OpenSSLOKP::rawPublicKey(EVP_PKEY* key)
{
    size_t length = 0;
    if (!key || EVP_PKEY_get_raw_public_key(key, nullptr, &length) != 1)
        return std::nullopt;
    Vector<uint8_t> result(length);
    if (EVP_PKEY_get_raw_public_key(key, result.mutableSpan().data(), &length) != 1)
        return std::nullopt;
    result.shrink(length);
    return WTF::move(result);
}

std::optional<Vector<uint8_t>> OpenSSLOKP::rawPrivateKey(EVP_PKEY* key)
{
    size_t length = 0;
    if (!key || EVP_PKEY_get_raw_private_key(key, nullptr, &length) != 1)
        return std::nullopt;
    Vector<uint8_t> result(length);
    if (EVP_PKEY_get_raw_private_key(key, result.mutableSpan().data(), &length) != 1)
        return std::nullopt;
    result.shrink(length);
    return WTF::move(result);
}

bool CryptoKeyOKP::supportsNamedCurve()
{
    return true;
}

std::optional<CryptoKeyPair> CryptoKeyOKP::platformGeneratePair(CryptoAlgorithmIdentifier identifier, NamedCurve namedCurve, bool extractable, CryptoKeyUsageBitmap usages)
{
    auto context = EvpPKeyCtxPtr(EVP_PKEY_CTX_new_id(OpenSSLOKP::keyType(namedCurve), nullptr));
    if (!context || EVP_PKEY_keygen_init(context.get()) <= 0)
        return std::nullopt;
    EVP_PKEY* generated = nullptr;
    if (EVP_PKEY_keygen(context.get(), &generated) <= 0)
        return std::nullopt;
    auto key = EvpPKeyPtr(generated);
    auto privateData = OpenSSLOKP::rawPrivateKey(key.get());
    auto publicData = OpenSSLOKP::rawPublicKey(key.get());
    if (!privateData || !publicData)
        return std::nullopt;

    bool isPublicKeyExtractable = true;
    auto publicKey = CryptoKeyOKP::create(identifier, namedCurve, CryptoKeyType::Public, WTF::move(*publicData), isPublicKeyExtractable, usages);
    auto privateKey = CryptoKeyOKP::create(identifier, namedCurve, CryptoKeyType::Private, WTF::move(*privateData), extractable, usages);
    if (!publicKey || !privateKey)
        return std::nullopt;
    return CryptoKeyPair { WTF::move(publicKey), WTF::move(privateKey) };
}

bool CryptoKeyOKP::platformCheckPairedKeys(CryptoAlgorithmIdentifier, NamedCurve namedCurve, const Vector<uint8_t>& privateKey, const Vector<uint8_t>& publicKey)
{
    auto key = OpenSSLOKP::privateKey(namedCurve, privateKey.span());
    auto derived = OpenSSLOKP::rawPublicKey(key.get());
    return derived && *derived == publicKey;
}

RefPtr<CryptoKeyOKP> CryptoKeyOKP::importSpki(CryptoAlgorithmIdentifier identifier, NamedCurve namedCurve, Vector<uint8_t>&& keyData, bool extractable, CryptoKeyUsageBitmap usages)
{
    const uint8_t* data = keyData.span().data();
    auto key = EvpPKeyPtr(d2i_PUBKEY(nullptr, &data, keyData.size()));
    // All of the input must be the one key, and of this curve (RFC 8410).
    if (!key || data != keyData.span().data() + keyData.size() || EVP_PKEY_get_id(key.get()) != OpenSSLOKP::keyType(namedCurve))
        return nullptr;
    auto raw = OpenSSLOKP::rawPublicKey(key.get());
    if (!raw)
        return nullptr;
    return create(identifier, namedCurve, CryptoKeyType::Public, WTF::move(*raw), extractable, usages);
}

ExceptionOr<Vector<uint8_t>> CryptoKeyOKP::exportSpki() const
{
    if (type() != CryptoKeyType::Public)
        return Exception { ExceptionCode::InvalidAccessError };
    auto key = OpenSSLOKP::publicKey(namedCurve(), platformKey().span());
    int length = key ? i2d_PUBKEY(key.get(), nullptr) : -1;
    if (length <= 0)
        return Exception { ExceptionCode::OperationError };
    Vector<uint8_t> result(length);
    uint8_t* output = result.mutableSpan().data();
    if (i2d_PUBKEY(key.get(), &output) != length)
        return Exception { ExceptionCode::OperationError };
    return WTF::move(result);
}

RefPtr<CryptoKeyOKP> CryptoKeyOKP::importPkcs8(CryptoAlgorithmIdentifier identifier, NamedCurve namedCurve, Vector<uint8_t>&& keyData, bool extractable, CryptoKeyUsageBitmap usages)
{
    const uint8_t* data = keyData.span().data();
    auto info = PKCS8PrivKeyInfoPtr(d2i_PKCS8_PRIV_KEY_INFO(nullptr, &data, keyData.size()));
    if (!info || data != keyData.span().data() + keyData.size())
        return nullptr;
    auto key = EvpPKeyPtr(EVP_PKCS82PKEY(info.get()));
    if (!key || EVP_PKEY_get_id(key.get()) != OpenSSLOKP::keyType(namedCurve))
        return nullptr;
    auto raw = OpenSSLOKP::rawPrivateKey(key.get());
    if (!raw)
        return nullptr;
    return create(identifier, namedCurve, CryptoKeyType::Private, WTF::move(*raw), extractable, usages);
}

ExceptionOr<Vector<uint8_t>> CryptoKeyOKP::exportPkcs8() const
{
    if (type() != CryptoKeyType::Private)
        return Exception { ExceptionCode::InvalidAccessError };
    auto key = OpenSSLOKP::privateKey(namedCurve(), platformKey().span());
    auto info = PKCS8PrivKeyInfoPtr(key ? EVP_PKEY2PKCS8(key.get()) : nullptr);
    int length = info ? i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr) : -1;
    if (length <= 0)
        return Exception { ExceptionCode::OperationError };
    Vector<uint8_t> result(length);
    uint8_t* output = result.mutableSpan().data();
    if (i2d_PKCS8_PRIV_KEY_INFO(info.get(), &output) != length)
        return Exception { ExceptionCode::OperationError };
    return WTF::move(result);
}

String CryptoKeyOKP::generateJwkD() const
{
    ASSERT(type() == CryptoKeyType::Private);
    return base64URLEncodeToString(m_data);
}

String CryptoKeyOKP::generateJwkX() const
{
    if (type() == CryptoKeyType::Public)
        return base64URLEncodeToString(m_data);

    ASSERT(type() == CryptoKeyType::Private);
    auto key = OpenSSLOKP::privateKey(namedCurve(), platformKey().span());
    auto publicData = OpenSSLOKP::rawPublicKey(key.get());
    if (!publicData)
        return { };
    return base64URLEncodeToString(publicData->span());
}

Vector<uint8_t> CryptoKeyOKP::platformExportRaw() const
{
    return m_data;
}

} // namespace WebCore
