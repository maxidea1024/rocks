#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Cpu.h>

// SHA-256 압축 커널(Docs/Design/CoreDataFoundations.ko.md 6절)이다.
// Sha256(Core/Hash/Sha256.h)이 이 프로세스의 커널 하나를 골라 쓰고, 시험은 커널마다 스칼라와
// 대조한다.
//
// 사용 예(시험):
//   const detail::Sha256Kernel kernel = detail::SelectSha256Kernel(platform::QueryCpuFeatures());
//   uint32 state[8] = {...초기값...};
//   detail::Sha256KernelBlocks(kernel)(state, blocks, count);
namespace rocks::detail {

// 64 byte 블록 count개를 차례로 state(8 word)에 압축한다. count가 0이면 아무것도 하지 않는다.
// blocks는 정렬하지 않아도 된다.
using Sha256BlocksFunction = void (*)(uint32* state, const uint8* blocks, usize count) noexcept;

enum class Sha256Kernel : uint8 {
    // 이식 가능한 스칼라 구현이다(모든 CPU).
    Scalar,
    // x64 SHA-NI. 조건: cpu.sha && cpu.ssse3 && cpu.sse41.
    X64Sha,
    // ARMv8 SHA2. 조건: cpu.armSha2.
    Arm64Sha2
};

// 커널 이름("Scalar"·"X64Sha"·"Arm64Sha2")이다. 진단과 시험 메시지에 쓴다.
[[nodiscard]] ROCKS_CORE_API const char* Sha256KernelName(Sha256Kernel kernel) noexcept;

// cpu가 이 빌드 대상 아키텍처의 하드웨어 커널 조건을 채우면 그 커널, 아니면 Scalar다. 자가 시험은
// 하지 않는다.
[[nodiscard]] ROCKS_CORE_API Sha256Kernel
SelectSha256Kernel(const platform::CpuFeatures& cpu) noexcept;

// kernel의 블록 함수다. 이 빌드 대상 아키텍처에 없는 커널(x64 빌드의 Arm64Sha2 등)은 nullptr다.
// 하드웨어 커널은 SelectSha256Kernel이 그 커널을 고른 CPU에서만 부를 수 있다(아니면 잘못된 명령
// 예외).
[[nodiscard]] ROCKS_CORE_API Sha256BlocksFunction Sha256KernelBlocks(Sha256Kernel kernel) noexcept;

// blocks를 알려진 답으로 자가 시험한다(결과가 틀린 커널을 거른다 — CPU가 기능 bit를 보고했는데
// 명령이 실제로 없으면 시험 중 잘못된 명령 예외로 끝난다): 한 블록("abc"), 두 블록(448 bit NIST
// 메시지), 여러 블록을 한 번에 넘긴 결과와 블록마다 넘긴 결과의 동치. 모두 맞으면 true다. nullptr는
// false다.
[[nodiscard]] ROCKS_CORE_API bool CheckSha256Kernel(Sha256BlocksFunction blocks) noexcept;

// 이 프로세스가 쓰는 커널이다. 첫 호출에 SelectSha256Kernel(QueryCpuFeatures())로 고르고 Scalar가
// 아니면 CheckSha256Kernel로 자가 시험한다. 자가 시험이 실패하면 Scalar를 쓰고 stderr에 한 줄을
// 남긴다. 여러 스레드가 동시에 처음 불러도 된다(함수 안 static 초기화). 결과는 프로세스 끝까지
// 같다.
[[nodiscard]] ROCKS_CORE_API Sha256Kernel ActiveSha256Kernel() noexcept;

// 한 블록의 메시지 word 묶음(4 word씩) 수다. 하드웨어 커널이 묶음마다 라운드 넷을 돈다.
inline constexpr int Sha256MessageGroups = 16;

// FIPS 180-4 4.2.2 라운드 상수 64개다(Sha256.cpp가 정의, 커널 파일이 같이 쓴다).
extern const uint32 Sha256RoundConstants[64];

// 스칼라 커널이다(Sha256.cpp).
ROCKS_CORE_API void Sha256BlocksScalar(uint32* state, const uint8* blocks, usize count) noexcept;

#if ROCKS_ARCH_X64
// SHA-NI 커널이다(Sha256X64.cpp). Sha256Kernel::X64Sha 조건을 채운 CPU에서만 부른다.
void Sha256BlocksX64Sha(uint32* state, const uint8* blocks, usize count) noexcept;
#elif ROCKS_ARCH_ARM64
// ARMv8 SHA2 커널이다(Sha256Arm64.cpp). Sha256Kernel::Arm64Sha2 조건을 채운 CPU에서만 부른다.
void Sha256BlocksArm64Sha2(uint32* state, const uint8* blocks, usize count) noexcept;
#endif

} // namespace rocks::detail
