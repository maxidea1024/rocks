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
#error "Rocks supports only Clang-family compilers (clang, clang-cl, Apple Clang, NDK clang)."
#endif

#if !defined(__cpp_concepts) || __cpp_concepts < 202002L || !defined(__cpp_if_consteval)
#error "Rocks requires C++23 (concepts, if consteval)."
#endif

// 대상 CPU 아키텍처 매크로다. 둘 중 정확히 하나가 1이다. 컴파일러가 정하는 원시 매크로
// (__x86_64__·_M_X64·__aarch64__·_M_ARM64 등)는 이 파일에서만 쓴다. 다른 코드는 전처리기
// 분기에 ROCKS_ARCH_*를, 함수 안에서는 if constexpr로 rocks::IsX64·IsArm64를 쓴다.
// 주의: 이 헤더가 정의하므로 쓰는 파일은 그보다 앞에 #include <Base/Compiler.h>를 둔다(정의
// 전에 쓰면 경고 없이 0이다). #ifdef·defined()로 묻지 않는다(항상 정의된다). 정책 검사
// target-macro·target-macro-order·target-macro-defined가 확인한다.
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
#error "Rocks supports only 64-bit x64 or ARM64."
#endif

// 대상 OS 매크로다. 빌드 시스템(Rocks.Build)이 정확히 하나를 1로 정의한다(ROCKS_PLATFORM_WINDOWS·
// ROCKS_PLATFORM_MAC·ROCKS_PLATFORM_ANDROID). 정의되지 않은 매크로는 0으로 본다. 합이 1이 아니거나
// 컴파일러가 정하는 대상(_WIN32·__APPLE__·__ANDROID__)과 어긋나면 빌드 오류다.
// - OS API를 쓰는 분기(#if ROCKS_PLATFORM_*)는 native backend 디렉터리
//   (PlatformBase/Private/<플랫폼> 등)에만 둔다(정책 검사 platform-boundary). OS별 구현은
//   디렉터리로 나누는 것이 기본이다.
// - 테스트·공통 코드의 플랫폼별 기대값은 if constexpr로 rocks::IsWindows·IsMac·IsAndroid를 쓴다.
// - 원시 OS 매크로(_WIN32·__APPLE__·__ANDROID__·__linux__ 등)는 이 파일에서만 쓴다.
// - ROCKS_PLATFORM_MAC은 macOS 대상만이다. iOS 같은 다른 Apple 대상은 아직 지원하지 않아 거부한다.
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
    "Exactly one of ROCKS_PLATFORM_WINDOWS, ROCKS_PLATFORM_MAC and ROCKS_PLATFORM_ANDROID must be 1 (set by Rocks.Build)."
#endif

#if (ROCKS_PLATFORM_WINDOWS && !defined(_WIN32)) ||                                                \
    (ROCKS_PLATFORM_MAC &&                                                                         \
     (!defined(__APPLE__) || !defined(__ENVIRONMENT_MAC_OS_X_VERSION_MIN_REQUIRED__))) ||          \
    (ROCKS_PLATFORM_ANDROID && !defined(__ANDROID__))
#error "ROCKS_PLATFORM_* does not match the target OS (ROCKS_PLATFORM_MAC is macOS only)."
#endif

// 빌드 구성 매크로다(UE5.8 UE_BUILD_DEBUG·UE_BUILD_DEVELOPMENT·UE_BUILD_SHIPPING 대응). 빌드
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
    "Exactly one of ROCKS_BUILD_DEBUG, ROCKS_BUILD_DEVELOPMENT and ROCKS_BUILD_RELEASE must be 1 (set by Rocks.Build)."
#endif

// 링크 방식이다(Docs/Design/BuildTargets.ko.md 3.8절 BD17, UE IS_MONOLITHIC). 빌드 시스템이 타깃
// 하나에 한 값(1 Monolithic, 0 Modular)을 모든 번역 단위에 정의한다. 기본값이 없다(빠지면 컴파일
// 오류).
#if !defined(ROCKS_MONOLITHIC) || (ROCKS_MONOLITHIC != 0 && ROCKS_MONOLITHIC != 1)
#error "ROCKS_MONOLITHIC must be defined to 0 or 1 by Rocks.Build."
#endif

