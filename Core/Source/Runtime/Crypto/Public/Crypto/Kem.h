#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 키 캡슐화(KEM)다(Docs/Design/Crypto.ko.md 4절). 구현은 libsodium crypto_kem_xwing·
// crypto_kem_mlkem768이다.
//
// - X-Wing(기본, GetPreferredKem): ML-KEM-768과 X25519를 묶은 하이브리드다. 둘 중 하나만 안전해도
//   공유 비밀이 안전하다. libsodium 1.0.22 구성은 draft-connolly-cfrg-xwing-kem-11과 같다(시험의
//   draft 부록 벡터로 고정한다). TLS의 X25519MLKEM768(연결형 결합)과는 호환되지 않는다.
// - ML-KEM-768(FIPS 203) 단독은 FIPS 203만 허용되는 환경이나 ML-KEM 전용 상대와의 상호운용에 쓴다.
//   ML-KEM은 공유 비밀이 암호문·공개키에 묶이지 않으므로(MAL-BIND) 서명 기반 AKE 등에서는 공개키·
//   암호문·맥락을 TryDeriveBoundKdfKey의 transcript로 넣어 세션 키를 뽑는다.
// - 보내는 쪽은 수신자 공개키로 TryEncapsulate를 불러 암호문(보낼 것)과 공유 비밀을 얻고, 받는 쪽은
//   TryDecapsulate로 같은 공유 비밀을 얻는다. 공유 비밀은 KdfKey로 준다. 용도별 키는 Kdf.h 타입
//   파생으로 뽑고, 공유 비밀을 그대로 다른 키로 쓰지 않는다.
// - KEM은 보낸 사람을 인증하지 않고, 보내는 쪽은 수신자 공개키가 진짜인지(서명·고정 키) 먼저
//   확인한다(중간자 방지). 보낸 사람 인증이 필요하면 암호문·공개키에 서명한다(Signature.h).
// - ML-KEM은 암호문이 틀려도 오류 대신 다른(무관한) 공유 비밀을 준다(암묵적 거부). 변조는 그 키로
//   연 AEAD가 AuthenticationFailed로 잡는다. X-Wing은 X25519 부분이 small-order 점(X25519 결과가
//   0)이면 ML-KEM 캡슐 해제 뒤 AuthenticationFailed다(RFC 7748 6.1이 허용하는 엄격화이고,
//   암호문만으로 정해지는 공개 술어라 oracle이 아니다).
// - 키 쌍은 seed(X-Wing 32 byte, ML-KEM 64 byte = d ‖ z, FIPS 203 seed 형식)에서 결정적으로 나오고,
//   키 저장소에는 seed만 둔다(키 쌍 하나가 작은 슬롯 하나). ML-KEM 캡슐 해제는 호출마다 seed를 확장
//   비밀키(2400 byte)로 펼쳐 키 저장소 임시 슬롯에 두었다가 지운다(libsodium X-Wing과 같은 방식).
//   공개키는 값 타입이다(최대 1216 byte).
//
// 사용 예:
//   // 받는 쪽
//   auto recipient = crypto::KemKeyPair::TryGenerate(crypto::GetPreferredKem());
//   // 보내는 쪽(확인한 수신자 공개키 publicKey)
//   const crypto::KemInfo info = crypto::TryGetKemInfo(publicKey.Algorithm()).Value();
//   uint8 ciphertext[crypto::KemMaxCiphertextBytes];
//   const Span<uint8> wire = Span<uint8>(ciphertext).Left(info.ciphertextBytes);
//   auto shared = crypto::TryEncapsulate(publicKey, wire);
//   if (!shared) { return shared.Error(); }
//   auto key = crypto::TryDeriveAeadKey(shared.Value(), 1, ChannelContext,
//                                       crypto::GetPreferredAead());
//   // 받는 쪽: 같은 공유 비밀
//   auto same = crypto::TryDecapsulate(recipient.Value(), receivedCiphertext);
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
} // namespace detail

// 영속 형식에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class KemAlgorithm : uint8 {
    XWing = 1,    // 공개키 1216, 암호문 1120, seed 32
    MlKem768 = 2, // 공개키 1184, 암호문 1088, seed 64(d ‖ z)
};

inline constexpr usize KemMaxPublicKeyBytes = 1216;
inline constexpr usize KemMaxCiphertextBytes = 1120;
inline constexpr usize KemMaxSeedBytes = 64;
inline constexpr usize KemSharedSecretBytes = 32;

