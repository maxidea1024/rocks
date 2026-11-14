#include <Crypto/Kdf.h>

#include <Base/Endian.h>
#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "KeyAccess.h"
#include "MacInternal.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

static_assert(HkdfSha256PrkBytes == crypto_kdf_hkdf_sha256_KEYBYTES);
static_assert(HkdfSha512PrkBytes == crypto_kdf_hkdf_sha512_KEYBYTES);
static_assert(HkdfSha256MaxOutputBytes == crypto_kdf_hkdf_sha256_BYTES_MAX);
static_assert(HkdfSha512MaxOutputBytes == crypto_kdf_hkdf_sha512_BYTES_MAX);
static_assert(KdfKeyBytes == crypto_kdf_KEYBYTES && KdfContext::Bytes == crypto_kdf_CONTEXTBYTES);
static_assert(KdfMinDerivedBytes == crypto_kdf_BYTES_MIN &&
              KdfMaxDerivedBytes == crypto_kdf_BYTES_MAX);

bool IsDefined(HkdfAlgorithm algorithm) noexcept {
    return algorithm == HkdfAlgorithm::Sha256 || algorithm == HkdfAlgorithm::Sha512;
}

usize PrkBytes(HkdfAlgorithm algorithm) noexcept {
    return algorithm == HkdfAlgorithm::Sha256 ? HkdfSha256PrkBytes : HkdfSha512PrkBytes;
}

usize MaxOutputBytes(HkdfAlgorithm algorithm) noexcept {
    return algorithm == HkdfAlgorithm::Sha256 ? HkdfSha256MaxOutputBytes : HkdfSha512MaxOutputBytes;
}

void Extract(HkdfAlgorithm algorithm, Span<const uint8> salt, Span<const uint8> ikm,
             Span<uint8> prk) noexcept {
    if (algorithm == HkdfAlgorithm::Sha256) {
        detail::RequireSodium(crypto_kdf_hkdf_sha256_extract(prk.Data(), salt.Data(), salt.Size(),
                                                             ikm.Data(), ikm.Size()),
                              "Crypto: crypto_kdf_hkdf_sha256_extract failed");
    } else {
        detail::RequireSodium(crypto_kdf_hkdf_sha512_extract(prk.Data(), salt.Data(), salt.Size(),
                                                             ikm.Data(), ikm.Size()),
                              "Crypto: crypto_kdf_hkdf_sha512_extract failed");
    }
}

void Expand(HkdfAlgorithm algorithm, Span<const uint8> prk, Span<const uint8> info,
            Span<uint8> output) noexcept {
    if (output.IsEmpty()) {
        return;
    }

    const char* context = reinterpret_cast<const char*>(info.Data());
    if (algorithm == HkdfAlgorithm::Sha256) {
        detail::RequireSodium(crypto_kdf_hkdf_sha256_expand(output.Data(), output.Size(), context,
                                                            info.Size(), prk.Data()),
                              "Crypto: crypto_kdf_hkdf_sha256_expand failed");
    } else {
        detail::RequireSodium(crypto_kdf_hkdf_sha512_expand(output.Data(), output.Size(), context,
                                                            info.Size(), prk.Data()),
                              "Crypto: crypto_kdf_hkdf_sha512_expand failed");
    }
}

} // namespace

// ---- HKDF ----

