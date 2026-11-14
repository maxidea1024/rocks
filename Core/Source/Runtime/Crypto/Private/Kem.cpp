#include <Crypto/Kem.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "KeyAccess.h"
#include "SecretArena.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

static_assert(KemMaxPublicKeyBytes == crypto_kem_xwing_PUBLICKEYBYTES &&
              crypto_kem_mlkem768_PUBLICKEYBYTES <= KemMaxPublicKeyBytes);
static_assert(KemMaxCiphertextBytes == crypto_kem_xwing_CIPHERTEXTBYTES &&
              crypto_kem_mlkem768_CIPHERTEXTBYTES <= KemMaxCiphertextBytes);
static_assert(KemMaxSeedBytes == crypto_kem_mlkem768_SEEDBYTES &&
              crypto_kem_xwing_SEEDBYTES <= KemMaxSeedBytes);
static_assert(KemSharedSecretBytes == crypto_kem_xwing_SHAREDSECRETBYTES &&
              KemSharedSecretBytes == crypto_kem_mlkem768_SHAREDSECRETBYTES &&
              KemSharedSecretBytes == KdfKeyBytes);
// X-Wing의 libsodium 비밀키는 seed 자체다. ML-KEM 확장 비밀키는 캡슐 해제 때 키 저장소 임시 슬롯에
// 펼친다.
static_assert(crypto_kem_xwing_SECRETKEYBYTES == crypto_kem_xwing_SEEDBYTES);
static_assert(crypto_kem_mlkem768_SECRETKEYBYTES <= detail::SecretSlotMaxBytes);

struct Traits {
    usize publicKeyBytes;
    usize ciphertextBytes;
    usize seedBytes;
};

bool IsDefined(KemAlgorithm algorithm) noexcept {
    return algorithm == KemAlgorithm::XWing || algorithm == KemAlgorithm::MlKem768;
}

// 정의된 알고리즘만 넘긴다.
Traits TraitsOf(KemAlgorithm algorithm) noexcept {
    if (algorithm == KemAlgorithm::XWing) {
        return {crypto_kem_xwing_PUBLICKEYBYTES, crypto_kem_xwing_CIPHERTEXTBYTES,
                crypto_kem_xwing_SEEDBYTES};
    }

    return {crypto_kem_mlkem768_PUBLICKEYBYTES, crypto_kem_mlkem768_CIPHERTEXTBYTES,
            crypto_kem_mlkem768_SEEDBYTES};
}

// ML-KEM seed를 확장 비밀키로 펼친다. 비밀키는 키 저장소 슬롯이고 공개키는 publicKey에 쓴다.
Result<SecretBytes> TryExpandMlKem(Span<const uint8> seed, uint8* publicKey) noexcept {
    auto secretKey = SecretBytes::TryCreate(crypto_kem_mlkem768_SECRETKEYBYTES);
    if (!secretKey) {
        return secretKey;
    }

    detail::RequireSodium(crypto_kem_mlkem768_seed_keypair(
                              publicKey, secretKey.Value().MutableView().Data(), seed.Data()),
                          "Crypto: crypto_kem_mlkem768_seed_keypair failed");
    return secretKey;
}

// seed에서 키 쌍을 만든다. 알고리즘과 seed 길이는 호출자가 검사했다.
Result<KemKeyPair> TryMakeFromSeed(KemAlgorithm algorithm, Span<const uint8> seed) noexcept {
    const Traits traits = TraitsOf(algorithm);
    auto seedCopy = SecretBytes::TryCopyFrom(seed);
    if (!seedCopy) {
        return Result<KemKeyPair>::Failure(seedCopy.Error());
    }

    uint8 publicKey[KemMaxPublicKeyBytes];
    detail::EnsureSodium();
    if (algorithm == KemAlgorithm::XWing) {
        // X-Wing 비밀키는 seed라 따로 둘 것이 없다. 공개키만 계산하고 출력 비밀키(= seed 사본)는 키
        // 저장소 임시 슬롯에 받아 버린다.
        auto secretKey = SecretBytes::TryCreate(crypto_kem_xwing_SECRETKEYBYTES);
        if (!secretKey) {
            return Result<KemKeyPair>::Failure(secretKey.Error());
        }

        detail::RequireSodium(crypto_kem_xwing_seed_keypair(
                                  publicKey, secretKey.Value().MutableView().Data(), seed.Data()),
                              "Crypto: crypto_kem_xwing_seed_keypair failed");
    } else {
        auto secretKey = TryExpandMlKem(seed, publicKey);
        if (!secretKey) {
            return Result<KemKeyPair>::Failure(secretKey.Error());
        }
    }

    auto key =
        KemPublicKey::TryFromBytes(algorithm, Span<const uint8>(publicKey, traits.publicKeyBytes));
    return Result<KemKeyPair>::Success(
        detail::KeyAccess::Make<KemKeyPair>(Move(seedCopy.Value()), key.Value()));
}

// 공유 비밀을 담을 KdfKey 자리다.
Result<SecretBytes> TryCreateSharedSecret() noexcept {
    return SecretBytes::TryCreate(KemSharedSecretBytes);
}

} // namespace

