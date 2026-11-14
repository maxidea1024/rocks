#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/Aead.h>
#include <Crypto/Secret.h>

// 키 교환(X25519 kx)과 공개키 봉인(sealed box)이다(Docs/Design/Crypto.ko.md 4·8절). 구현은
// libsodium crypto_kx·crypto_box_seal이다.
//
// - kx: 두 X25519 키 쌍에서 방향별 세션 키 둘(rx·tx)을 AeadKey로 얻는다. 키는 BLAKE2b-512(X25519
//   공유 비밀 ‖ client_pk ‖ server_pk)에서 나오므로 역할이 묶인다: 클라이언트 tx = 서버 rx다.
//   양쪽이 같은 역할로 착각하면 키가 맞지 않아 열리지 않는다(fail-closed).
// - 세션 키는 두 공개키에서 결정적으로 나온다. 양쪽이 장기 키 쌍이면 연결마다 같은 키가 되므로,
//   순번 봉인기(AeadSequenceSealer)처럼 nonce를 0부터 다시 세는 곳에 쓰려면 적어도 한쪽 키 쌍을
//   세션마다 새로 만든다. 상대 신원 확인(인증)은 kx가 하지 않는다. 공개키를 서명(Signature.h)이나
//   고정된 키로 따로 확인한다.
// - sealed box: 수신자 공개키만으로 봉인한다. 보내는 쪽은 익명이고(매번 새 임시 키 쌍), 출력은 임시
//   공개키 32 byte ‖ XSalsa20-Poly1305 상자(tag 16 byte ‖ 암호문)다. libsodium crypto_box_seal과
//   같은 형식이다. 보낸 사람을 확인하지 않으므로 필요하면 메시지에 서명을 넣는다.
// - 키 타입을 용도마다 나눈다(KxKeyPair·BoxKeyPair). 같은 X25519 키를 kx와 sealed box에 함께 쓰지
//   않는다. 비밀키는 키 저장소에 있고 공개키는 값 타입이다.
//
// 사용 예:
//   auto client = crypto::KxKeyPair::TryGenerate();            // 세션마다 새 키 쌍
//   // client.Value().PublicKey()를 보내고 서버 공개키 serverKey를 받는다
//   auto keys = crypto::TryClientSessionKeys(client.Value(), serverKey,
//                                            crypto::AeadAlgorithm::ChaCha20Poly1305Ietf);
//   if (!keys) { return keys.Error(); }
//   // keys.Value().transmit로 보내고 receive로 받는다
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
} // namespace detail

inline constexpr usize X25519PublicKeyBytes = 32;
inline constexpr usize X25519SecretKeyBytes = 32;
// sealed box 출력이 평문보다 긴 byte 수(임시 공개키 32 + tag 16)다.
inline constexpr usize SealedBoxOverheadBytes = 48;

// kx 공개키다. 32 byte가 아니면 TryFromBytes가 InvalidArgument다. 점이 올바른지(small-order가
// 아닌지)는 세션 키를 만들 때 본다.
struct KxPublicKey {
    uint8 bytes[X25519PublicKeyBytes] = {};

    [[nodiscard]] static Result<KxPublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const KxPublicKey& other) const noexcept = default;
};

// sealed box 수신자 공개키다. 규칙은 KxPublicKey와 같다.
struct BoxPublicKey {
    uint8 bytes[X25519PublicKeyBytes] = {};

    [[nodiscard]] static Result<BoxPublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const BoxPublicKey& other) const noexcept = default;
};

// kx X25519 키 쌍이다. move-only이며 생성 뒤 바뀌지 않는다.
class KxKeyPair {
public:
    KxKeyPair() noexcept = default;

    // OS 난수로 만든다. OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<KxKeyPair> TryGenerate() noexcept;

    // X25519 비밀키 32 byte에서 만든다(공개키를 계산한다). 길이가 다르면 InvalidArgument다.
    // secretKey는 호출자가 지운다.
    [[nodiscard]] static Result<KxKeyPair> TryImport(Span<const uint8> secretKey) noexcept;

