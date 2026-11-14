#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 인증 암호(AEAD)다(Docs/Design/Crypto.ko.md 4·6·8·11절). 구현은 libsodium이다(AES-128-GCM은 AES
// 경량 계층 단계에서 더한다).
//
// - 기본은 XChaCha20-Poly1305다(GetPreferredAead, 결정 D2). 모든 CPU에서 상수 시간이고 nonce가 24
//   byte라 무작위로 만들어도 안전하다.
// - AES-256-GCM·AEGIS는 HW 명령(x86-64 AES-NI·PCLMULQDQ·AVX, ARM64 AES·PMULL)이 있을 때만 쓴다. 두
//   알고리즘은 같은 조건이라 함께 쓸 수 있거나 함께 Unsupported다(소프트웨어 AES와 AEGIS soft
//   경로는 상수 시간이 아니라서 쓰지 않는다, 결정 D3).
// - 고수준 TrySeal·TryOpen은 nonce를 만들어 앞에 붙인다(출력 = nonce ‖ 암호문 ‖ tag). 무작위
//   nonce가 안전한 알고리즘(XChaCha20-Poly1305, AEGIS-256)만 받고, 나머지(ChaCha20-IETF·AES-GCM의
//   96 bit, AEGIS-128L의 128 bit nonce)는 Unsupported다. 그 알고리즘은 hazmat nonce 지정 API(카운터
//   nonce)나 세그먼트 스트림으로 쓴다.
// - 복호 실패(tag 불일치, 잘린 입력, 다른 키·AD)는 모두 AuthenticationFailed이고 평문 출력 전체를
//   0으로 지운다. 어느 검사가 실패했는지는 알리지 않는다(결정 D5). AES-GCM·AEGIS는 검증 전에 복호한
//   byte를 plaintext에 잠시 쓰고 실패하면 지운다(ChaCha20 계열은 검증 뒤에 쓴다). 호출 중에 다른
//   스레드나 프로세스가 같은 버퍼를 읽지 않게 한다.
// - 키는 알고리즘에 묶인 AeadKey이고 키 저장소에 있다. 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
//   써도 된다. GCM·ChaCha20-Poly1305·AEGIS는 key commitment가 없다. 한 암호문이 다른 키로도 열릴 수
//   있으므로 형식은 키 ID를 기록하고 시도 복호를 하지 않는다(2절).
//
// 제자리 처리(7절): TrySeal은 plaintext.Data() == output.Data() + NonceBytes일 때, TryOpen은
// plaintext.Data() == sealed.Data() + NonceBytes일 때만 겹침을 허용한다. hazmat 함수는 입력과
// 출력이 같은 주소에서 시작할 때만 허용한다. 그 밖의 겹침은 쓰기 전에 InvalidArgument다.
//
// 사용 예:
//   auto key = crypto::AeadKey::TryGenerate(crypto::GetPreferredAead());
//   if (!key) { return key.Error(); }
//   auto sealedBytes = crypto::TryGetSealedBytes(key.Value().Algorithm(), plaintext.Size());
//   Array<uint8> sealed(allocator);   // sealedBytes.Value() 크기로 준비
//   auto written = crypto::TrySeal(key.Value(), associatedData, plaintext, sealed.MutableView());
//   auto opened = crypto::TryOpen(key.Value(), associatedData, sealed.View(), plaintextOut);
namespace rocks::crypto {

// 영속 형식에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class AeadAlgorithm : uint8 {
    XChaCha20Poly1305 = 1,    // key 32, nonce 24, tag 16
    ChaCha20Poly1305Ietf = 2, // key 32, nonce 12, tag 16 (RFC 8439)
    Aes256Gcm = 3,            // key 32, nonce 12, tag 16
    Aes128Gcm = 4,            // key 16, nonce 12, tag 16 (AES 경량 계층, 아직 Unsupported)
    Aegis256 = 5,             // key 32, nonce 32, tag 32
    Aegis128L = 6,            // key 16, nonce 16, tag 32
};

struct AeadInfo {
    usize keyBytes = 0;
    usize nonceBytes = 0;
    usize tagBytes = 0;
    // (key, nonce) 한 쌍으로 봉인할 수 있는 평문 상한이다(11절 표).
    uint64 maxMessageBytes = 0;
    // 무작위 nonce를 써도 되는가(XChaCha20-Poly1305, AEGIS-256). 고수준 TrySeal은 이 알고리즘만
    // 받는다.
    bool randomNonceSafe = false;
    // 이 기기에서 쓸 수 있는가. AES·AEGIS는 HW 명령이 있을 때만 true다.
    bool available = false;
    // 진단용 구현 이름이다(예: "libsodium/aesni"). 쓸 수 없으면 빈 문자열이다.
    StringView implementation;
};

// 알고리즘 정보다. 열거값이 아니면 InvalidArgument다.
[[nodiscard]] Result<AeadInfo> TryGetAeadInfo(AeadAlgorithm algorithm) noexcept;

// 기본 AEAD다(XChaCha20-Poly1305). 모든 기기에서 같은 값이다(결정 D2).
[[nodiscard]] AeadAlgorithm GetPreferredAead() noexcept;

// 고수준 TrySeal의 출력 길이(nonce + plaintextBytes + tag)다. 넘치면 Overflow, 열거값이 아니면
// InvalidArgument다.
[[nodiscard]] Result<usize> TryGetSealedBytes(AeadAlgorithm algorithm,
                                              usize plaintextBytes) noexcept;

// 알고리즘에 묶인 AEAD 키다. move-only이며 생성 뒤 바뀌지 않는다. AES-256-GCM 키는 만들 때 키 확장
// 상태를 한 번 계산해 키 저장소에 둔다. 평문 내보내기는 hazmat::TryExportKey다.
class AeadKey {
public:
    AeadKey() noexcept = default;
    ~AeadKey() noexcept;

