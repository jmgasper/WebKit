/* Copyright (C) 2026 KunanyiOS contributors. SPDX-License-Identifier: BSD-2-Clause */
#include "config.h"
#include "WebExtensionCRXHaiku.h"
#if ENABLE(WK_WEB_EXTENSIONS) && PLATFORM(HAIKU)
#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <vector>

namespace WebKit {
namespace {
using Bytes = std::span<const uint8_t>;
using Error = WebExtensionCRXErrorHaiku;
constexpr size_t maximumHeaderBytes = 1024 * 1024;
constexpr size_t maximumProofs = 64;
constexpr size_t maximumKeyBytes = 16 * 1024;
constexpr size_t maximumSignatureBytes = 16 * 1024;

uint32_t littleEndian32(Bytes bytes)
{
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}
struct Field { uint32_t number; unsigned wire; Bytes bytes; };
// A bounded protobuf wire reader. Preserve signed_header_data byte-for-byte;
// never reserialize it. Unknown fields, groups and repeated singular fields
// retain protobuf behavior, with a depth limit for unknown nested groups.
class Reader {
public:
    explicit Reader(Bytes input) : m_bytes(input) { }
    bool done() const { return m_bytes.empty(); }
    bool next(Field& field)
    {
        uint64_t tag;
        if (!varint(tag) || tag > std::numeric_limits<uint32_t>::max() || !(tag >> 3))
            return false;
        field = { static_cast<uint32_t>(tag >> 3), static_cast<unsigned>(tag & 7), { } };
        return consume(field, 0);
    }
private:
    bool varint(uint64_t& value)
    {
        value = 0;
        for (unsigned index = 0; index < 10; ++index) {
            if (m_bytes.empty()) return false;
            auto byte = m_bytes.front(); m_bytes = m_bytes.subspan(1);
            if (index == 9 && byte > 1) return false;
            value |= uint64_t(byte & 127) << (index * 7);
            if (!(byte & 128)) return true;
        }
        return false;
    }
    bool consume(Field& field, unsigned depth)
    {
        uint64_t length = 0;
        switch (field.wire) {
        case 0: return varint(length);
        case 1: length = 8; break;
        case 2: if (!varint(length)) return false; break;
        case 3: {
            if (depth >= 64) return false;
            while (!m_bytes.empty()) {
                uint64_t tag;
                if (!varint(tag) || tag > std::numeric_limits<uint32_t>::max() || !(tag >> 3)) return false;
                Field child { static_cast<uint32_t>(tag >> 3), static_cast<unsigned>(tag & 7), { } };
                if (child.wire == 4) return child.number == field.number;
                if (!consume(child, depth + 1)) return false;
            }
            return false;
        }
        case 5: length = 4; break;
        default: return false;
        }
        if (length > m_bytes.size()) return false;
        field.bytes = m_bytes.first(static_cast<size_t>(length));
        m_bytes = m_bytes.subspan(static_cast<size_t>(length));
        return true;
    }
    Bytes m_bytes;
};
struct Proof { unsigned algorithm; Bytes key; Bytes signature; };
std::expected<Proof, Error> parseProof(unsigned algorithm, Bytes bytes)
{
    Proof result { algorithm, { }, { } };
    Reader reader(bytes);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field)) return std::unexpected(Error::InvalidHeader);
        if (field.wire != 2) continue;
        if (field.number == 1) result.key = field.bytes;
        if (field.number == 2) result.signature = field.bytes;
    }
    if (result.key.empty() || result.signature.empty()) return std::unexpected(Error::InvalidHeader);
    if (result.key.size() > maximumKeyBytes || result.signature.size() > maximumSignatureBytes) return std::unexpected(Error::LimitExceeded);
    return result;
}
bool verify(const Proof& proof, Bytes signedHeader, Bytes archive)
{
    const auto* pointer = proof.key.data();
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(d2i_PUBKEY(nullptr, &pointer, static_cast<long>(proof.key.size())), EVP_PKEY_free);
    if (!key || pointer != proof.key.data() + proof.key.size()) return false;
    if ((proof.algorithm == 2 && EVP_PKEY_base_id(key.get()) != EVP_PKEY_RSA)
        || (proof.algorithm == 3 && EVP_PKEY_base_id(key.get()) != EVP_PKEY_EC)) return false;
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* keyContext = nullptr;
    if (!context || EVP_DigestVerifyInit(context.get(), &keyContext, EVP_sha256(), nullptr, key.get()) != 1) return false;
    // Chromium's implementation uses PKCS#1 v1.5, despite the old PSS comment
    // in crx3.proto. Do not guess the algorithm from that comment.
    if (proof.algorithm == 2 && EVP_PKEY_CTX_set_rsa_padding(keyContext, RSA_PKCS1_PADDING) != 1) return false;
    static constexpr char prefix[] = "CRX3 SignedData"; // Include the terminating NUL.
    auto size = static_cast<uint32_t>(signedHeader.size());
    std::array<uint8_t, 4> length { uint8_t(size), uint8_t(size >> 8), uint8_t(size >> 16), uint8_t(size >> 24) };
    return EVP_DigestVerifyUpdate(context.get(), prefix, sizeof(prefix)) == 1
        && EVP_DigestVerifyUpdate(context.get(), length.data(), length.size()) == 1
        && EVP_DigestVerifyUpdate(context.get(), signedHeader.data(), signedHeader.size()) == 1
        && EVP_DigestVerifyUpdate(context.get(), archive.data(), archive.size()) == 1
        && EVP_DigestVerifyFinal(context.get(), proof.signature.data(), proof.signature.size()) == 1;
}
}

