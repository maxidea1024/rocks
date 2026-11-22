#pragma once

#include "AesKernels.h"

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

// AES 경량 계층의 모드(GCM 일회성·멀티파트, CTR, CMAC)와 키 스케줄
// 수명이다(Docs/Design/Crypto.ko.md 5.2·5.3·9.3절). 모듈 내부 헤더이고 구현은 AesModes.cpp다. 공개
// 경로(AeadKey·hazmat AES 모드· MacKey)는 인자(길이·겹침·단계·한도)를 먼저 검사하고 이 함수를
// 부른다. 여기 함수는 그 검사를 다시 하지 않고, 어기면 범위 밖 쓰기나 카운터 재사용으로 이어지는
// 조건만 Fatal로 막는다.
//
// 사용 예(Aead 어댑터 안):
//   const AesKernels& kernels = GetAesKernels();
//   auto schedule = TryCreateAesSchedule(kernels, keyBytes);   // 키를 만들 때 한 번
//   AesGcmSealDetached(kernels, *schedule.Value(), nonce, ad, plaintext, ciphertext, tag);
//   DestroyAesSchedule(schedule.Value());                      // 키를 버릴 때
namespace rocks::crypto::detail {

inline constexpr usize AesGcmNonceBytes = 12;
inline constexpr usize AesGcmTagBytes = 16;
inline constexpr usize AesCmacTagBytes = 16;

// (key, nonce)당 평문 상한이다(NIST SP 800-38D의 2^39 - 256 bit = 16 × (2^32 - 2) byte). 카운터가
// 2부터 2^32 - 1까지 돌므로 inc32가 한 바퀴 돌지 않는다.
inline constexpr uint64 AesGcmMaxMessageBytes = 16 * ((uint64{1} << 32) - 2);

// AD 상한이다. 길이 블록에 bit 수(64 bit)로 쓰므로 2^61 byte 미만이어야 한다.
inline constexpr uint64 AesGcmMaxAssociatedBytes = (uint64{1} << 61) - 1;

// ---- 키 스케줄 ----

// key(16 또는 32 byte)로 키 저장소(SecretArena) 슬롯에 키 스케줄(라운드 키와 GHASH H 거듭제곱)을
// 만든다. key는 읽기만 하고 호출자가 지운다.
// - 반환한 스케줄은 DestroyAesSchedule로 돌려줄 때까지 유효하며, 그동안 바뀌지 않으므로 여러
//   스레드가 동시에 읽어도 된다. 소유자(키 객체)가 하나만 돌려준다.
// - 오류: kernels가 비었으면 Unsupported, key 길이가 16·32가 아니면 InvalidArgument, 키 저장소가
//   찼으면 OutOfMemory다. 실패하면 슬롯을 남기지 않는다.
[[nodiscard]] ROCKS_CRYPTO_API Result<AesSchedule*>
TryCreateAesSchedule(const AesKernels& kernels, Span<const uint8> key) noexcept;

// 스케줄 슬롯 전체를 지우고 키 저장소에 돌려준다. nullptr는 아무 일도 하지 않는다. 키 저장소가 준
// 살아 있는 슬롯이 아니면 Fatal이다(SecretArena 규칙).
ROCKS_CRYPTO_API void DestroyAesSchedule(AesSchedule* schedule) noexcept;

// ---- GCM 일회성 ----

// AES-GCM 봉인(분리 tag)이다. ciphertext ← CTR(plaintext), tag ← GHASH(AD, ciphertext) xor E_K(J0).
// - schedule: kernels의 expandKey가 만든 스케줄(AES-128·AES-256 모두 같은 함수다).
// - nonce: 12 byte(96 bit)만 받는다. J0 = nonce ‖ 00000001.
// - plaintext·ciphertext: 길이가 같고, ciphertext는 plaintext와 같은 주소(제자리)이거나 겹치지
//   않는다. tag(16 byte)·nonce·AD는 출력과 겹치지 않는다. 겹침은 호출자가 검사한다.
// - 길이 오류(nonce·tag, 두 버퍼 길이 차이, 평문·AD 상한 초과)와 빈 표는 호출자가 막았어야 할
//   결함이므로 Fatal이다.
// - 쓰는 동안 만든 keystream·GHASH 누적값·E_K(J0)는 돌아가기 전에 지운다.
ROCKS_CRYPTO_API void AesGcmSealDetached(const AesKernels& kernels, const AesSchedule& schedule,
                                         Span<const uint8> nonce, Span<const uint8> associatedData,
                                         Span<const uint8> plaintext, Span<uint8> ciphertext,
                                         Span<uint8> tag) noexcept;

// AES-GCM 열기(분리 tag)다. tag가 맞으면 plaintext에 평문을 쓰고 true다. 틀리면 plaintext 전체를
// 0으로 지우고 false다(호출자는 AuthenticationFailed로 돌려준다). tag는 상수 시간으로 비교한다.
// - 평문은 tag를 확인하기 전에 plaintext에 쓰인다(틀리면 지운다). 그래서 plaintext는 프로세스 전용
//   메모리여야 한다(8절 AES-GCM 복호 규칙과 같다).
// - 인자 규칙은 AesGcmSealDetached와 같다(plaintext는 ciphertext와 같은 주소이거나 겹치지 않는다).
//   tag 길이도 호출자가 16 byte로 맞춘다(잘린 tag는 호출자가 AuthenticationFailed로 거부한다).
[[nodiscard]] ROCKS_CRYPTO_API bool
AesGcmOpenDetached(const AesKernels& kernels, const AesSchedule& schedule, Span<const uint8> nonce,
                   Span<const uint8> associatedData, Span<const uint8> ciphertext,
                   Span<const uint8> tag, Span<uint8> plaintext) noexcept;

// ---- GCM 멀티파트(9.3절) ----

// GCM 멀티파트 상태다. 키 스케줄 사본과 키에서 파생된 값(GHASH 누적값, 남은 keystream)을 품으므로
// 키 저장소 슬롯에 둔다(1 KiB 등급). 키 객체와 수명이 독립이다(스케줄을 복사한다).
// - 단계: AD 흡수 → 데이터(dataStarted) → 끝. AD는 데이터 전에만 받는다(호출자가 검사한다).
// - pending·pendingBytes: 아직 GHASH에 넣지 않은 부분 블록이다. 데이터 단계에서는 암호문 byte이고
//   pendingBytes = messageBytes mod 16이다. keystream의 뒤 16 - pendingBytes byte가 그 블록의 남은
//   keystream이다.
struct AesGcmStream {
    AesSchedule schedule;
    AesBlock counter;
    AesBlock j0;
    AesBlock ghash;
    AesBlock keystream;
    AesBlock pending;
    uint64 associatedBytes;
    uint64 messageBytes;
    const AesKernels* kernels;
    uint32 pendingBytes;
    bool dataStarted;
};

// stream을 시작 상태로 만든다(source 스케줄 복사, J0 = nonce ‖ 1, 첫 데이터 카운터 2). nonce는 12
// byte여야 하고 kernels는 완전해야 한다(아니면 Fatal).
void AesGcmStreamStart(const AesKernels& kernels, const AesSchedule& source,
                       Span<const uint8> nonce, AesGcmStream& stream) noexcept;

// AD를 흡수한다. 데이터 단계 전이어야 하고 합이 AesGcmMaxAssociatedBytes 이하여야 한다(아니면
// Fatal).
void AesGcmStreamAbsorbAssociated(AesGcmStream& stream, Span<const uint8> associatedData) noexcept;

// 데이터를 암호화(encrypt true)하거나 복호한다. output.Size() == input.Size()이고 output은 input과
// 같은 주소이거나 겹치지 않는다. 합이 AesGcmMaxMessageBytes 이하여야 한다(아니면 Fatal). 복호는
// 암호문을 덮기 전에 GHASH에 넣는다(제자리).
void AesGcmStreamProcess(AesGcmStream& stream, bool encrypt, Span<const uint8> input,
                         Span<uint8> output) noexcept;

// tag(16 byte)를 계산해 쓴다. 뒤에 stream 전체(스케줄 사본 포함)를 0으로 지운다.
void AesGcmStreamFinish(AesGcmStream& stream, Span<uint8> tag) noexcept;

// ---- CTR(hazmat) ----

// counter(16 byte 카운터 블록)에서 width 폭으로 blocks개의 카운터를 더 쓸 수 있는가. 끝 width bit를
// big-endian 정수 c로 보면 남은 값은 2^width - c개다. exhausted(마지막 값을 이미 씀)면 0개다.
[[nodiscard]] bool AesCtrHasCapacity(const AesBlock& counter, AesCounterWidth width, bool exhausted,
                                     uint64 blocks) noexcept;

// blocks개를 쓰고 나면 마지막 카운터 값(폭의 모든 bit가 1)을 쓴 것인가. AesCtrHasCapacity가 참인
// blocks(1 이상)에만 부른다.
[[nodiscard]] bool AesCtrUsesLastCounter(const AesBlock& counter, AesCounterWidth width,
                                         uint64 blocks) noexcept;

// CTR 스트림 상태다. 키 스케줄 사본과 남은 keystream을 품으므로 키 저장소 슬롯에 둔다. 키 객체와
// 수명이 독립이다.
// - keystream의 뒤 keystreamBytes byte가 앞 호출에서 쓰고 남은 keystream이다(0–15).
// - exhausted: 폭 안의 마지막 카운터 값을 이미 썼다. 남은 keystream만 쓸 수 있다.
struct AesCtrStream {
    AesSchedule schedule;
    AesBlock counter;
    AesBlock keystream;
    const AesKernels* kernels;
    uint32 keystreamBytes;
    AesCounterWidth width;
    bool exhausted;
};

// stream을 시작 상태로 만든다(source 스케줄 복사, 카운터 = initialCounter). kernels는 완전해야
// 한다(아니면 Fatal).
void AesCtrStreamStart(const AesKernels& kernels, const AesSchedule& source,
                       const AesBlock& initialCounter, AesCounterWidth width,
                       AesCtrStream& stream) noexcept;

// bytes byte를 처리하는 데 새 keystream 블록이 몇 개 드는가(남은 keystream을 먼저 쓴다).
[[nodiscard]] uint64 AesCtrStreamBlocksFor(const AesCtrStream& stream, usize bytes) noexcept;

// input을 keystream으로 XOR해 output에 쓴다. output.Size() == input.Size()이고 output은 input과
// 같은 주소이거나 겹치지 않는다. 필요한 카운터가 남아 있어야 한다(AesCtrHasCapacity, 아니면 Fatal).
void AesCtrStreamProcess(AesCtrStream& stream, Span<const uint8> input,
                         Span<uint8> output) noexcept;

// ---- CMAC(RFC 4493, SP 800-38B) ----

// CMAC 누적 상태다. 키 스케줄은 따로 넘긴다(일회성은 키의 스케줄을 그대로 쓰고, 증분 상태는
// 사본을 함께 둔다). 0으로 채운 값이 시작 상태다.
// - pending·pendingBytes: 마지막 블록 후보(1–16 byte)다. CMAC은 마지막 블록에 서브키를 섞으므로
//   다음 입력이 올 때까지 CBC-MAC에 넣지 않는다.
struct AesCmacState {
    AesBlock chain;
    AesBlock pending;
    usize pendingBytes;
};

// message를 흡수한다.
void AesCmacUpdate(const AesKernels& kernels, const AesSchedule& schedule, AesCmacState& state,
                   Span<const uint8> message) noexcept;

// tag(16 byte)를 쓴다. 서브키 L = E_K(0^128), K1 = dbl(L), K2 = dbl(K1)을 분기 없이 만든다. 뒤에
// state와 서브키를 지운다.
void AesCmacFinish(const AesKernels& kernels, const AesSchedule& schedule, AesCmacState& state,
                   Span<uint8> tag) noexcept;

} // namespace rocks::crypto::detail
