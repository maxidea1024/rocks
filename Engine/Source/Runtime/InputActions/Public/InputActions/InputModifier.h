#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Curves/TangentCurve.h>
#include <InputActions/InputModifierState.h>
#include <InputActions/InputValue.h>
#include <InputActions/ModifierContext.h>

namespace rocks::input {

// 모디파이어 종류다(UE UInputModifier 파생 — Docs/Design/InputActions 3.1절).
enum class InputModifierKind : uint8 {
    DeadZone,
    Scalar,
    ScaleByDeltaTime,
    Negate,
    Swizzle,
    ResponseCurveExponential,
    ResponseCurveUser,
    FovScaling,
    ToWorldSpace,
    Smooth,
    SmoothDelta
};

inline constexpr uint32 InputModifierKindCount = 11;

// DeadZone 방식이다(UE EDeadZoneType).
enum class DeadZoneType : uint8 {
    Axial,
    Radial,
    UnscaledRadial
};

// Swizzle 순서다(UE EInputAxisSwizzle).
enum class SwizzleOrder : uint8 {
    YXZ,
    ZYX,
    XZY,
    YZX,
    ZXY
};

// SmoothDelta 보간식이다(UE ENormalizeInputSmoothingType — 15종).
enum class SmoothDeltaMethod : uint8 {
    Lerp,
    InterpTo,
    InterpConstantTo,
    CircularIn,
    CircularOut,
    CircularInOut,
    EaseIn,
    EaseOut,
    EaseInOut,
    ExpoIn,
    ExpoOut,
    ExpoInOut,
    SinIn,
    SinOut,
    SinInOut
};

inline constexpr uint32 SmoothDeltaMethodCount = 15;

// UE 기본값이다(InputModifiers.h).
inline constexpr float DefaultDeadZoneLower = 0.2f;
inline constexpr float DefaultDeadZoneUpper = 1.0f;
inline constexpr float DefaultSmoothDeltaSpeed = 0.5f;
inline constexpr float DefaultSmoothDeltaExponent = 2.0f;
// FovScaling의 기준 FOV(도 — UE kPlayerInput_BaseFOV)다.
inline constexpr float FovScalingBaseFov = 80.0f;

// 모디파이어 하나다(종류 + 매개변수 — 3.1절). 매개변수는 그 종류에만 쓰인다. 사용자 응답 커브는
// 키 배열을 가져서 이동만 된다(복사는 TryCopyFrom).
//
// 사용 예:
//   InputModifier deadZone(allocator);
//   deadZone.kind = InputModifierKind::DeadZone;
//   deadZone.lowerThreshold = 0.25f;
struct ROCKS_INPUTACTIONS_API InputModifier {
    explicit InputModifier(Allocator& allocator) noexcept
        : responseX(allocator), responseY(allocator), responseZ(allocator) {
    }

    InputModifier(InputModifier&&) noexcept = default;
    InputModifier& operator=(InputModifier&&) noexcept = default;
    InputModifier(const InputModifier&) = delete;
    InputModifier& operator=(const InputModifier&) = delete;

    // other를 복사한다. 실패(OutOfMemory)면 이것은 그대로다.
    Result<void> TryCopyFrom(const InputModifier& other) noexcept;

    InputModifierKind kind = InputModifierKind::DeadZone;
    // DeadZone.
    float lowerThreshold = DefaultDeadZoneLower;
    float upperThreshold = DefaultDeadZoneUpper;
    DeadZoneType deadZoneType = DeadZoneType::Radial;
    // Scalar.
    Vec3d scalar = Vec3d(1.0, 1.0, 1.0);
    // Negate.
    bool negateX = true;
    bool negateY = true;
    bool negateZ = true;
    // Swizzle.
    SwizzleOrder swizzle = SwizzleOrder::YXZ;
    // ResponseCurveExponential.
    Vec3d curveExponent = Vec3d(1.0, 1.0, 1.0);
    // ResponseCurveUser(키가 없고 기본값도 없으면 그 축은 0 — UE null 커브).
    TangentCurve responseX;
    TangentCurve responseY;
    TangentCurve responseZ;
    // FovScaling.
    float fovScale = 1.0f;
    // SmoothDelta.
    SmoothDeltaMethod smoothMethod = SmoothDeltaMethod::Lerp;
    float smoothSpeed = DefaultSmoothDeltaSpeed;
    float easingExponent = DefaultSmoothDeltaExponent;
};

// value에 modifier를 적용한다. 결과 타입은 value 타입이다(쓰지 않는 성분은 0 — UE
// EnhancedPlayerInput.cpp:1149-1163). state는 그 모디파이어의 상태다.
ROCKS_INPUTACTIONS_API InputValue ApplyInputModifier(const InputModifier& modifier,
                                                     InputModifierState& state,
                                                     const InputValue& value,
                                                     const ModifierContext& context) noexcept;

} // namespace rocks::input
