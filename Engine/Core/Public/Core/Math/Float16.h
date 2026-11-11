#pragma once

#include <Core/Math/MathUtility.h>

namespace rocks {

// IEEE binary16 저장 값(UE FFloat16 대응)이다. 텍스처·정점·네트워크 압축 등 저장용이며 연산은
// float로 변환해서 수행한다. 변환은 platform::PlatformMath::FloatToHalf/HalfToFloat를 사용한다.
// - FromFloat: round-to-nearest-even. 크기 65520 이상은 ±Inf, NaN은 NaN(UE Set과 같은 규칙).
// - FromFloatClamped: 유한 범위 [-65504, 65504]로 포화한다. UE SetClamped는 FMath::Clamp를 거쳐
//   NaN이 최댓값이 될 수 있지만 Rocks는 NaN을 NaN으로 유지한다.
// - ToFloat: 항상 정확하다.
// operator==는 bit 비교다(+0과 -0은 다르고 같은 bit의 NaN은 같다). 값 비교는 ToFloat 후 수행한다.
struct Float16 {
    uint16 bits = 0;

    static constexpr Float16 FromBits(uint16 value) noexcept {
        Float16 result;
        result.bits = value;
        return result;
    }

    static Float16 FromFloat(float value) noexcept {
        return FromBits(platform::PlatformMath::FloatToHalf(value));
    }

    static Float16 FromFloatClamped(float value) noexcept {
        if (math::IsNaN(value)) {
            return FromFloat(value);
        }
        return FromFloat(math::Clamp(value, -MaxFinite, MaxFinite));
    }

    float ToFloat() const noexcept {
        return platform::PlatformMath::HalfToFloat(bits);
    }

    constexpr bool IsNaN() const noexcept {
        return (bits & 0x7c00u) == 0x7c00u && (bits & 0x3ffu) != 0;
    }

    constexpr bool IsFinite() const noexcept {
        return (bits & 0x7c00u) != 0x7c00u;
    }

    friend constexpr bool operator==(const Float16&, const Float16&) noexcept = default;

    static constexpr float MaxFinite = 65504.0f;
};

static_assert(sizeof(Float16) == 2);

} // namespace rocks