Result<KemInfo> TryGetKemInfo(KemAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<KemInfo>::Failure(ErrorCode::InvalidArgument);
    }

    const Traits traits = TraitsOf(algorithm);
    KemInfo info;
    info.publicKeyBytes = traits.publicKeyBytes;
    info.ciphertextBytes = traits.ciphertextBytes;
    info.seedBytes = traits.seedBytes;
    return Result<KemInfo>::Success(info);
}

KemAlgorithm GetPreferredKem() noexcept {
    return KemAlgorithm::XWing;
}

// ---- 공개키·키 쌍 ----

Result<KemPublicKey> KemPublicKey::TryFromBytes(KemAlgorithm algorithm,
                                                Span<const uint8> bytes) noexcept {
    if (!IsDefined(algorithm) || bytes.Size() != TraitsOf(algorithm).publicKeyBytes) {
        return Result<KemPublicKey>::Failure(ErrorCode::InvalidArgument);
    }

    KemPublicKey key;
    key.algorithm_ = algorithm;
    key.size_ = bytes.Size();
    platform::MemoryCopy(key.bytes_, bytes.Data(), bytes.Size());
    return Result<KemPublicKey>::Success(key);
}

KemKeyPair::KemKeyPair(SecretBytes&& seed, const KemPublicKey& publicKey) noexcept
    : key_(Move(seed)), publicKey_(publicKey) {
}

Result<KemKeyPair> KemKeyPair::TryGenerate(KemAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<KemKeyPair>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 seed[KemMaxSeedBytes];
    const Span<uint8> seedView = Span<uint8>(seed).Left(TraitsOf(algorithm).seedBytes);
    if (auto filled = TryFillRandom(seedView); !filled) {
        return Result<KemKeyPair>::Failure(filled.Error());
    }

    auto key = TryMakeFromSeed(algorithm, seedView);
    SecureZero(seedView);
    return key;
}

Result<KemKeyPair> KemKeyPair::TryFromSeed(KemAlgorithm algorithm,
                                           Span<const uint8> seed) noexcept {
    if (!IsDefined(algorithm) || seed.Size() != TraitsOf(algorithm).seedBytes) {
        return Result<KemKeyPair>::Failure(ErrorCode::InvalidArgument);
    }

    return TryMakeFromSeed(algorithm, seed);
}

Result<void> hazmat::TryExportSeed(const KemKeyPair& key, Span<uint8> output) noexcept {
    const Span<const uint8> seed = detail::KeyAccess::Bytes(key);
    if (seed.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != seed.Size() || Overlaps(output, seed)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), seed.Data(), seed.Size());
    return Result<void>::Success();
}

// ---- 캡슐화 ----

Result<KdfKey> TryEncapsulate(const KemPublicKey& recipient, Span<uint8> ciphertext) noexcept {
    if (recipient.IsEmpty() ||
        ciphertext.Size() != TraitsOf(recipient.Algorithm()).ciphertextBytes) {
        return Result<KdfKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto shared = TryCreateSharedSecret();
    if (!shared) {
        return Result<KdfKey>::Failure(shared.Error());
    }

    detail::EnsureSodium();
    uint8* ss = shared.Value().MutableView().Data();
    const uint8* pk = recipient.View().Data();
    const int status = recipient.Algorithm() == KemAlgorithm::XWing
                           ? crypto_kem_xwing_enc(ciphertext.Data(), ss, pk)
                           : crypto_kem_mlkem768_enc(ciphertext.Data(), ss, pk);
    // 공개키가 올바르지 않다(ML-KEM 계수 범위, X25519 small-order). 상대가 준 키이므로
    // AuthenticationFailed다(11절 "상대에게서 받은 공개키").
    if (status != 0) {
        SecureZero(ciphertext);
        return Result<KdfKey>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<KdfKey>::Success(detail::KeyAccess::Make<KdfKey>(Move(shared.Value())));
}

Result<KdfKey> TryDecapsulate(const KemKeyPair& recipient, Span<const uint8> ciphertext) noexcept {
    const Span<const uint8> seed = detail::KeyAccess::Bytes(recipient);
    if (seed.IsEmpty()) {
        return Result<KdfKey>::Failure(ErrorCode::StaleHandle);
    }

    const KemAlgorithm algorithm = recipient.Algorithm();
    // 암호문 길이는 메시지에서 온 값이다(결정 D5).
    if (ciphertext.Size() != TraitsOf(algorithm).ciphertextBytes) {
        return Result<KdfKey>::Failure(ErrorCode::AuthenticationFailed);
    }

    auto shared = TryCreateSharedSecret();
    if (!shared) {
        return Result<KdfKey>::Failure(shared.Error());
    }

    detail::EnsureSodium();
    uint8* ss = shared.Value().MutableView().Data();
    int status = 0;
    if (algorithm == KemAlgorithm::XWing) {
        status = crypto_kem_xwing_dec(ss, ciphertext.Data(), seed.Data());
    } else {
        uint8 publicKey[crypto_kem_mlkem768_PUBLICKEYBYTES];
        auto secretKey = TryExpandMlKem(seed, publicKey);
        if (!secretKey) {
            return Result<KdfKey>::Failure(secretKey.Error());
        }

        status = crypto_kem_mlkem768_dec(ss, ciphertext.Data(), secretKey.Value().View().Data());
    }

    if (status != 0) {
        return Result<KdfKey>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<KdfKey>::Success(detail::KeyAccess::Make<KdfKey>(Move(shared.Value())));
}

} // namespace rocks::crypto
