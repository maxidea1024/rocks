#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 액션 값의 종류다(UE EInputActionValueType — Docs/Design/InputActions 2.1절). 이 순서가 승격
// 순서다(Boolean < Axis1D < Axis2D < Axis3D).
ROCKS_ENUM()
enum class InputValueType : uint8 {
    Boolean,
    Axis1D,
    Axis2D,
    Axis3D
};

inline constexpr uint32 InputValueTypeCount = 4;

// IsNonZero의 기본 허용 오차다(UE KINDA_SMALL_NUMBER).
inline constexpr double InputValueNonZeroTolerance = 1.0e-4;

// 액션 값이다(UE FInputActionValue — 2.1절). 값은 늘 Vec3d이고 타입이 쓰는 성분만 의미가 있다.
//
// 사용 예:
//   InputValue move(InputValueType::Axis2D, Vec3d(0.5, -1.0, 7.0));   // z는 0이 된다
//   if (move.IsNonZero()) { ... }
//
// 스레드: 값 타입이다.
struct ROCKS_INPUTACTIONS_API InputValue {
    Vec3d value;
    InputValueType type = InputValueType::Boolean;

    constexpr InputValue() noexcept = default;

    // 쓰지 않는 성분을 0으로 한다. x는 그대로다(Boolean이어도 0.3은 0.3 — UE
    // InputActionValue.h:49-65).
    InputValue(InputValueType valueType, const Vec3d& raw) noexcept;

    // Boolean 값이다(참이면 1).
    static InputValue FromBool(bool down) noexcept;

    // 같은 값을 다른 타입으로 다시 만든다(타입 지정 생성과 같다 — 2.1절 ConvertTo).
    InputValue ConvertTo(InputValueType valueType) const noexcept {
        return InputValue(valueType, value);
    }

    // SizeSquared ≥ tolerance²다(UE IsNonZero).
    bool IsNonZero(double tolerance = InputValueNonZeroTolerance) const noexcept;

    bool AsBool() const noexcept {
        return IsNonZero();
    }

    // Boolean·1D는 x(부호 유지), 2D는 |(x, y)|, 3D는 |v|다(UE GetMagnitude).
    double Magnitude() const noexcept;

    // Boolean·1D는 x², 2D는 x² + y², 3D는 |v|²다(UE GetMagnitudeSq).
    double MagnitudeSquared() const noexcept;

    // 성분 합이고 타입은 큰 쪽이다(UE operator+=).
    InputValue& operator+=(const InputValue& other) noexcept;

    friend bool operator==(const InputValue&, const InputValue&) noexcept = default;
};

} // namespace rocks::input