    // 빈 키 쌍은 모두 0이다.
    [[nodiscard]] KxPublicKey PublicKey() const noexcept {
        return publicKey_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    KxKeyPair(SecretBytes&& key, const uint8* publicKey) noexcept;

    SecretBytes key_;
    KxPublicKey publicKey_;
};

// sealed box 수신자 X25519 키 쌍이다. 만들기 규칙은 KxKeyPair와 같다.
class BoxKeyPair {
public:
    BoxKeyPair() noexcept = default;

    [[nodiscard]] static Result<BoxKeyPair> TryGenerate() noexcept;
    [[nodiscard]] static Result<BoxKeyPair> TryImport(Span<const uint8> secretKey) noexcept;

    [[nodiscard]] BoxPublicKey PublicKey() const noexcept {
        return publicKey_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    BoxKeyPair(SecretBytes&& key, const uint8* publicKey) noexcept;

    SecretBytes key_;
    BoxPublicKey publicKey_;
};

// 방향별 세션 키다. receive로 상대가 보낸 것을 열고 transmit로 보낸다.
struct KxSessionKeys {
    AeadKey receive;
    AeadKey transmit;
};

// 클라이언트 쪽 세션 키다. algorithm은 키가 32 byte인 AEAD(XChaCha20-Poly1305, ChaCha20-Poly1305-
// IETF, AES-256-GCM, AEGIS-256)여야 한다. 키 byte는 libsodium crypto_kx와 같다.
// - 빈 키 쌍은 StaleHandle, 열거값이 아니거나 키가 32 byte가 아닌 알고리즘은 InvalidArgument, 이
//   기기에서 쓸 수 없는 알고리즘은 Unsupported다.
// - 상대 공개키가 올바른 점이 아니면(small-order 등, 공유 비밀이 0) AuthenticationFailed다.
// - 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<KxSessionKeys> TryClientSessionKeys(const KxKeyPair& client,
                                                         const KxPublicKey& server,
                                                         AeadAlgorithm algorithm) noexcept;

// 서버 쪽 세션 키다. 인자와 오류는 TryClientSessionKeys와 같다. 서버 transmit = 클라이언트
// receive다.
[[nodiscard]] Result<KxSessionKeys> TryServerSessionKeys(const KxKeyPair& server,
                                                         const KxPublicKey& client,
                                                         AeadAlgorithm algorithm) noexcept;

// sealed box 출력 길이(messageBytes + 48)다. 넘치면 Overflow다.
[[nodiscard]] Result<usize> TryGetSealedBoxBytes(usize messageBytes) noexcept;

// recipient에게 message를 봉인한다. 쓴 byte 수(message + 48)를 돌려준다.
// - output이 작거나 message가 상한을 넘으면 CapacityExceeded다.
// - 제자리: message.Data() == output.Data() + 32일 때만 겹침을 허용하고, 그 밖의 겹침은
//   InvalidArgument다.
// - recipient가 올바른 점이 아니면 AuthenticationFailed이고 출력을 지운다.
// - 임시 키 쌍은 libsodium 난수 경로로 만들며, OS 난수가 실패하면 Fatal이다(11절).
[[nodiscard]] Result<usize> TrySealToRecipient(const BoxPublicKey& recipient,
                                               Span<const uint8> message,
                                               Span<uint8> output) noexcept;

// TrySealToRecipient의 출력을 연다. 평문 byte 수를 돌려준다.
// - sealed가 48 byte보다 짧거나, 변조되었거나, 다른 수신자의 것이면 AuthenticationFailed이고
//   plaintext 전체를 지운다.
// - plaintext가 평문 길이보다 작으면 CapacityExceeded다(쓰지 않는다).
// - 제자리: plaintext.Data() == sealed.Data() + 32일 때만 겹침을 허용한다. 빈 키 쌍은
//   StaleHandle이다.
[[nodiscard]] Result<usize> TryOpenSealed(const BoxKeyPair& recipient, Span<const uint8> sealed,
                                          Span<uint8> plaintext) noexcept;

namespace hazmat {

// X25519 비밀키(32 byte)를 평문으로 output에 복사한다(10.4절). output.Size()는 32여야 한다.
[[nodiscard]] Result<void> TryExportKey(const KxKeyPair& key, Span<uint8> output) noexcept;
[[nodiscard]] Result<void> TryExportKey(const BoxKeyPair& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
