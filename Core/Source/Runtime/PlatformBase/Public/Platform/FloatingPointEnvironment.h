#pragma once

#include <Base/Types.h>

// 결정적 부동소수점 계산(쿠킹·빌드 캐시 결과)이 기대하는 CPU 부동소수점 환경을 확인하고, 그
// 환경에서 계산을 실행한다.
//
// 기대 환경: 반올림 모드 nearest-even, 비정규 수 보존(FTZ·DAZ 끔), 부동소수점 예외 trap 꺼짐, 대체
// 동작 끔. x64는 MXCSR, ARM64는 FPCR로 판정하며 연산이 켜는 상태 플래그(MXCSR bit 0–5, ARM64
// FPSR)는 보지 않는다. 드라이버·미들웨어가 프로세스나 스레드의 환경을 바꿔 놓을 수 있으므로 결정적
// 결과가 필요한 작업은 RunWithDeterministicFloatingPoint 안에서 계산한다. 환경은 스레드별 상태다.
//
// 왜 RAII guard가 아닌가: compiler는 부동소수점 연산이 환경과 무관하다고 보고(FENV_ACCESS off) 같은
// 함수 안의 연산을 제어 레지스터 쓰기 앞뒤로 옮기거나 합친다. `#pragma STDC FENV_ACCESS ON`도
// inline된 다른 함수의 연산과 FTZ 변화는 지키지 못한다(clang 21에서 확인). 그래서 환경을 바꾼 뒤
// compiler가 들여다볼 수 없는 호출 경계(LTO도 대상을 알 수 없게 세탁한 함수 포인터)를 넘어 계산을
// 실행한다.
//
// 사용 예:
//   struct CookJob { const Texture* source; Result result; };
//   CookJob job{&source, {}};
//   platform::RunWithDeterministicFloatingPoint(
//       [](void* context) noexcept {
//           auto& job = *static_cast<CookJob*>(context);
//           job.result = CookTexture(*job.source);    // 이 안의 모든 연산이 기대 환경에서 실행된다
//       },
//       &job);
// 설계: Docs/Design/CoreDataFoundations.ko.md 2.1절.
namespace rocks::platform {

// 현재 스레드의 환경이 기대 환경인지다.
bool IsDeterministicFloatingPointEnvironment() noexcept;

// 현재 스레드의 제어 설정을 기대 환경으로 바꾸고 function(context)를 실행한 뒤, 제어 설정을 호출 전
// 값으로 되돌린다(function 안에서 누가 바꿨어도 되돌린다). 실행 중 쌓인 상태 플래그는 남긴다.
// 중첩해도 된다. 반환: 호출 전 환경이 기대 환경이 아니어서 바꿔야 했는지(진단용 — 누가 환경을 바꿔
// 놓았는지 찾을 때). function은 예외를 던지지 않아야 하며(엔진은 예외를 쓰지 않는다) 같은
// 스레드에서 끝나야 한다.
bool RunWithDeterministicFloatingPoint(void (*function)(void* context) noexcept,
                                       void* context) noexcept;

namespace detail {

// 시험용: 제어 레지스터(MXCSR 또는 FPCR) 값을 그대로 읽고 쓴다.
uint64 ReadFloatingPointControl() noexcept;
void WriteFloatingPointControl(uint64 value) noexcept;
// 비정규 수를 0으로 만드는 비트(x64 FTZ, ARM64 FZ)다.
uint64 FlushToZeroBit() noexcept;
// 비정규 입력을 0으로 보는 비트(x64 DAZ, ARM64는 FZ가 겸하므로 FZ)다.
uint64 DenormalsAreZeroBit() noexcept;
// 반올림 모드를 0 쪽(truncate)으로 바꾸는 비트 패턴이다(x64 RC = 11, ARM64 RMode = 11).
uint64 RoundTowardZeroBits() noexcept;
// 제어 설정이 아닌 상태 플래그 비트(x64 MXCSR bit 0–5, ARM64는 FPCR에 없으므로 0)다.
uint64 StatusFlagBits() noexcept;

} // namespace detail

} // namespace rocks::platform