Result<void> TryHkdf(HkdfAlgorithm algorithm, Span<const uint8> inputKeyMaterial,
                     Span<const uint8> salt, Span<const uint8> info, Span<uint8> output) noexcept {
    if (!IsDefined(algorithm) || output.Size() > MaxOutputBytes(algorithm) ||
        Overlaps(output, inputKeyMaterial) || Overlaps(output, salt) || Overlaps(output, info)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (output.IsEmpty()) {
        return Result<void>::Success();
    }

    detail::EnsureSodium();
    uint8 prk[HkdfSha512PrkBytes];
    const Span<uint8> prkView(prk, PrkBytes(algorithm));
    Extract(algorithm, salt, inputKeyMaterial, prkView);
    Expand(algorithm, prkView, info, output);
    sodium_memzero(prk, sizeof(prk));
    return Result<void>::Success();
}

Result<void> TryHkdfExtract(HkdfAlgorithm algorithm, Span<const uint8> salt,
                            Span<const uint8> inputKeyMaterial, Span<uint8> prk) noexcept {
    if (!IsDefined(algorithm) || prk.Size() != PrkBytes(algorithm) || Overlaps(prk, salt) ||
        Overlaps(prk, inputKeyMaterial)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    Extract(algorithm, salt, inputKeyMaterial, prk);
    return Result<void>::Success();
}

Result<void> TryHkdfExpand(HkdfAlgorithm algorithm, Span<const uint8> prk, Span<const uint8> info,
                           Span<uint8> output) noexcept {
    if (!IsDefined(algorithm) || prk.Size() != PrkBytes(algorithm) ||
        output.Size() > MaxOutputBytes(algorithm) || Overlaps(output, prk) ||
        Overlaps(output, info)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    Expand(algorithm, prk, info, output);
    return Result<void>::Success();
}

// ---- KdfKey ----

Result<KdfKey> KdfKey::TryGenerate() noexcept {
    auto key = SecretBytes::TryGenerate(KdfKeyBytes);
    if (!key) {
        return Result<KdfKey>::Failure(key.Error());
    }

    return Result<KdfKey>::Success(KdfKey(Move(key.Value())));
}

Result<KdfKey> KdfKey::TryImport(Span<const uint8> keyBytes) noexcept {
    if (keyBytes.Size() != KdfKeyBytes) {
        return Result<KdfKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryCopyFrom(keyBytes);
    if (!key) {
        return Result<KdfKey>::Failure(key.Error());
    }

    return Result<KdfKey>::Success(KdfKey(Move(key.Value())));
}

Result<void> hazmat::TryExportKey(const KdfKey& key, Span<uint8> output) noexcept {
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != KdfKeyBytes || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), KdfKeyBytes);
    return Result<void>::Success();
}

void detail::KdfContextNeedsNulTerminatedLiteral() noexcept {
}

// ---- BLAKE2b KDF ----

Result<SecretBytes> TryDeriveSecret(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                    usize bytes) noexcept {
    if (master.IsEmpty()) {
        return Result<SecretBytes>::Failure(ErrorCode::StaleHandle);
    }

    if (bytes < KdfMinDerivedBytes || bytes > KdfMaxDerivedBytes) {
        return Result<SecretBytes>::Failure(ErrorCode::InvalidArgument);
    }

    auto secret = SecretBytes::TryCreate(bytes);
    if (!secret) {
        return secret;
    }

    detail::EnsureSodium();
    detail::RequireSodium(crypto_kdf_derive_from_key(secret.Value().MutableView().Data(), bytes,
                                                     subkeyId, context.Data(),
                                                     detail::KeyAccess::Bytes(master).Data()),
                          "Crypto: crypto_kdf_derive_from_key failed");
    return secret;
}

namespace {

// 타입 파생의 BLAKE2b 입력이다. crypto_kdf와 같은 키·salt(LE64(id) ‖ 0⁸)를 쓰고, personal 뒤 8
// byte에 키 타입·알고리즘 tag를 넣는다. crypto_kdf(TryDeriveSecret)는 그 자리가 0이라 서로 겹치지
// 않는다. 이 byte는 파생 결과를 정하므로 바꾸지 않는다(바꾸면 저장된 키 계층이 모두 달라진다).
struct TypeTag {
    uint8 bytes[8];
};

constexpr TypeTag KdfKeyTag = {{'R', 'k', 'K', 'd', 'f', 'K', 'e', 'y'}};

TypeTag MacKeyTag(MacAlgorithm algorithm) noexcept {
    return TypeTag{{'R', 'k', 'M', 'a', 'c', 'K', 0x00, static_cast<uint8>(algorithm)}};
}

Result<SecretBytes> TryDeriveTyped(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                   TypeTag tag, usize bytes) noexcept {
    if (master.IsEmpty()) {
        return Result<SecretBytes>::Failure(ErrorCode::StaleHandle);
    }

    auto secret = SecretBytes::TryCreate(bytes);
    if (!secret) {
        return secret;
    }

    uint8 salt[crypto_generichash_blake2b_SALTBYTES] = {};
    StoreLittleEndian<uint64>(salt, subkeyId);
    uint8 personal[crypto_generichash_blake2b_PERSONALBYTES] = {};
    platform::MemoryCopy(personal, context.Data(), KdfContext::Bytes);
    platform::MemoryCopy(personal + KdfContext::Bytes, tag.bytes, sizeof(tag.bytes));

    detail::EnsureSodium();
    const Span<const uint8> key = detail::KeyAccess::Bytes(master);
    detail::RequireSodium(crypto_generichash_blake2b_salt_personal(
                              secret.Value().MutableView().Data(), bytes, nullptr, 0, key.Data(),
                              key.Size(), salt, personal),
                          "Crypto: crypto_generichash_blake2b_salt_personal failed");
    return secret;
}

} // namespace

Result<MacKey> TryDeriveMacKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                               MacAlgorithm algorithm) noexcept {
    if (!detail::IsDefinedMacAlgorithm(algorithm)) {
        return Result<MacKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto secret = TryDeriveTyped(master, subkeyId, context, MacKeyTag(algorithm),
                                 detail::DefaultMacKeyBytes(algorithm));
    if (!secret) {
        return Result<MacKey>::Failure(secret.Error());
    }

    return Result<MacKey>::Success(
        detail::KeyAccess::Make<MacKey>(algorithm, Move(secret.Value())));
}

Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId, KdfContext context) noexcept {
    auto secret = TryDeriveTyped(master, subkeyId, context, KdfKeyTag, KdfKeyBytes);
    if (!secret) {
        return Result<KdfKey>::Failure(secret.Error());
    }

    return Result<KdfKey>::Success(detail::KeyAccess::Make<KdfKey>(Move(secret.Value())));
}

} // namespace rocks::crypto
