#pragma once

#include <Base/Types.h>

// 컴파일러·언어·CPU 아키텍처 확인과 컴파일러 속성 매크로다. 지원하지 않는 환경은 빌드 오류로
// 드러낸다.
//
// 지원 환경: Clang 계열(clang·clang-cl·Apple Clang·Android NDK clang), C++23, x64 또는
// ARM64(64-bit).
//
// 속성 매크로는 측정으로 필요가 확인된 경우에만 쓴다. 성능은 기본적으로 빌드 구성(최적화
// 수준·ThinLTO·모듈 단위 최적화)으로 얻으며 함수마다 붙이지 않는다.

#if !defined(__clang__)
#error "Rocks는 Clang 계열 컴파일러만 지원한다(clang, clang-cl, Apple Clang, NDK clang)."
#endif

#if !defined(__cpp_concepts) || __cpp_concepts < 202002L || !defined(__cpp_if_consteval)
#error "Rocks는 C++23(concepts, if consteval)이 필요하다."
#endif

#if !defined(__x86_64__) && !defined(_M_X64) && !defined(__aarch64__) && !defined(_M_ARM64)
#error "Rocks는 64-bit x64 또는 ARM64만 지원한다."
#endif

// 항상 인라인한다. 디버그 빌드(-O0)에서도 펼친다. 측정한 hot path에만 쓴다.
#define ROCKS_FORCEINLINE inline __attribute__((always_inline))

// 인라인하지 않는다. 드문 오류 경로·큰 진단 함수를 hot path 밖으로 뺄 때 쓴다.
#define ROCKS_NOINLINE __attribute__((noinline))

// 도달할 수 없는 지점이다. 도달하면 정의되지 않은 동작이므로 계약 위반 검사(Fatal) 대신 쓰지
// 않는다.
#define ROCKS_UNREACHABLE() __builtin_unreachable()

// 연결된 디버거에 중단점을 건다. 디버거가 없으면 프로세스가 종료될 수 있다.
#define ROCKS_DEBUG_BREAK() __builtin_debugtrap()

namespace rocks {

// 컴파일 대상 CPU 아키텍처다.
inline constexpr bool IsX64 =
#if defined(__x86_64__) || defined(_M_X64)
    true;
#else
    false;
#endif

inline constexpr bool IsArm64 =
#if defined(__aarch64__) || defined(_M_ARM64)
    true;
#else
    false;
#endif

static_assert(IsX64 != IsArm64, "exactly one supported CPU architecture must be selected");
static_assert(sizeof(void*) == 8, "Rocks requires a 64-bit target");

} // namespace rocks
