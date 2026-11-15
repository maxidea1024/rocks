#include <Crypto/KeyExchange.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "AeadInternal.h"
#include "KeyAccess.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

static_assert(X25519PublicKeyBytes == crypto_kx_PUBLICKEYBYTES &&
              X25519PublicKeyBytes == crypto_box_PUBLICKEYBYTES &&
              X25519PublicKeyBytes == crypto_scalarmult_BYTES);
static_assert(X25519SecretKeyBytes == crypto_kx_SECRETKEYBYTES &&
              X25519SecretKeyBytes == crypto_box_SECRETKEYBYTES &&
              X25519SecretKeyBytes == crypto_scalarmult_SCALARBYTES);
static_assert(SealedBoxOverheadBytes == crypto_box_SEALBYTES);

constexpr usize SessionKeyBytes = crypto_kx_SESSIONKEYBYTES;
constexpr usize EphemeralKeyBytes = crypto_box_PUBLICKEYBYTES;
// sealed box 평문 상한이다. 출력 길이(평문 + 48)가 usize를 넘지 않게도 막는다.
constexpr usize MaxSealedBoxMessageBytes =
    crypto_box_MESSAGEBYTES_MAX < MaxSize - SealedBoxOverheadBytes
        ? crypto_box_MESSAGEBYTES_MAX
        : MaxSize - SealedBoxOverheadBytes;

// X25519 키 쌍의 비밀키(키 저장소)와 공개키다.
struct X25519Parts {
    SecretBytes key;
    uint8 publicKey[X25519PublicKeyBytes];
};

// 키 저장소의 비밀키로 공개키를 계산한다.
Result<X25519Parts> TryFinishX25519(Result<SecretBytes>&& key) noexcept {
    if (!key) {
        return Result<X25519Parts>::Failure(key.Error());
    }

    X25519Parts parts{Move(key.Value()), {}};
    detail::EnsureSodium();
    // clamp한 스칼라는 0이 아니므로 기저점 곱은 실패하지 않는다.
    detail::RequireSodium(crypto_scalarmult_base(parts.publicKey, parts.key.View().Data()),
                          "Crypto: crypto_scalarmult_base failed");
    return Result<X25519Parts>::Success(Move(parts));
}

// 비밀키를 키 저장소로 복사하고 공개키를 계산한다. 길이가 다르면 InvalidArgument다.
Result<X25519Parts> TryMakeX25519(Span<const uint8> secretKey) noexcept {
    if (secretKey.Size() != X25519SecretKeyBytes) {
        return Result<X25519Parts>::Failure(ErrorCode::InvalidArgument);
    }

    return TryFinishX25519(SecretBytes::TryCopyFrom(secretKey));
}

// OS 난수 비밀키로 만든다(crypto_kx_keypair·crypto_box_keypair와 같은 방식). 비밀키를 키 저장소에
// 바로 만들어 스택 사본을 남기지 않는다.
Result<X25519Parts> TryGenerateX25519() noexcept {
    return TryFinishX25519(SecretBytes::TryGenerate(X25519SecretKeyBytes));
}

template <typename PublicKey>
Result<PublicKey> TryPublicKeyFromBytes(Span<const uint8> bytes) noexcept {
    if (bytes.Size() != X25519PublicKeyBytes) {
        return Result<PublicKey>::Failure(ErrorCode::InvalidArgument);
    }

    PublicKey key;
    platform::MemoryCopy(key.bytes, bytes.Data(), X25519PublicKeyBytes);
    return Result<PublicKey>::Success(key);
}

template <typename KeyPair>
Result<KeyPair> TryKeyPairFrom(Result<X25519Parts>&& parts) noexcept {
    if (!parts) {
        return Result<KeyPair>::Failure(parts.Error());
    }

    return Result<KeyPair>::Success(
        detail::KeyAccess::Make<KeyPair>(Move(parts.Value().key), parts.Value().publicKey));
}