// 공유 라이브러리 export·import 철자다(BuildTargets 3.8절 BD27). 모듈 API 매크로
// ROCKS_<MODULE>_API는 빌드가 모듈 정의 헤더에 넣고(Monolithic이면 빈 값), 같은 철자를 쓴다. 모듈
// API 매크로가 아닌 곳(Modular 진입점 등)이 직접 쓴다.
#if ROCKS_PLATFORM_WINDOWS
#define ROCKS_DLLEXPORT __declspec(dllexport)
#define ROCKS_DLLIMPORT __declspec(dllimport)
#else
#define ROCKS_DLLEXPORT __attribute__((visibility("default")))
#define ROCKS_DLLIMPORT __attribute__((visibility("default")))
#endif

// AddressSanitizer 계측 여부다. Asan 구성에서 계측하는 번역 단위만 1이다(계측을 끈 외부
// 라이브러리는 0). 자체 할당기의 수동 poison(Core/Memory/AddressSanitizer.h)이 쓴다. 기능 선택에는
// 쓰지 않는다.
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ROCKS_ADDRESS_SANITIZER 1
#endif
#endif

#ifndef ROCKS_ADDRESS_SANITIZER
#define ROCKS_ADDRESS_SANITIZER 0
#endif

// 이 함수 본문의 메모리 접근을 AddressSanitizer 계측에서 뺀다(Asan 구성이 아니면 아무 영향이 없다).
// 자체 할당기가 수동 poison(Core/Memory/AddressSanitizer.h)으로 닫아 둔 메모리 안의 자기 관리
// word(자유 목록 연결·반환 표지)를 읽고 쓰는 작은 내부 함수에만 쓴다.
// - 본문에서 직접 하는 읽기·쓰기만 빠진다. 이 함수가 부르는 다른 함수(인라인되지 않은 계측 함수)의
//   접근은 그대로 검사되므로, 닫힌 메모리는 이 함수 안에서 직접 건드린다.
// - 사용자 메모리를 다루는 일반 코드에 붙이지 않는다(그 함수의 오류를 ASan이 놓친다).
//
// 사용 예(Core/Private/Memory/BinnedAllocator.cpp):
//   ROCKS_NO_SANITIZE_ADDRESS void* ReadLink(void* block) noexcept {
//       return static_cast<FreeBlock*>(block)->next;   // 닫힌 자유 블록의 연결 word
//   }
#define ROCKS_NO_SANITIZE_ADDRESS __attribute__((no_sanitize("address")))

// 인라인하지 않는다. 드문 오류 경로·큰 진단 함수를 hot path 밖으로 뺄 때 쓴다.
#define ROCKS_NOINLINE __attribute__((noinline))

// 이 함수를 부른 호출자의 복귀 주소다(const void*). 치명 오류 보고가 발생 위치를 남길 때
// 쓴다(부르는 함수는 ROCKS_NOINLINE이어야 호출자가 바뀌지 않는다).
#define ROCKS_RETURN_ADDRESS() static_cast<const void*>(__builtin_return_address(0))

// 도달할 수 없는 지점이다. 도달하면 정의되지 않은 동작이므로 계약 위반 검사(Fatal) 대신 쓰지
// 않는다.
#define ROCKS_UNREACHABLE() __builtin_unreachable()

// 연결된 디버거에 중단점을 건다. 디버거가 없으면 프로세스가 종료될 수 있다.
#define ROCKS_DEBUG_BREAK() __builtin_debugtrap()

// 표준 배치 타입 Type 안에서 member의 byte offset이다(offsetof 대응, 상수 식).
#define ROCKS_OFFSET_OF(Type, member) __builtin_offsetof(Type, member)

// 이 지점부터 ROCKS_ENABLE_OPTIMIZATION까지 정의하는 함수의 최적화를 끈다(UE5.8
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

