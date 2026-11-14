#pragma once

#include <Base/Types.h>

// 컴파일러·언어·CPU 아키텍처 확인과 컴파일러 속성 매크로다. 지원하지 않는 환경은 빌드 오류로
// 드러낸다.
//
// 지원 환경: Clang 계열(clang·clang-cl·Apple Clang·Android NDK clang), C++23, x64 또는
// ARM64(64-bit).
//
// 강제 인라인(UE FORCEINLINE 대응)은 제공하지 않는다. 인라인 판단은 Clang 최적화(Development·
// Release의 -O2, Release의 ThinLTO 모듈 간 인라인)에 맡기며, always_inline·__forceinline은 정책
// 검사가 거부한다. 성능 문제는 측정한 뒤 알고리즘·데이터 배치·모듈 최적화 설정으로 다룬다.

#if !defined(__clang__)
#error "Rocks는 Clang 계열 컴파일러만 지원한다(clang, clang-cl, Apple Clang, NDK clang)."
#endif

#if !defined(__cpp_concepts) || __cpp_concepts < 202002L || !defined(__cpp_if_consteval)
#error "Rocks는 C++23(concepts, if consteval)이 필요하다."
#endif

// 대상 CPU 아키텍처 매크로다. 둘 중 정확히 하나가 1이다. 컴파일러가 정하는 원시
// 매크로(__x86_64__·_M_X64·
// __aarch64__·_M_ARM64)는 이 파일에서만 쓰고, 다른 코드는 전처리기 분기에 ROCKS_ARCH_*를, 함수
// 안에서는 if constexpr로 rocks::IsX64·IsArm64를 쓴다(정책 검사 target-macro).
//
// 사용 예:
//   #if ROCKS_ARCH_X64
//   #include <immintrin.h>
//   #endif
#if defined(__x86_64__) || defined(_M_X64)
#define ROCKS_ARCH_X64 1
#define ROCKS_ARCH_ARM64 0
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ROCKS_ARCH_X64 0
#define ROCKS_ARCH_ARM64 1
#else
#error "Rocks는 64-bit x64 또는 ARM64만 지원한다."
#endif

// 대상 OS 매크로다. 빌드 시스템(Rocks.Build)이 정확히 하나를 1로 정의한다(ROCKS_PLATFORM_WINDOWS·
// ROCKS_PLATFORM_MAC·ROCKS_PLATFORM_ANDROID). 정의되지 않은 매크로는 0으로 본다. 합이 1이 아니거나
// 컴파일러가 정하는 대상(_WIN32·__APPLE__·__ANDROID__)과 어긋나면 빌드 오류다.
// - OS API를 쓰는 분기(#if ROCKS_PLATFORM_*)는 native backend
// 디렉터리(PlatformBase/Private/<플랫폼> 등)에만
//   둔다(정책 검사 platform-boundary). OS별 구현은 디렉터리로 나누는 것이 기본이다.
// - 테스트·공통 코드의 플랫폼별 기대값은 if constexpr로 rocks::IsWindows·IsMac·IsAndroid를 쓴다.
// - 원시 OS 매크로(_WIN32·__APPLE__·__ANDROID__·__linux__)는 이 파일에서만 쓴다(정책 검사
// target-macro).
//
// 사용 예:
//   if constexpr (rocks::IsMac && rocks::IsArm64) {
//       CHECK(features.armAes);   // Apple Silicon은 모두 AES 명령이 있다
//   }
#ifndef ROCKS_PLATFORM_WINDOWS
#define ROCKS_PLATFORM_WINDOWS 0
#endif

#ifndef ROCKS_PLATFORM_MAC
#define ROCKS_PLATFORM_MAC 0
#endif

#ifndef ROCKS_PLATFORM_ANDROID
#define ROCKS_PLATFORM_ANDROID 0
#endif

#if (ROCKS_PLATFORM_WINDOWS != 0 && ROCKS_PLATFORM_WINDOWS != 1) ||                                \
    (ROCKS_PLATFORM_MAC != 0 && ROCKS_PLATFORM_MAC != 1) ||                                        \
    (ROCKS_PLATFORM_ANDROID != 0 && ROCKS_PLATFORM_ANDROID != 1) ||                                \
    ROCKS_PLATFORM_WINDOWS + ROCKS_PLATFORM_MAC + ROCKS_PLATFORM_ANDROID != 1
#error                                                                                             \
    "ROCKS_PLATFORM_WINDOWS·ROCKS_PLATFORM_MAC·ROCKS_PLATFORM_ANDROID 중 정확히 하나가 1이어야 한다(Rocks.Build가 정의한다)."
#endif

#if (ROCKS_PLATFORM_WINDOWS && !defined(_WIN32)) || (ROCKS_PLATFORM_MAC && !defined(__APPLE__)) || \
    (ROCKS_PLATFORM_ANDROID && !defined(__ANDROID__))
#error "ROCKS_PLATFORM_*가 컴파일 대상 OS와 다르다."
#endif

