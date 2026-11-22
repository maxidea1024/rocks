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
// - 세션 난수가 없다: 같은 두 키 쌍이면 늘 같은 세션 키다. 그래서 카운터
//   nonce(AeadSequenceSealer)로 봉인하는 쪽은 자기 키 쌍을 세션마다 새로 만든다. 상대만 새 키
//   쌍이면 공격자가 상대의 이전 공개키와 레코드를 재전송해 같은 세션 키를 다시 만들게 할 수 있고,
//   장기 키 쪽 봉인이 seq 0부터 같은 nonce를 다시 쓴다. 장기 키 쪽은 무작위 nonce AEAD(TrySeal)만
//   쓰고, 상대의 첫 메시지는 재전송될 수 있다고 본다(0-RTT와 같다).
// - kx는 인증하지 않는다. 상대 장기 공개키를 고정하면 암묵적 인증은 되지만 key confirmation은 없고,
//   장기 비밀키가 새면 지난 세션이 모두 풀린다(전방 비밀성 없음). 양쪽이 장기 키면 KCI에도
//   노출된다. 신원은 장기 Ed25519 키가 세션 공개키와 맥락(양쪽 공개키·역할)을 함께 서명해
//   확인한다(Signature.h).
// - 알고리즘은 키 byte에 묶이지 않는다. 같은 키 쌍·상대로 알고리즘만 바꿔 부르면 같은 byte가 두
//   알고리즘에 쓰이므로(2절의 예외) 프로토콜이 알고리즘을 고정하거나 협상을 인증한다.
// - sealed box: 수신자 공개키만으로 봉인한다. 보내는 쪽은 익명이고(매번 새 임시 키 쌍), 출력은 임시
//   공개키 32 byte ‖ XSalsa20-Poly1305 상자(tag 16 byte ‖ 암호문)다. libsodium crypto_box_seal과
//   같은 형식이다. 보낸 사람을 확인해야 하면 메시지에 서명을 넣되, 서명 대상에 수신자 공개키를
//   포함한다(그래야 받은 쪽이 서명된 메시지를 다른 수신자에게 다시 봉인해 넘기는 surreptitious
//   forwarding을 막는다).
// - 키 타입을 용도마다 나눈다(KxKeyPair·BoxKeyPair). 같은 X25519 키를 kx와 sealed box에 함께 쓰지
//   않는다(2절 "용도가 다르면 따로 파생", 수명·회전 정책 분리, KeyRing·KeyBlob 타입 tag와 정합).
//   byte 형식은 libsodium kx·box 키 쌍과 같다. 비밀키는 키 저장소에 있고 공개키는 값 타입이다.
//
// 사용 예(양쪽 모두 세션마다 새 키 쌍, 신원은 장기 서명 키로 확인):
//   auto client = crypto::KxKeyPair::TryGenerate();
//   // client 공개키를 보내고, 서버의 세션 공개키 serverKey와 그 서명을 받아 확인한다
//   auto keys = crypto::TryClientSessionKeys(client.Value(), serverKey,
//                                            crypto::GetPreferredAead());
//   if (!keys) { return keys.Error(); }
//   auto written = crypto::TrySeal(keys.Value().transmit, header, payload, record);
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
struct ROCKS_CRYPTO_API KxPublicKey {
    uint8 bytes[X25519PublicKeyBytes] = {};

    [[nodiscard]] static Result<KxPublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const KxPublicKey& other) const noexcept = default;
};

// sealed box 수신자 공개키다. 규칙은 KxPublicKey와 같다.
struct ROCKS_CRYPTO_API BoxPublicKey {
    uint8 bytes[X25519PublicKeyBytes] = {};

    [[nodiscard]] static Result<BoxPublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const BoxPublicKey& other) const noexcept = default;
};

// kx X25519 키 쌍이다. move-only이며 생성 뒤 바뀌지 않는다.
class ROCKS_CRYPTO_API KxKeyPair {
public:
    KxKeyPair() noexcept = default;

    // OS 난수로 만든다. OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<KxKeyPair> TryGenerate() noexcept;

    // X25519 비밀키 32 byte에서 만든다(공개키를 계산한다). 길이가 다르면 InvalidArgument다.
    // secretKey는 호출자가 지운다.
    [[nodiscard]] static Result<KxKeyPair> TryImport(Span<const uint8> secretKey) noexcept;