// AES 명령 경로를 함수 하나에만 켜는 target 속성이다(Docs/Design/Crypto.ko.md 5.3절). 전역 -maes·
// -march 없이 이 속성을 붙인 함수만 AES·carry-less 곱셈 명령으로 컴파일한다.
//   ROCKS_TARGET_X64_AES    AES-NI·PCLMULQDQ·AVX. libsodium aesni 원본과 같다. avx가 SSSE3·SSE4.1을
//                           함께 켜고(_mm_shuffle_epi8) 속성 함수 안의 SSE 명령을 모두 VEX로
//                           바꾼다. ROCKS_ARCH_X64에서만 정의한다.
//   ROCKS_TARGET_ARM64_AES  NEON·AES(AESE·AESMC·PMULL)와 AESE·AESMC 융합 조율. libsodium armcrypto
//                           원본의 crypto(SHA2까지 켠다)는 빼서 감지 조건과 같은 범위만 켠다.
//                           ROCKS_ARCH_ARM64에서만 정의한다.
// - PlatformBase CpuFeatures가 다음을 보고할 때만 부른다. 아니면 잘못된 명령 예외로 죽는다. x64:
//   aes && pclmulqdq && avx(avx는 OS의 YMM 상태 저장 확인 포함), arm64: armAes && armPmull. 그래서
//   이 속성 함수는 감지한 뒤 고른 커널 표를 거쳐서만 부른다.
// - 속성이 없는 호출자로 inline된다고 기대하지 않는다(대상 기본 CPU가 기능을 이미 포함할 때만
//   inline될 수 있다. 예: macOS arm64). 블록 하나가 아니라 버퍼 단위로 경계를 둔다.
// - 다른 아키텍처에서는 정의하지 않으므로 쓰는 파일은 #if ROCKS_ARCH_*로 감싼다(잘못 쓰면 컴파일
//   오류다).
// - 정책 검사 target-intrinsics: 이 매크로와 AES·carry-less 곱셈 intrinsics는 Crypto/Private/Aes/
//   에서만 쓴다. target-attribute: target 속성(__attribute__((target(...)))·#pragma clang attribute
//   등)은 이 파일 밖에서 직접 쓰지 않는다.
//
// 사용 예(Crypto/Private/Aes/AesX64.cpp):
//   #include <Base/Compiler.h>
//   #if ROCKS_ARCH_X64
//   #include <immintrin.h>
//   ROCKS_TARGET_X64_AES static __m128i EncryptRound(__m128i block, __m128i key) noexcept {
//       return _mm_aesenc_si128(block, key);
//   }
//   #endif
#if ROCKS_ARCH_X64
#define ROCKS_TARGET_X64_AES __attribute__((target("aes,avx,pclmul")))
#elif ROCKS_ARCH_ARM64
#define ROCKS_TARGET_ARM64_AES __attribute__((target("neon,aes,fuse-aes")))
#endif

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

// 대상 CPU의 캐시 라인 크기(byte)다(UE5.8 PLATFORM_CACHE_LINE_SIZE 대응). 여러 스레드가 따로 쓰는
// 원자 변수·카운터를 alignas(CacheLineSize)로 떼어 false sharing을 막는 데 쓴다. 메모리 할당 정렬의
// 의미는 없다(할당기는 자체 정렬 계약을 따른다).
// - Windows x64: 64 (UE Engine/Source/Runtime/Core/Public/Windows/WindowsPlatform.h:206)
// - macOS arm64: 128 (UE Engine/Source/Runtime/Core/Public/Mac/MacPlatform.h:63, Apple Silicon)
// - Android arm64: 64 (UE는 Android 값을 따로 두지 않고 기본값을 쓴다,
//   Engine/Source/Runtime/Core/Public/HAL/Platform.h:990)
//
// 사용 예:
//   struct alignas(CacheLineSize) PaddedCounter {
//       uint64 value = 0; // platform::AtomicLoad·AtomicStore로 접근
//   };
inline constexpr usize CacheLineSize = IsMac && IsArm64 ? 128 : 64;

static_assert(IsX64 != IsArm64, "exactly one supported CPU architecture must be selected");
static_assert(sizeof(void*) == 8, "Rocks requires a 64-bit target");

} // namespace rocks