    AeadKey(const AeadKey&) = delete;
    AeadKey& operator=(const AeadKey&) = delete;
    AeadKey(AeadKey&& other) noexcept;
    AeadKey& operator=(AeadKey&& other) noexcept;

    // OS 난수 키다. 이 기기에서 쓸 수 없는 알고리즘은 Unsupported, 열거값이 아니면 InvalidArgument,
    // OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<AeadKey> TryGenerate(AeadAlgorithm algorithm) noexcept;

    // keyBytes를 복사한 키다. 길이가 알고리즘 키 길이와 다르면 InvalidArgument다. 나머지는
    // TryGenerate와 같다. keyBytes는 호출자가 지운다.
    [[nodiscard]] static Result<AeadKey> TryImport(AeadAlgorithm algorithm,
                                                   Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] AeadAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    AeadKey(AeadAlgorithm algorithm, SecretBytes&& key, void* state) noexcept
        : algorithm_(algorithm), key_(Move(key)), state_(state) {
    }

    void Release() noexcept;

    AeadAlgorithm algorithm_ = AeadAlgorithm::XChaCha20Poly1305;
    SecretBytes key_;
    // AES-256-GCM의 libsodium beforenm 상태(키 저장소 슬롯)다. 다른 알고리즘은 nullptr다.
    void* state_ = nullptr;
};

// master에서 AEAD 키를 뽑는다(Kdf.h 타입 파생, 키 타입·알고리즘을 섞는다). 알고리즘 오류는
// AeadKey::TryGenerate와 같고 먼저 검사한다. 빈 master는 StaleHandle, 키 저장소가 찼으면
// OutOfMemory다.
[[nodiscard]] Result<AeadKey> TryDeriveAeadKey(const KdfKey& master, uint64 subkeyId,
                                               KdfContext context,
                                               AeadAlgorithm algorithm) noexcept;

// 무작위 nonce를 만들어 봉인한다. 출력 = nonce ‖ 암호문 ‖ tag이고 쓴 byte 수를 돌려준다.
// - 무작위 nonce가 안전하지 않은 알고리즘(randomNonceSafe false)은 Unsupported다.
// - output이 TryGetSealedBytes보다 작거나 평문이 상한을 넘으면 CapacityExceeded다.
// - 허용되지 않은 겹침(제자리 규칙 밖, AD와 출력)은 InvalidArgument다.
// - 빈 키는 StaleHandle, nonce용 OS 난수 실패는 DependencyFailed다.
[[nodiscard]] Result<usize> TrySeal(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> plaintext, Span<uint8> output) noexcept;

// TrySeal의 출력을 연다. 평문 byte 수를 돌려준다.
// - sealed가 nonce + tag보다 짧거나 tag가 맞지 않으면 AuthenticationFailed이고 plaintext 전체를
//   0으로 지운다.
// - plaintext가 평문 길이보다 작으면 CapacityExceeded다(쓰지 않는다).
// - 허용되지 않은 겹침은 InvalidArgument, 무작위 nonce 알고리즘이 아닌 키는 Unsupported, 빈 키는
//   StaleHandle이다.
[[nodiscard]] Result<usize> TryOpen(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> sealed, Span<uint8> plaintext) noexcept;

namespace hazmat {

// 키 byte를 평문으로 output에 복사한다(10.4절). output.Size()는 키 길이와 같아야 한다.
[[nodiscard]] Result<void> TryExportKey(const AeadKey& key, Span<uint8> output) noexcept;

// nonce를 호출자가 책임진다. 같은 키로 같은 nonce를 두 번 쓰면 기밀성과 무결성이 모두
// 깨진다(AEGIS-256·XChaCha20-Poly1305가 아니면 무작위 nonce도 쓰지 않는다). 결합 형식은 암호문 ‖
// tag다. 이 함수들은 모든 알고리즘을 받는다.
// - nonce 길이가 틀리거나 허용되지 않은 겹침이면 InvalidArgument, 빈 키는 StaleHandle이다. GCM의 96
//   bit가 아닌 nonce는 Unsupported다(표준은 허용하지만 지원하지 않는다).
// - 출력이 작거나 평문이 상한을 넘으면 CapacityExceeded다.
// - 여는 쪽은 tag 불일치·잘린 입력이 AuthenticationFailed이고 plaintext 전체를 0으로 지운다.
[[nodiscard]] Result<usize> TrySealWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                             Span<const uint8> associatedData,
                                             Span<const uint8> plaintext,
                                             Span<uint8> sealed) noexcept;
[[nodiscard]] Result<usize> TryOpenWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                             Span<const uint8> associatedData,
                                             Span<const uint8> sealed,
                                             Span<uint8> plaintext) noexcept;

// 분리 형식이다. ciphertext.Size()는 plaintext.Size()와 같아야 한다(다르면 InvalidArgument). 봉인
// 쪽 tag.Size()는 tag 길이와 같아야 하고(InvalidArgument), 여는 쪽 tag 길이가 틀리면 잘린 tag로
// 보고 AuthenticationFailed다.
[[nodiscard]] Result<void> TrySealDetached(const AeadKey& key, Span<const uint8> nonce,
                                           Span<const uint8> associatedData,
                                           Span<const uint8> plaintext, Span<uint8> ciphertext,
                                           Span<uint8> tag) noexcept;
[[nodiscard]] Result<void> TryOpenDetached(const AeadKey& key, Span<const uint8> nonce,
                                           Span<const uint8> associatedData,
                                           Span<const uint8> ciphertext, Span<const uint8> tag,
                                           Span<uint8> plaintext) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
