#include <Crypto/Kdf.h>

#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "KeyAccess.h"
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

Result<void> KdfKey::TryExport(Span<uint8> output) const noexcept {
    if (key_.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != KdfKeyBytes || Overlaps(output, key_.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), key_.View().Data(), KdfKeyBytes);
    return Result<void>::Success();
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

Result<MacKey> TryDeriveMacKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                               MacAlgorithm algorithm) noexcept {
    // 권장 키 길이(HMAC-SHA-512 64, 나머지 32 byte)로 뽑는다. 열거값이 아니면 아래 TryImport가
    // InvalidArgument를 돌려준다.
    const usize bytes = algorithm == MacAlgorithm::HmacSha512 ? 64 : 32;
    auto secret = TryDeriveSecret(master, subkeyId, context, bytes);
    if (!secret) {
        return Result<MacKey>::Failure(secret.Error());
    }

    // 중간 SecretBytes는 소멸하며 지워진다.
    return MacKey::TryImport(algorithm, secret.Value().View());
}

Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId, KdfContext context) noexcept {
    auto secret = TryDeriveSecret(master, subkeyId, context, KdfKeyBytes);
    if (!secret) {
        return Result<KdfKey>::Failure(secret.Error());
    }

    return Result<KdfKey>::Success(KdfKey(Move(secret.Value())));
}

} // namespace rocks::crypto
