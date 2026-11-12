#pragma once

#include <Platform/Math/ClangPlatformMath.h>
#include <Platform/Math/GenericPlatformMath.h>
#include <Platform/Math/NeonPlatformMath.h>
#include <Platform/Math/SsePlatformMath.h>
#include <Platform/Math/VectorRegisterNeon.h>
#include <Platform/Math/VectorRegisterScalar.h>
#include <Platform/Math/VectorRegisterSse.h>

namespace rocks::platform {

// 현재 컴파일 대상 CPU에 최적화된 scalar 수학과 4-lane 벡터 레지스터 구현을 선택한다.
// 구조는 Generic 기준 구현 → compiler 계층(Clang) → CPU 명령 계층(SSE2/NEON) 순서의 상속이다.
// Windows x64와 Intel macOS는 같은 SSE2 경로를, Apple Silicon은 NEON 경로를 사용한다.
// 지원 toolchain이 모두 Clang이므로 현재 OS별 차이는 없고, OS별 재정의가 필요해지면 같은
// 상속 단계를 추가한다. 알 수 없는 CPU는 정확성이 같은 Generic/scalar 구현을 사용한다.
//
// - PlatformMath: scalar 함수 모음. 사용 예: PlatformMath::Sqrt(x), PlatformMath::FloorToInt32(x)
// - simd:: 선택된 Register4f 연산. scalar::는 모든 CPU의 기준 구현으로 항상 존재한다.
// - ScalarSimdBitExact: 같은 입력에서 simd::와 scalar:: 결과가 bit 단위로 같은지 여부다(NaN은
//   NaN 여부만 같고 부호·payload는 다를 수 있다). x64 baseline에는 FMA 명령이 없어 true다.
//   빌드는 -ffp-contract=off로 compiler의 FMA 축약을 금지하지만, ARM64는 실기기로 확인하기
//   전까지 false로 두며 허용 오차로 비교한다.
#if defined(_M_X64) || defined(__x86_64__)
using PlatformMath = SsePlatformMath<ClangPlatformMath>;
namespace simd = sse;
inline constexpr const char* MathBackendName = "x64-SSE2";
inline constexpr bool ScalarSimdBitExact = true;
#elif defined(_M_ARM64) || defined(__aarch64__)
using PlatformMath = NeonPlatformMath<ClangPlatformMath>;
namespace simd = neon;
inline constexpr const char* MathBackendName = "arm64-NEON";
inline constexpr bool ScalarSimdBitExact = false;
#else
using PlatformMath = ClangPlatformMath;
namespace simd = scalar;
inline constexpr const char* MathBackendName = "generic-scalar";
inline constexpr bool ScalarSimdBitExact = true;
#endif

} // namespace rocks::platform
