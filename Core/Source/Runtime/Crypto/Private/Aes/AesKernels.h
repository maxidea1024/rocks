#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <Platform/Cpu.h>

// AES 경량 계층의 키 스케줄과 커널 표다(Docs/Design/Crypto.ko.md 5.2·5.3절). 모듈 내부 헤더이며
// intrinsics 헤더를 포함하지 않는다. 커널 구현은 AesX64.cpp(AES-NI·PCLMULQDQ)와
// AesArm64.cpp(AESE·AESMC·PMULL)이고, 아키텍처와 무관한 모드(GCM 등)는 AesModes.cpp가 이 표로
// 조립한다(AesModesInternal.h).
//
// - 커널은 감지한 CPU에서만 돈다. 지원하지 않는 CPU에서 부르면 잘못된 명령 예외로 죽으므로 표는
//   GetAesKernels()(감지·자가 시험을 거친 표)로만 얻는다. 시험은 값을 받는 SelectAesKernels·
//   CheckAesKernels로 HW 없는 경로와 자가 시험 실패 경로를 재현한다(11절 결정성).
// - 커널은 블록 하나가 아니라 버퍼 단위다. target 속성 함수는 속성이 없는 호출자로 inline되지 않아,
//   블록마다 간접 호출하면 느려진다(5.3절).
namespace rocks::crypto::detail {

inline constexpr usize AesBlockBytes = 16;

// AES-256의 라운드 수다(AES-128은 10).
inline constexpr usize AesMaxRounds = 14;

// GHASH 키 H의 거듭제곱(H^1, H^2, ...)을 담는 칸 수다. x64 커널은 14개(병렬 7블록 × 2), arm64
// 커널은 12개(6블록 × 2)를 쓴다.
inline constexpr usize AesGhashPowerCapacity = 16;

// AES 블록 하나(16 byte)다. 커널이 정렬된 vector load를 쓸 수 있게 16 byte로 정렬한다.
struct alignas(16) AesBlock {
    uint8 bytes[AesBlockBytes];
};

// 확장한 AES 키다. AES 키를 만들 때 한 번 계산하고(ExpandKey) 이후 바꾸지 않으므로 여러 스레드가
// 동시에 읽어도 된다.
// - roundKeys[0..rounds]: FIPS-197 라운드 키를 byte 순서 그대로 둔다(두 아키텍처가 같은 배치다).
//   나머지 칸은 0이다.
// - hPowers: GHASH 키 H = E_K(0^128)의 거듭제곱이다. 커널 내부 표현(byte 역순, 한 bit 왼쪽으로 민
//   값)이라 이 스케줄을 만든 커널 표만 해석한다.
// - rounds: 10(AES-128) 또는 14(AES-256)다.
// 라운드 키와 H는 비밀이므로 스케줄은 키 저장소(SecretArena) 슬롯에 둔다(TryCreateAesSchedule).
// 시험·자가 시험처럼 공개 키로 스택에 만들 때도 쓴 뒤 지운다.
struct alignas(16) AesSchedule {
    AesBlock roundKeys[AesMaxRounds + 1];
    AesBlock hPowers[AesGhashPowerCapacity];
    uint32 rounds;
};

static_assert(sizeof(AesSchedule) <= 512, "AesSchedule must fit a 512-byte key store slot");

// CTR 카운터의 증가 폭이다. 카운터 블록의 끝 32·64·128 bit를 big-endian 정수로 보고 1씩 더한다.
// 올림은 그 폭 안에서만 전파하고 2^폭에서 0으로 돌며, 앞 byte는 바뀌지 않는다. GCM은 Bits32(inc32,
// SP 800-38D 6.2)이고, CTR hazmat은 호출자가 고른다(기본 Bits128). 한 바퀴 도는 것(카운터 재사용)은
// 커널이 막지 않는다. 호출자(AesModes.cpp)가 블록 수를 세어 미리 거부한다.
enum class AesCounterWidth : uint8 {
    Bits32,
    Bits64,
    Bits128,
};

// 아키텍처별 커널 다섯 개다(5.3절). 모든 함수는 버퍼 단위이고 입력·출력은 정렬되지 않아도 된다.
// 스케줄은 같은 표의 expandKey가 만든 것만 넘긴다. 빈 표(모든 포인터가 nullptr)는 "AES 없음"이다.
struct AesKernels {
    // key(keyBytes = 16 또는 32)를 확장해 schedule 전체를 쓴다(라운드 키, H 거듭제곱, rounds).
    // 다른 길이는 Fatal이다(호출자가 검사한다).
    void (*expandKey)(const uint8* key, usize keyBytes, AesSchedule& schedule) noexcept = nullptr;

