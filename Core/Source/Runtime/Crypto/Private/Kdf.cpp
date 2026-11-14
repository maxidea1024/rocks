#include <Crypto/Kdf.h>

#include <Base/Endian.h>
#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "KdfInternal.h"
#include "KeyAccess.h"
#include "MacInternal.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/Fatal.h>
#include <Platform/DataIndependentTiming.h>
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
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
    auto key = SecretBytes::TryGenerate(KdfKeyBytes);
    if (!key) {
        return Result<KdfKey>::Failure(key.Error());
    }

    return Result<KdfKey>::Success(KdfKey(Move(key.Value())));
}

Result<KdfKey> KdfKey::TryImport(Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
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

Result<SecretBytes> detail::TryDeriveTypedSecret(const KdfKey& master, uint64 subkeyId,
                                                 KdfContext context, KdfTypeTag tag,
                                                 usize bytes) noexcept {
    platform::DataIndependentTimingScope dit;
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

Result<MacKey> TryDeriveMacKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                               MacAlgorithm algorithm) noexcept {
    platform::DataIndependentTimingScope dit;
    if (!detail::IsDefinedMacAlgorithm(algorithm)) {
        return Result<MacKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto secret = detail::TryDeriveTypedSecret(master, subkeyId, context,
                                               detail::MacKeyTypeTag(static_cast<uint8>(algorithm)),
                                               detail::DefaultMacKeyBytes(algorithm));
    if (!secret) {
        return Result<MacKey>::Failure(secret.Error());
    }

    return Result<MacKey>::Success(
        detail::KeyAccess::Make<MacKey>(algorithm, Move(secret.Value())));
}

Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId, KdfContext context) noexcept {
    platform::DataIndependentTimingScope dit;
    auto secret =
        detail::TryDeriveTypedSecret(master, subkeyId, context, detail::KdfKeyTypeTag, KdfKeyBytes);
    if (!secret) {
        return Result<KdfKey>::Failure(secret.Error());
    }

    return Result<KdfKey>::Success(detail::KeyAccess::Make<KdfKey>(Move(secret.Value())));
}

Result<KdfKey> TryDeriveBoundKdfKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                    Span<const Span<const uint8>> transcript) noexcept {
    platform::DataIndependentTimingScope dit;
    if (master.IsEmpty()) {
        return Result<KdfKey>::Failure(ErrorCode::StaleHandle);
    }

    auto secret = SecretBytes::TryCreate(KdfKeyBytes);
    if (!secret) {
        return Result<KdfKey>::Failure(secret.Error());
    }

    // 타입 파생과 같은 key·salt·personal 배치에 transcript를 BLAKE2b 입력으로 넣는다. 상태는 키에서
    // 나온 비밀이라 끝나면 지운다.
    uint8 salt[crypto_generichash_blake2b_SALTBYTES] = {};
    StoreLittleEndian<uint64>(salt, subkeyId);
    uint8 personal[crypto_generichash_blake2b_PERSONALBYTES] = {};
    platform::MemoryCopy(personal, context.Data(), KdfContext::Bytes);
    platform::MemoryCopy(personal + KdfContext::Bytes, detail::KdfBoundTypeTag.bytes,
                         sizeof(detail::KdfBoundTypeTag.bytes));

    detail::EnsureSodium();
    const Span<const uint8> key = detail::KeyAccess::Bytes(master);
    crypto_generichash_blake2b_state state;
    detail::RequireSodium(crypto_generichash_blake2b_init_salt_personal(
                              &state, key.Data(), key.Size(), KdfKeyBytes, salt, personal),
                          "Crypto: crypto_generichash_blake2b_init_salt_personal failed");
    for (Span<const uint8> piece : transcript) {
        uint8 length[8];
        StoreLittleEndian<uint64>(length, static_cast<uint64>(piece.Size()));
        detail::RequireSodium(crypto_generichash_blake2b_update(&state, length, sizeof(length)),
                              "Crypto: crypto_generichash_blake2b_update failed");
        if (!piece.IsEmpty()) {
            detail::RequireSodium(
                crypto_generichash_blake2b_update(&state, piece.Data(), piece.Size()),
                "Crypto: crypto_generichash_blake2b_update failed");
        }
    }

    detail::RequireSodium(
        crypto_generichash_blake2b_final(&state, secret.Value().MutableView().Data(), KdfKeyBytes),
        "Crypto: crypto_generichash_blake2b_final failed");
    SecureZero(Span<uint8>(reinterpret_cast<uint8*>(&state), sizeof(state)));
    return Result<KdfKey>::Success(detail::KeyAccess::Make<KdfKey>(Move(secret.Value())));
}

} // namespace rocks::crypto
