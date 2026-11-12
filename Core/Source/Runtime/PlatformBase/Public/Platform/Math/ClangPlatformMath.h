#pragma once

#include <Platform/Math/GenericPlatformMath.h>

namespace rocks::platform {

// Rocks가 지원하는 Windows(clang-cl)와 macOS(Apple Clang) toolchain의 공통 compiler 계층이다.
// CPU 명령 선택은 compiler가 담당하는 bit 연산만 대체한다. 0 입력 결과는 Generic 계약과 같다.
struct ClangPlatformMath : GenericPlatformMath {
    using GenericPlatformMath::CountLeadingZeros32;
    using GenericPlatformMath::CountLeadingZeros64;
    using GenericPlatformMath::CountTrailingZeros32;
    using GenericPlatformMath::CountTrailingZeros64;
    using GenericPlatformMath::PopCount32;
    using GenericPlatformMath::PopCount64;

    // __builtin_clz/ctz의 0 입력은 정의되지 않으므로 먼저 분기한다.
    static constexpr uint32 CountLeadingZeros32(uint32 value) noexcept {
        return value == 0 ? 32u : static_cast<uint32>(__builtin_clz(value));
    }

    static constexpr uint32 CountLeadingZeros64(uint64 value) noexcept {
        return value == 0 ? 64u : static_cast<uint32>(__builtin_clzll(value));
    }

    static constexpr uint32 CountTrailingZeros32(uint32 value) noexcept {
        return value == 0 ? 32u : static_cast<uint32>(__builtin_ctz(value));
    }

    static constexpr uint32 CountTrailingZeros64(uint64 value) noexcept {
        return value == 0 ? 64u : static_cast<uint32>(__builtin_ctzll(value));
    }

    static constexpr uint32 PopCount32(uint32 value) noexcept {
        return static_cast<uint32>(__builtin_popcount(value));
    }

    static constexpr uint32 PopCount64(uint64 value) noexcept {
        return static_cast<uint32>(__builtin_popcountll(value));
    }
};

} // namespace rocks::platform
