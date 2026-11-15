#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/Aead.h>

// 저수준 AES 모드(hazmat)다: AES 키(AesKey)와 CTR, AES-GCM 멀티파트(Docs/Design/Crypto.ko.md 4·5.2·
// 9.3절). 구현은 HW 전용 AES 경량 계층이다. AES 명령(x86-64 AES-NI·PCLMULQDQ·AVX, ARM64
// AES·PMULL)이 없는 기기에서는 모두 Unsupported다(소프트웨어 AES는 두지 않는다). AES-CMAC은
// MAC이므로 Mac.h(MacAlgorithm::AesCmac)에 있다.
//
// 상호운용 형식이 이 모드를 요구할 때만 쓴다. 새 형식은 고수준 AEAD(Aead.h)나 secretstream·세그먼트
// 스트림을 쓴다.
// - CTR은 인증이 없다. 암호문 변조를 막지 못하므로 MAC(Encrypt-then-MAC)을 따로 붙인다. 같은 키로
//   같은 카운터를 두 번 쓰면 두 평문의 XOR이 드러난다. 카운터 블록 관리는 호출자 몫이다.
// - GCM 멀티파트 복호는 tag를 확인하기 전에 평문을 내보낸다(TryUpdateUnverified). TryFinishVerify가
//   성공하기 전까지 그 평문을 쓰거나 내보내지 않는다.
//
// 공통 규칙(9.1절):
// - 객체는 move-only다. 기본 생성·이동된 객체·Reset 뒤의 객체는 빈 객체이고 모든 연산이
//   StaleHandle이다. 한 객체는 한 번에 한 스레드만 쓴다.
// - 상태(키 스케줄 사본, 남은 keystream, GHASH 누적값)는 키 저장소(SecretArena) 슬롯에 두고
// 끝·Reset·
//   소멸 때 지운다. 상태가 키 스케줄을 복사하므로 만든 뒤에는 키 객체를 버려도 된다.
// - 쓰기 전에 거부한 오류(길이·겹침·단계 InvalidArgument, 한도·출력 부족 CapacityExceeded)는 상태를
//   바꾸지 않는다. 출력은 입력과 같은 주소에서 시작할 때(제자리)만 겹칠 수 있다.
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
} // namespace detail

namespace hazmat {

// AES 블록 길이이자 CTR 카운터 블록 길이다.
inline constexpr usize AesBlockBytes = 16;

// AES-128·AES-256 키 길이다(AES-192는 받지 않는다).
inline constexpr usize Aes128KeyBytes = 16;
inline constexpr usize Aes256KeyBytes = 32;

// GCM 멀티파트의 nonce(96 bit만)·tag 길이다.
inline constexpr usize AesGcmNonceBytes = 12;
inline constexpr usize AesGcmTagBytes = 16;

// GCM 멀티파트의 (key, nonce)당 평문 상한(16 × (2^32 - 2) byte, SP 800-38D)과 AD 상한이다.
inline constexpr uint64 AesGcmMaxMessageBytes = 16 * ((uint64{1} << 32) - 2);
inline constexpr uint64 AesGcmMaxAssociatedBytes = (uint64{1} << 61) - 1;

// 블록 암호 모드(CTR)용 AES 키다. AEAD 키(AeadKey)와 섞지 않는다(2절: 키는 알고리즘·용도에 묶인다).
// move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에 읽어도 된다. 키를 만들 때 라운드 키를
// 한 번 계산해 키 저장소에 두고 소멸 때 지운다.
//
// 사용 예:
//   auto key = crypto::hazmat::AesKey::TryImport(keyBytes);    // 16 또는 32 byte
//   if (!key) { return key.Error(); }
//   auto done = crypto::hazmat::TryAesCtrXor(key.Value(), counterBlock, input, output);
class AesKey {
public:
    AesKey() noexcept = default;
    ~AesKey() noexcept;