    // CTR 암·복호다. counter(big-endian 카운터 블록)부터 ⌈bytes / 16⌉개의 keystream 블록
    // E_K(counter_i)로 input을 XOR해 output에 쓰고, counter를 다음에 쓸 값으로 바꾼다. 증가는
    // width(AesCounterWidth)를 따른다. 마지막 부분 블록은 keystream 앞부분만 쓰고 그 블록의 나머지
    // keystream은 버린다(카운터는 그 블록만큼 올라간다). output은 input과 같은 주소(제자리)이거나
    // 겹치지 않아야 한다.
    void (*ctrXor)(const AesSchedule& schedule, AesCounterWidth width, AesBlock& counter,
                   const uint8* input, uint8* output, usize bytes) noexcept = nullptr;

    // GHASH 누적이다. state(GCM 명세 byte 순서의 누적값 X)에 data를 16 byte 블록으로 흡수한다:
    // X ← (X xor B_i) · H. 마지막 부분 블록은 0으로 채운 블록으로 흡수한다. 그래서 여러 번 나눠
    // 부를 때는 마지막 호출만 16의 배수가 아니어야 같은 결과가 난다.
    void (*ghashUpdate)(const AesSchedule& schedule, AesBlock& state, const uint8* data,
                        usize bytes) noexcept = nullptr;

    // CBC-MAC 누적이다(CMAC 몫). data의 블록 blocks개마다 state ← E_K(state xor B_i)다. data는
    // 16 × blocks byte다.
    void (*cbcMacUpdate)(const AesSchedule& schedule, AesBlock& state, const uint8* data,
                         usize blocks) noexcept = nullptr;

    // 블록 하나를 암호화한다(GCM J0, CMAC 서브키 L 같은 일회성 용도만). output은 input과 같아도
    // 된다.
    void (*encryptBlock)(const AesSchedule& schedule, const AesBlock& input,
                         AesBlock& output) noexcept = nullptr;

    // 구현 이름이다(예: "rocks-aes/x64"). 정적 수명이다.
    StringView name;

    // 다섯 함수가 모두 있는가. 빈 표나 일부만 채운 표는 쓰지 않는다.
    bool IsComplete() const noexcept {
        return expandKey != nullptr && ctrXor != nullptr && ghashUpdate != nullptr &&
               cbcMacUpdate != nullptr && encryptBlock != nullptr;
    }
};

// cpu가 AES 경량 계층과 libsodium AES 경로의 공통 HW 조건을 채우는가(6.1절 "HW 조건은 하나다").
// x64는 AES-NI·PCLMULQDQ·AVX(AVX는 OS의 YMM 상태 확인 포함), arm64는 AES·PMULL이다.
[[nodiscard]] ROCKS_CRYPTO_API bool HasAesInstructions(const platform::CpuFeatures& cpu) noexcept;

// 이 빌드 대상 아키텍처의 커널 표다(감지·자가 시험 전, 정적 수명). AesX64.cpp 또는 AesArm64.cpp가
// 정의한다. HasAesInstructions가 참인 CPU에서만 표의 함수를 부를 수 있다.
[[nodiscard]] ROCKS_CRYPTO_API const AesKernels& GetNativeAesKernels() noexcept;

// cpu가 HW 조건을 채우면 이 아키텍처의 커널 표, 아니면 빈 표다. 자가 시험은 하지 않는다.
[[nodiscard]] ROCKS_CRYPTO_API AesKernels
SelectAesKernels(const platform::CpuFeatures& cpu) noexcept;

// candidate로 자가 시험(GCM 알려진 답 AES-128·AES-256, CBC-MAC 알려진 답)을 돌려 모두 맞으면
// candidate, 하나라도 다르거나 표가 완전하지 않으면 빈 표다. 로그는 남기지 않는다. candidate의
// 함수는 이 CPU에서 실행할 수 있어야 한다(SelectAesKernels의 결과나 시험이 만든 표).
[[nodiscard]] ROCKS_CRYPTO_API AesKernels CheckAesKernels(const AesKernels& candidate) noexcept;

// 이 프로세스가 쓰는 표다. 첫 호출에 SelectAesKernels(QueryCpuFeatures())로 고르고
// CheckAesKernels로 자가 시험한다. 자가 시험이 실패하면 빈 표가 되고 stderr에 한 줄을 남긴다. 그 뒤
// 모든 AES 경량 계층 호출은 Unsupported다. 여러 스레드가 동시에 처음 불러도 된다(함수 안 static
// 초기화). 결과는 프로세스 끝까지 같다.
[[nodiscard]] ROCKS_CRYPTO_API const AesKernels& GetAesKernels() noexcept;

} // namespace rocks::crypto::detail