// 빌드 구성 매크로다(UE5.6 UE_BUILD_DEBUG·UE_BUILD_DEVELOPMENT·UE_BUILD_SHIPPING 대응). 빌드
// 시스템(Rocks.Build)이 구성에 따라 정확히 하나를 1로 정의한다. 모듈이 직접 정의할 수 없다.
//   ROCKS_BUILD_DEBUG        Debug·Asan 구성. 기본은 최적화 없음.
//   ROCKS_BUILD_DEVELOPMENT  Development 구성. 최적화와 디버그 정보, 개발용 기능을 모두 쓴다.
//   ROCKS_BUILD_RELEASE      Release 구성(UE Shipping). 배포용이며 개발용 기능을 뺀다.
// 구성 매크로는 기능 선택용이다. 실제 최적화 여부는 모듈 재정의(CodeOptimization.Always·Never)로
// 구성과 달라질 수 있으므로 이 매크로로 추론하지 않는다.
// 계약 위반 검사(Fatal)는 모든 구성에서 유지한다. 구성 매크로로 끄는 것은 디버그 표시·치트·통계
// 같은 개발용 기능뿐이다.
//
// 사용 예:
//   #if !ROCKS_BUILD_RELEASE
//   DrawDebugBounds(actor);          // 배포 빌드에는 컴파일되지 않는다
//   #endif
//   if constexpr (rocks::IsReleaseBuild) { ... }
//
// 정의되지 않은 매크로는 0으로 본다. 합이 1이 아니면(빌드 시스템 밖에서 컴파일하거나 모듈이 잘못
// 정의하면) 빌드 오류다.
#ifndef ROCKS_BUILD_DEBUG
#define ROCKS_BUILD_DEBUG 0
#endif

#ifndef ROCKS_BUILD_DEVELOPMENT
#define ROCKS_BUILD_DEVELOPMENT 0
#endif

#ifndef ROCKS_BUILD_RELEASE
#define ROCKS_BUILD_RELEASE 0
#endif

#if (ROCKS_BUILD_DEBUG != 0 && ROCKS_BUILD_DEBUG != 1) ||                                          \
    (ROCKS_BUILD_DEVELOPMENT != 0 && ROCKS_BUILD_DEVELOPMENT != 1) ||                              \
    (ROCKS_BUILD_RELEASE != 0 && ROCKS_BUILD_RELEASE != 1) ||                                      \
    ROCKS_BUILD_DEBUG + ROCKS_BUILD_DEVELOPMENT + ROCKS_BUILD_RELEASE != 1
#error                                                                                             \
    "ROCKS_BUILD_DEBUG·ROCKS_BUILD_DEVELOPMENT·ROCKS_BUILD_RELEASE 중 정확히 하나가 1이어야 한다(Rocks.Build가 정의한다)."
#endif

// 인라인하지 않는다. 드문 오류 경로·큰 진단 함수를 hot path 밖으로 뺄 때 쓴다.
#define ROCKS_NOINLINE __attribute__((noinline))

// 도달할 수 없는 지점이다. 도달하면 정의되지 않은 동작이므로 계약 위반 검사(Fatal) 대신 쓰지
// 않는다.
#define ROCKS_UNREACHABLE() __builtin_unreachable()

// 연결된 디버거에 중단점을 건다. 디버거가 없으면 프로세스가 종료될 수 있다.
#define ROCKS_DEBUG_BREAK() __builtin_debugtrap()

// 표준 배치 타입 Type 안에서 member의 byte offset이다(offsetof 대응, 상수 식).
#define ROCKS_OFFSET_OF(Type, member) __builtin_offsetof(Type, member)

// 이 지점부터 ROCKS_ENABLE_OPTIMIZATION까지 정의하는 함수의 최적화를 끈다(UE5.6
// UE_DISABLE_OPTIMIZATION·UE_ENABLE_OPTIMIZATION 대응). Development 빌드에서 파일 일부만 단계
// 실행하려고 임시로 쓴다. 모듈 전체는 규칙의 Optimization => CodeOptimization.Never로 끈다.
// - 제품 소스에 커밋하지 않는다(정책 검사가 거부한다. 테스트 소스는 예외).
// - Release 빌드에서는 컴파일 오류다.
//
// 사용 예:
//   ROCKS_DISABLE_OPTIMIZATION
//   void UpdateSuspiciousSystem(World& world) { ... }   // 변수가 최적화로 사라지지 않는다
//   ROCKS_ENABLE_OPTIMIZATION
#if ROCKS_BUILD_RELEASE
#define ROCKS_DISABLE_OPTIMIZATION                                                                 \
    _Pragma("GCC error \"ROCKS_DISABLE_OPTIMIZATION is not allowed in Release builds\"")
#else
#define ROCKS_DISABLE_OPTIMIZATION _Pragma("clang optimize off")
#endif

#define ROCKS_ENABLE_OPTIMIZATION _Pragma("clang optimize on")

namespace rocks {

// 컴파일 대상 CPU 아키텍처다(위 ROCKS_ARCH_* 매크로와 같다).
inline constexpr bool IsX64 = ROCKS_ARCH_X64 != 0;
inline constexpr bool IsArm64 = ROCKS_ARCH_ARM64 != 0;

// 컴파일 대상 OS다(위 ROCKS_PLATFORM_* 매크로와 같다). 함수 안에서는 if constexpr로 분기한다.
inline constexpr bool IsWindows = ROCKS_PLATFORM_WINDOWS != 0;
inline constexpr bool IsMac = ROCKS_PLATFORM_MAC != 0;
inline constexpr bool IsAndroid = ROCKS_PLATFORM_ANDROID != 0;

// 빌드 구성이다(위 ROCKS_BUILD_* 매크로와 같다). 함수 안에서는 if constexpr로 분기한다.
inline constexpr bool IsDebugBuild = ROCKS_BUILD_DEBUG != 0;
inline constexpr bool IsDevelopmentBuild = ROCKS_BUILD_DEVELOPMENT != 0;
inline constexpr bool IsReleaseBuild = ROCKS_BUILD_RELEASE != 0;

static_assert(IsX64 != IsArm64, "exactly one supported CPU architecture must be selected");
static_assert(sizeof(void*) == 8, "Rocks requires a 64-bit target");

} // namespace rocks