    AesKey(const AesKey&) = delete;
    AesKey& operator=(const AesKey&) = delete;
    AesKey(AesKey&& other) noexcept;
    AesKey& operator=(AesKey&& other) noexcept;

    // OS 난수 키다. keyBytes는 16(AES-128) 또는 32(AES-256)다. 다른 길이는 InvalidArgument, AES
    // HW가 없으면 Unsupported, OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<AesKey> TryGenerate(usize keyBytes) noexcept;

    // keyBytes(16 또는 32 byte)를 확장한 키다. 오류는 TryGenerate와 같다. keyBytes는 호출자가
    // 지운다.
    [[nodiscard]] static Result<AesKey> TryImport(Span<const uint8> keyBytes) noexcept;

    // 키 byte 수(16·32)다. 빈 키는 0이다.
    [[nodiscard]] usize KeyBytes() const noexcept {
        return keyBytes_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

private:
    friend struct detail::KeyAccess;

    void Release() noexcept;

    // 키 스케줄(detail::AesSchedule, 키 저장소 슬롯)이다. 원래 키 byte는 따로 두지 않는다.
    void* state_ = nullptr;
    usize keyBytes_ = 0;
};

// CTR 카운터 폭이다. 카운터 블록의 끝 32·64·128 bit를 big-endian 정수로 보고 블록마다 1씩 더하며,
// 올림은 그 폭 안에서만 전파한다(앞 byte는 바뀌지 않는다). 상호운용 형식이 정한 폭을 고른다:
// NIST SP 800-38A 부록 B.1의 전체 블록 증가는 Bits128, nonce ‖ 32 bit 블록 카운터(RFC 3686 등)는
// Bits32, nonce ‖ 64 bit 카운터는 Bits64다.
enum class AesCtrCounterWidth : uint8 {
    Bits32 = 32,
    Bits64 = 64,
    Bits128 = 128,
};

// AES-CTR 스트림이다. TryProcess를 여러 번 불러 이어지는 keystream으로 처리한다(부분 블록의 남은
// keystream을 다음 호출이 이어 쓴다). 암호화와 복호는 같은 연산이다.
//
// - 폭 안의 카운터 값이 다 떨어지면(마지막 값 2^폭 - 1을 쓴 뒤, 또는 이번 호출에 필요한 블록이 남은
//   값보다 많으면) 쓰기 전에 CapacityExceeded이고 상태는 그대로다. 카운터는 한 바퀴 돌지 않는다.
//   마지막 값으로 만든 블록의 남은 keystream은 계속 쓸 수 있다.
//
// 사용 예:
//   auto ctr = crypto::hazmat::AesCtr::TryCreate(key, initialCounterBlock);   // 기본 Bits128
//   if (!ctr) { return ctr.Error(); }
//   for (...) { if (auto done = ctr.Value().TryProcess(chunk, outChunk); !done) { ... } }
class AesCtr {
public:
    AesCtr() noexcept = default;
    ~AesCtr() noexcept;

    AesCtr(const AesCtr&) = delete;
    AesCtr& operator=(const AesCtr&) = delete;
    AesCtr(AesCtr&& other) noexcept;
    AesCtr& operator=(AesCtr&& other) noexcept;

    // key로 CTR 스트림을 만든다. initialCounterBlock은 첫 keystream 블록의 카운터 블록(16
    // byte)이다. 빈 키는 StaleHandle, 카운터 블록 길이가 16이 아니거나 width가 열거값이 아니면
    // InvalidArgument, 키 저장소가 찼으면 OutOfMemory다. 상태가 키 스케줄을 복사하므로 key는 만든
    // 뒤 버려도 된다.
    [[nodiscard]] static Result<AesCtr>
    TryCreate(const AesKey& key, Span<const uint8> initialCounterBlock,
              AesCtrCounterWidth width = AesCtrCounterWidth::Bits128) noexcept;