template <typename KeyPair>
Result<void> TryExportSecret(const KeyPair& key, Span<uint8> output) noexcept {
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != bytes.Size() || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), bytes.Size());
    return Result<void>::Success();
}

// 오류 순서: 빈 키 쌍 StaleHandle → 알고리즘(InvalidArgument·Unsupported) → 키 길이 → 상대 공개키.
Result<KxSessionKeys> TrySessionKeys(bool client, const KxKeyPair& self, const KxPublicKey& peer,
                                     AeadAlgorithm algorithm) noexcept {
    const Span<const uint8> secretKey = detail::KeyAccess::Bytes(self);
    if (secretKey.IsEmpty()) {
        return Result<KxSessionKeys>::Failure(ErrorCode::StaleHandle);
    }

    // 열거값·키 길이를 HW 사용 가능 여부보다 먼저 본다(정적 오용은 기기와 관계없이
    // InvalidArgument).
    auto info = TryGetAeadInfo(algorithm);
    if (!info || info.Value().keyBytes != SessionKeyBytes) {
        return Result<KxSessionKeys>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto usable = detail::CheckAeadUsable(algorithm); !usable) {
        return Result<KxSessionKeys>::Failure(usable.Error());
    }

    auto receive = SecretBytes::TryCreate(SessionKeyBytes);
    auto transmit = SecretBytes::TryCreate(SessionKeyBytes);
    if (!receive || !transmit) {
        return Result<KxSessionKeys>::Failure(!receive ? receive.Error() : transmit.Error());
    }

    const KxPublicKey selfPublic = self.PublicKey();
    uint8* rx = receive.Value().MutableView().Data();
    uint8* tx = transmit.Value().MutableView().Data();
    const int status =
        client
            ? crypto_kx_client_session_keys(rx, tx, selfPublic.bytes, secretKey.Data(), peer.bytes)
            : crypto_kx_server_session_keys(rx, tx, selfPublic.bytes, secretKey.Data(), peer.bytes);
    // 상대 공개키가 small-order 점이면 공유 비밀이 0이라 libsodium이 거부한다. 상대가 보낸 값이므로
    // AuthenticationFailed다(11절 "잘못된 키").
    if (status != 0) {
        return Result<KxSessionKeys>::Failure(ErrorCode::AuthenticationFailed);
    }

    auto receiveKey = detail::TryMakeAeadKey(algorithm, Move(receive.Value()));
    if (!receiveKey) {
        return Result<KxSessionKeys>::Failure(receiveKey.Error());
    }

    auto transmitKey = detail::TryMakeAeadKey(algorithm, Move(transmit.Value()));
    if (!transmitKey) {
        return Result<KxSessionKeys>::Failure(transmitKey.Error());
    }

    return Result<KxSessionKeys>::Success(
        KxSessionKeys{Move(receiveKey.Value()), Move(transmitKey.Value())});
}

} // namespace

// ---- 공개키·키 쌍 ----

Result<KxPublicKey> KxPublicKey::TryFromBytes(Span<const uint8> bytes) noexcept {
    return TryPublicKeyFromBytes<KxPublicKey>(bytes);
}

Result<BoxPublicKey> BoxPublicKey::TryFromBytes(Span<const uint8> bytes) noexcept {
    return TryPublicKeyFromBytes<BoxPublicKey>(bytes);
}

KxKeyPair::KxKeyPair(SecretBytes&& key, const uint8* publicKey) noexcept : key_(Move(key)) {
    platform::MemoryCopy(publicKey_.bytes, publicKey, X25519PublicKeyBytes);
}

Result<KxKeyPair> KxKeyPair::TryGenerate() noexcept {
    platform::DataIndependentTimingScope dit;
    return TryKeyPairFrom<KxKeyPair>(TryGenerateX25519());
}

Result<KxKeyPair> KxKeyPair::TryImport(Span<const uint8> secretKey) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryKeyPairFrom<KxKeyPair>(TryMakeX25519(secretKey));
}