struct KemInfo {
    usize publicKeyBytes = 0;
    usize ciphertextBytes = 0;
    usize seedBytes = 0;
};

// 알고리즘 정보다. 열거값이 아니면 InvalidArgument다.
[[nodiscard]] Result<KemInfo> TryGetKemInfo(KemAlgorithm algorithm) noexcept;

// 기본 KEM(X-Wing)이다.
[[nodiscard]] KemAlgorithm GetPreferredKem() noexcept;

// KEM 공개키다(값 타입). 기본 생성한 값은 비어 있다.
class KemPublicKey {
public:
    KemPublicKey() noexcept = default;

    // 알고리즘이 열거값이 아니거나 길이가 공개키 길이와 다르면 InvalidArgument다. 내용(ML-KEM 계수
    // 범위, X25519 점)은 캡슐화 때 본다.
    [[nodiscard]] static Result<KemPublicKey> TryFromBytes(KemAlgorithm algorithm,
                                                           Span<const uint8> bytes) noexcept;

    [[nodiscard]] KemAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes_, size_);
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] bool operator==(const KemPublicKey& other) const noexcept = default;

private:
    KemAlgorithm algorithm_ = KemAlgorithm::XWing;
    usize size_ = 0;
    uint8 bytes_[KemMaxPublicKeyBytes] = {};
};

// KEM 키 쌍이다. move-only이며 생성 뒤 바뀌지 않는다. 키 저장소에 seed만 둔다.
class KemKeyPair {
public:
    KemKeyPair() noexcept = default;

    // OS 난수 seed로 만든다. 열거값이 아니면 InvalidArgument, OS 난수 실패는 DependencyFailed, 키
    // 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<KemKeyPair> TryGenerate(KemAlgorithm algorithm) noexcept;

    // seed에서 결정적으로 만든다. seed 길이가 KemInfo::seedBytes와 다르면 InvalidArgument다. seed는
    // 호출자가 지운다.
    [[nodiscard]] static Result<KemKeyPair> TryFromSeed(KemAlgorithm algorithm,
                                                        Span<const uint8> seed) noexcept;

    [[nodiscard]] const KemPublicKey& PublicKey() const noexcept {
        return publicKey_;
    }

    [[nodiscard]] KemAlgorithm Algorithm() const noexcept {
        return publicKey_.Algorithm();
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    KemKeyPair(SecretBytes&& seed, const KemPublicKey& publicKey) noexcept;

    // seed다(X-Wing은 libsodium 비밀키 자체, ML-KEM은 d ‖ z).
    SecretBytes key_;
    KemPublicKey publicKey_;
};

// recipient에게 캡슐화한다. ciphertext에 암호문을 쓰고 공유 비밀(32 byte)을 KdfKey로 돌려준다.
// - recipient가 비었거나 ciphertext.Size()가 암호문 길이와 다르면 InvalidArgument다.
// - 공개키 내용이 올바르지 않으면(ML-KEM 계수가 q 이상(X-Wing의 ML-KEM 부분 포함), X-Wing의 X25519
//   부분이 small-order 점) AuthenticationFailed이고 ciphertext를 지운다.
// - 키 저장소가 찼으면 OutOfMemory다. 캡슐화 난수는 libsodium 난수 경로이고 OS 난수 실패는
//   Fatal이다.
[[nodiscard]] Result<KdfKey> TryEncapsulate(const KemPublicKey& recipient,
                                            Span<uint8> ciphertext) noexcept;

// 암호문에서 공유 비밀을 얻는다. 빈 키 쌍은 StaleHandle이다. 암호문 길이가 틀리거나 X-Wing의 X25519
// 부분이 small-order 점이면 AuthenticationFailed다. ML-KEM 부분이 변조되면 오류 없이 다른 공유
// 비밀이 나온다(암묵적 거부). ML-KEM 확장 비밀키를 둘 임시 슬롯이 없으면 OutOfMemory다.
[[nodiscard]] Result<KdfKey> TryDecapsulate(const KemKeyPair& recipient,
                                            Span<const uint8> ciphertext) noexcept;

namespace hazmat {

// seed를 평문으로 output에 복사한다(10.4절). 빈 키 쌍은 StaleHandle, output.Size()가
// KemInfo::seedBytes와 다르면 InvalidArgument다. output은 호출자가 지운다.
[[nodiscard]] Result<void> TryExportSeed(const KemKeyPair& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