    // 빈 키 쌍(기본 생성·옮겨 간 객체)은 모두 0이다.
    [[nodiscard]] KxPublicKey PublicKey() const noexcept {
        return key_.IsEmpty() ? KxPublicKey() : publicKey_;
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
class ROCKS_CRYPTO_API BoxKeyPair {
public:
    BoxKeyPair() noexcept = default;

    [[nodiscard]] static Result<BoxKeyPair> TryGenerate() noexcept;
    [[nodiscard]] static Result<BoxKeyPair> TryImport(Span<const uint8> secretKey) noexcept;

    // 빈 키 쌍(기본 생성·옮겨 간 객체)은 모두 0이다.
    [[nodiscard]] BoxPublicKey PublicKey() const noexcept {
        return key_.IsEmpty() ? BoxPublicKey() : publicKey_;
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
// IETF, AES-256-GCM, AEGIS-256)여야 한다. 키 byte는 libsodium crypto_kx와 같다. 16 byte 키
// 알고리즘을 받지 않는 이유: 잘라 쓰면 알고리즘 사이에 관련 키가 생기고, kx 키를 순번 봉인기에 쓰면
// iv가 0이라 128 bit 키에서 다중 사용자 보강을 잃는다.
// - 빈 키 쌍은 StaleHandle, 열거값이 아니거나 키가 32 byte가 아닌 알고리즘은 InvalidArgument(기기와
//   관계없이 같다), 이 기기에서 쓸 수 없는 알고리즘은 Unsupported다.
// - 상대 공개키가 small-order 점이면(공유 비밀이 0) AuthenticationFailed다. 그 밖의 32 byte는 RFC
//   7748대로 받는다(최상위 bit는 무시하고, twist 점과 p 이상인 비정규 u도 계산한다).
// - 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] ROCKS_CRYPTO_API Result<KxSessionKeys>
TryClientSessionKeys(const KxKeyPair& client, const KxPublicKey& server,
                     AeadAlgorithm algorithm) noexcept;

// 서버 쪽 세션 키다. 인자와 오류는 TryClientSessionKeys와 같다. 서버 transmit = 클라이언트
// receive다.
[[nodiscard]] ROCKS_CRYPTO_API Result<KxSessionKeys>
TryServerSessionKeys(const KxKeyPair& server, const KxPublicKey& client,
                     AeadAlgorithm algorithm) noexcept;

// sealed box 출력 길이(messageBytes + 48)다. 넘치면 Overflow다.
[[nodiscard]] ROCKS_CRYPTO_API Result<usize> TryGetSealedBoxBytes(usize messageBytes) noexcept;

// recipient에게 message를 봉인한다. 쓴 byte 수(message + 48)를 돌려준다.
// - output이 작거나 message가 상한을 넘으면 CapacityExceeded다.
// - 제자리: message.Data() == output.Data() + 32일 때만 겹침을 허용하고, 그 밖의 겹침은
//   InvalidArgument다.
// - recipient가 올바른 점이 아니면(small-order) AuthenticationFailed이고 출력의 임시 공개키 자리를
//   지운다(평문 자리는 쓰지 않았으므로 제자리 평문은 그대로다).
// - 임시 키 쌍은 libsodium 난수 경로로 만들며, OS 난수가 실패하면 Fatal이다(11절).
[[nodiscard]] ROCKS_CRYPTO_API Result<usize> TrySealToRecipient(const BoxPublicKey& recipient,
                                                                Span<const uint8> message,
                                                                Span<uint8> output) noexcept;

// TrySealToRecipient의 출력을 연다. 평문 byte 수를 돌려준다.
// - sealed가 48 byte보다 짧거나, 변조되었거나, 다른 수신자의 것이거나, 임시 공개키가 small-order면
//   AuthenticationFailed이고 plaintext 전체를 지운다.
// - plaintext가 평문 길이보다 작으면 CapacityExceeded다(쓰지 않는다).
// - 제자리: plaintext.Data() == sealed.Data() + 32일 때만 겹침을 허용하고 겹침은 길이 검사보다 먼저
//   본다. 빈 키 쌍은 StaleHandle이다.
[[nodiscard]] ROCKS_CRYPTO_API Result<usize> TryOpenSealed(const BoxKeyPair& recipient,
                                                           Span<const uint8> sealed,
                                                           Span<uint8> plaintext) noexcept;

namespace hazmat {

// X25519 비밀키(32 byte)를 평문으로 output에 복사한다(10.4절). 빈 키 쌍은 StaleHandle,
// output.Size()가 32가 아니면 InvalidArgument다. output은 호출자가 지운다.
[[nodiscard]] ROCKS_CRYPTO_API Result<void> TryExportKey(const KxKeyPair& key,
                                                         Span<uint8> output) noexcept;
[[nodiscard]] ROCKS_CRYPTO_API Result<void> TryExportKey(const BoxKeyPair& key,
                                                         Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