std::expected<WebExtensionVerifiedCRXHaiku, WebExtensionCRXErrorHaiku> verifyWebExtensionCRXHaiku(std::span<const uint8_t> bytes)
{
    if (bytes.size() < 12 || !std::equal(bytes.begin(), bytes.begin() + 4, "Cr24")) return std::unexpected(Error::InvalidHeader);
    if (littleEndian32(bytes.subspan(4, 4)) != 3) return std::unexpected(Error::UnsupportedVersion);
    size_t headerSize = littleEndian32(bytes.subspan(8, 4));
    if (headerSize > maximumHeaderBytes) return std::unexpected(Error::LimitExceeded);
    if (headerSize > bytes.size() - 12) return std::unexpected(Error::InvalidHeader);
    auto header = bytes.subspan(12, headerSize);
    auto archive = bytes.subspan(12 + headerSize);
    if (archive.size() < 4 || archive[0] != 'P' || archive[1] != 'K'
        || !((archive[2] == 3 && archive[3] == 4) || (archive[2] == 5 && archive[3] == 6))) return std::unexpected(Error::InvalidHeader);
    Bytes signedHeader;
    std::vector<Proof> proofs;
    Reader reader(header);
    while (!reader.done()) {
        Field field;
        if (!reader.next(field)) return std::unexpected(Error::InvalidHeader);
        if (field.wire != 2) continue;
        if (field.number == 10000) signedHeader = field.bytes;
        if (field.number != 2 && field.number != 3) continue;
        if (proofs.size() >= maximumProofs) return std::unexpected(Error::LimitExceeded);
        auto proof = parseProof(field.number, field.bytes);
        if (!proof) return std::unexpected(proof.error());
        proofs.push_back(*proof);
    }
    Bytes identifier;
    Reader signedReader(signedHeader);
    while (!signedReader.done()) {
        Field field;
        if (!signedReader.next(field)) return std::unexpected(Error::InvalidHeader);
        if (field.number == 1 && field.wire == 2) identifier = field.bytes;
    }
    if (identifier.size() != 16) return std::unexpected(Error::InvalidHeader);
    bool developer = false;
    for (const auto& proof : proofs) {
        std::array<uint8_t, EVP_MAX_MD_SIZE> hash;
        unsigned size = 0;
        if (EVP_Digest(proof.key.data(), proof.key.size(), hash.data(), &size, EVP_sha256(), nullptr) != 1 || size != 32)
            return std::unexpected(Error::InvalidSignature);
        developer |= std::equal(identifier.begin(), identifier.end(), hash.begin());
        if (!verify(proof, signedHeader, archive)) return std::unexpected(Error::InvalidSignature);
    }
    if (!developer) return std::unexpected(Error::MissingDeveloperProof);
    std::string name;
    for (auto byte : identifier) {
        name += static_cast<char>('a' + (byte >> 4)); name += static_cast<char>('a' + (byte & 15));
    }
    return WebExtensionVerifiedCRXHaiku { 12 + headerSize, std::move(name) };
}
}
#endif