BoxKeyPair::BoxKeyPair(SecretBytes&& key, const uint8* publicKey) noexcept : key_(Move(key)) {
    platform::MemoryCopy(publicKey_.bytes, publicKey, X25519PublicKeyBytes);
}

Result<BoxKeyPair> BoxKeyPair::TryGenerate() noexcept {
    platform::DataIndependentTimingScope dit;
    return TryKeyPairFrom<BoxKeyPair>(TryGenerateX25519());
}

Result<BoxKeyPair> BoxKeyPair::TryImport(Span<const uint8> secretKey) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryKeyPairFrom<BoxKeyPair>(TryMakeX25519(secretKey));
}

Result<void> hazmat::TryExportKey(const KxKeyPair& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryExportSecret(key, output);
}

Result<void> hazmat::TryExportKey(const BoxKeyPair& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryExportSecret(key, output);
}

// ---- kx ----

Result<KxSessionKeys> TryClientSessionKeys(const KxKeyPair& client, const KxPublicKey& server,
                                           AeadAlgorithm algorithm) noexcept {
    platform::DataIndependentTimingScope dit;
    return TrySessionKeys(true, client, server, algorithm);
}

Result<KxSessionKeys> TryServerSessionKeys(const KxKeyPair& server, const KxPublicKey& client,
                                           AeadAlgorithm algorithm) noexcept {
    platform::DataIndependentTimingScope dit;
    return TrySessionKeys(false, server, client, algorithm);
}

// ---- sealed box ----

Result<usize> TryGetSealedBoxBytes(usize messageBytes) noexcept {
    if (messageBytes > MaxSize - SealedBoxOverheadBytes) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(messageBytes + SealedBoxOverheadBytes);
}

Result<usize> TrySealToRecipient(const BoxPublicKey& recipient, Span<const uint8> message,
                                 Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (message.Size() > MaxSealedBoxMessageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize sealedBytes = message.Size() + SealedBoxOverheadBytes;
    if (output.Size() < sealedBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const Span<uint8> sealed = output.Left(sealedBytes);
    const bool inPlace = detail::StartsAt(message.Data(), sealed.Data(), EphemeralKeyBytes);
    if (Overlaps(message, sealed) && !inPlace) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    // 수신자 공개키가 small-order 점이면 공유 비밀이 0이라 libsodium이 거부한다. 이때 libsodium은
    // 임시 공개키 32 byte만 쓰므로 그 자리만 지운다(제자리 평문은 그대로 둔다).
    if (crypto_box_seal(sealed.Data(), detail::InputPointer(message), message.Size(),
                        recipient.bytes) != 0) {
        SecureZero(sealed.Left(EphemeralKeyBytes));
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(sealedBytes);
}

Result<usize> TryOpenSealed(const BoxKeyPair& recipient, Span<const uint8> sealed,
                            Span<uint8> plaintext) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> secretKey = detail::KeyAccess::Bytes(recipient);
    if (secretKey.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    // 겹침은 길이 검사 전에 본다. 실패 때 plaintext를 지우므로 입력을 덮지 않게 한다.
    const bool inPlace = detail::StartsAt(plaintext.Data(), sealed.Data(), EphemeralKeyBytes);
    if (Overlaps(sealed, plaintext) && !inPlace) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    if (sealed.Size() < SealedBoxOverheadBytes) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    const usize messageBytes = sealed.Size() - SealedBoxOverheadBytes;
    if (plaintext.Size() < messageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    detail::EnsureSodium();
    uint8 scratch = 0;
    const BoxPublicKey publicKey = recipient.PublicKey();
    if (crypto_box_seal_open(detail::OutputPointer(plaintext.Left(messageBytes), &scratch),
                             sealed.Data(), sealed.Size(), publicKey.bytes,
                             secretKey.Data()) != 0) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(messageBytes);
}

} // namespace rocks::crypto