    // input을 keystream과 XOR해 output에 쓴다. output.Size()는 input.Size()와 같아야 하고, output은
    // input과 같은 주소에서 시작하거나 겹치지 않아야 한다(아니면 InvalidArgument). 카운터가
    // 모자라면 CapacityExceeded이고 아무것도 쓰지 않는다. 빈 객체는 StaleHandle이다.
    [[nodiscard]] Result<void> TryProcess(Span<const uint8> input, Span<uint8> output) noexcept;

    // 상태를 지우고 빈 객체가 된다. 같은 카운터로 다시 시작하는 길은 두지 않는다(카운터 재사용).
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

private:
    void* state_ = nullptr;
};

// 일회성 AES-CTR이다. 인자·오류·겹침 규칙은 AesCtr::TryCreate·TryProcess와 같고 키 저장소를 쓰지
// 않는다. 카운터가 모자라면(input이 initialCounterBlock부터 폭 안에 남은 블록보다 길면) 쓰기 전에
// CapacityExceeded다.
[[nodiscard]] Result<void>
TryAesCtrXor(const AesKey& key, Span<const uint8> initialCounterBlock, Span<const uint8> input,
             Span<uint8> output, AesCtrCounterWidth width = AesCtrCounterWidth::Bits128) noexcept;

// AES-GCM 멀티파트 암호화다(9.3절). 일회성 hazmat::TrySealDetached(Aead.h)와 같은 암호문·tag를
// 낸다. 상호운용 형식이 GCM 하나로 메모리에 다 올릴 수 없는 큰 메시지를 요구할 때만 쓴다.
//
// - key: AES-128-GCM 또는 AES-256-GCM AeadKey다. 다른 알고리즘은 InvalidArgument다. 상태가 키
//   스케줄을 복사하므로 key는 만든 뒤 버려도 된다.
// - nonce: 96 bit만 받는다(다른 길이는 Unsupported). 같은 키로 같은 nonce를 두 번 쓰면 안 된다.
//   이 객체는 nonce를 관리하지 않는다(Aead.h hazmat 규칙과 같다).
// - 순서: TryUpdateAssociatedData(0번 이상) → TryUpdate(0번 이상) → TryFinish. TryUpdate를 한
// 번이라도
//   부른 뒤(빈 입력 포함)나 끝난 뒤의 AD는 InvalidArgument이고 상태는 그대로다.
// - 한도: 평문 합이 AesGcmMaxMessageBytes, AD 합이 AesGcmMaxAssociatedBytes를 넘으면 쓰기 전에
//   CapacityExceeded다. 키당 블록 한도(AeadInfo::maxBlocksPerKey)는 호출자가 센다.
//
// 사용 예:
//   auto encryptor = crypto::hazmat::AesGcmEncryptor::TryCreate(key, nonce);
//   if (!encryptor) { return encryptor.Error(); }
//   (void)encryptor.Value().TryUpdateAssociatedData(header);
//   auto written = encryptor.Value().TryUpdate(chunk, outChunk);   // 조각마다
//   uint8 tag[crypto::hazmat::AesGcmTagBytes];
//   auto finished = encryptor.Value().TryFinish(Span<uint8>(tag));
class AesGcmEncryptor {
public:
    AesGcmEncryptor() noexcept = default;
    ~AesGcmEncryptor() noexcept;

    AesGcmEncryptor(const AesGcmEncryptor&) = delete;
    AesGcmEncryptor& operator=(const AesGcmEncryptor&) = delete;
    AesGcmEncryptor(AesGcmEncryptor&& other) noexcept;
    AesGcmEncryptor& operator=(AesGcmEncryptor&& other) noexcept;

    // 빈 키는 StaleHandle, AES-GCM이 아닌 키는 InvalidArgument, 96 bit가 아닌 nonce는 Unsupported,
    // 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<AesGcmEncryptor> TryCreate(const AeadKey& key,
                                                           Span<const uint8> nonce) noexcept;

    // AD를 이어 붙인다(조각으로 나눠 넣어도 한 번에 넣은 것과 같다).
    [[nodiscard]] Result<void> TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept;

    // plaintext를 암호화해 ciphertext 앞 plaintext.Size() byte에 쓰고 그 수를 돌려준다.
    // ciphertext가 작으면 CapacityExceeded, 제자리가 아닌 겹침은 InvalidArgument다.
    [[nodiscard]] Result<usize> TryUpdate(Span<const uint8> plaintext,
                                          Span<uint8> ciphertext) noexcept;

    // tag(AesGcmTagBytes)를 쓰고 끝낸다. tag 길이가 틀리면 InvalidArgument이고 상태는 그대로다(다시
    // 부를 수 있다). 끝난 뒤의 모든 호출은 InvalidArgument다.
    [[nodiscard]] Result<void> TryFinish(Span<uint8> tag) noexcept;

    // 상태를 지우고 빈 객체가 된다. 같은 nonce로 다시 시작하는 길은 두지 않는다.
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

private:
    [[nodiscard]] Result<void> CheckActive() const noexcept;

    void* state_ = nullptr;
    bool finished_ = false;
};

// AES-GCM 멀티파트 복호다. 인자·순서·한도는 AesGcmEncryptor와 같다.
//
// - TryUpdateUnverified가 내는 평문은 TryFinishVerify가 성공하기 전까지 인증되지 않았다. 실패하면
//   호출자가 지금까지 받은 평문을 모두 버리고 지운다(이 객체는 이미 내보낸 평문을 되돌리지 못한다).
// - TryFinishVerify는 결과와 관계없이 끝낸다. tag 불일치·길이가 틀린 tag(잘린 tag)는
//   AuthenticationFailed이고 sticky다: 뒤의 모든 호출이 AuthenticationFailed다(Reset 전까지).
//   성공한 뒤의 호출은 InvalidArgument다.
class AesGcmDecryptor {
public:
    AesGcmDecryptor() noexcept = default;
    ~AesGcmDecryptor() noexcept;

    AesGcmDecryptor(const AesGcmDecryptor&) = delete;
    AesGcmDecryptor& operator=(const AesGcmDecryptor&) = delete;
    AesGcmDecryptor(AesGcmDecryptor&& other) noexcept;
    AesGcmDecryptor& operator=(AesGcmDecryptor&& other) noexcept;

    // 오류는 AesGcmEncryptor::TryCreate와 같다.
    [[nodiscard]] static Result<AesGcmDecryptor> TryCreate(const AeadKey& key,
                                                           Span<const uint8> nonce) noexcept;

    [[nodiscard]] Result<void> TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept;

    // ciphertext를 복호해 plaintext 앞 ciphertext.Size() byte에 쓰고 그 수를 돌려준다(검증 전
    // 평문). plaintext가 작으면 CapacityExceeded, 제자리가 아닌 겹침은 InvalidArgument다.
    [[nodiscard]] Result<usize> TryUpdateUnverified(Span<const uint8> ciphertext,
                                                    Span<uint8> plaintext) noexcept;

    // tag를 상수 시간으로 확인하고 끝낸다. 맞으면 성공, 다르거나 길이가 틀리면
    // AuthenticationFailed다.
    [[nodiscard]] Result<void> TryFinishVerify(Span<const uint8> tag) noexcept;

    // 상태를 지우고 빈 객체가 된다(sticky 실패도 지운다).
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

    // TryFinishVerify가 실패했는가. 실패한 뒤의 호출은 늘 AuthenticationFailed다.
    [[nodiscard]] bool HasFailed() const noexcept {
        return failed_;
    }

private:
    [[nodiscard]] Result<void> CheckActive() const noexcept;

    void* state_ = nullptr;
    bool finished_ = false;
    bool failed_ = false;
};

} // namespace hazmat

} // namespace rocks::crypto
