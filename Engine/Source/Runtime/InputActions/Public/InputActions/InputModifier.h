#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <CurveAssets/CurveAssetConstants.h>
#include <Curves/TangentCurve.h>
#include <InputActions/InputModifierState.h>
#include <InputActions/InputValue.h>
#include <InputActions/ModifierContext.h>
#include <Meta/Annotations.h>

namespace rocks::input {

// 모디파이어 종류다(UE UInputModifier 파생 — Docs/Design/InputActions 3.1절).
ROCKS_ENUM()
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
ROCKS_ENUM()
enum class DeadZoneType : uint8 {
    Axial,
    Radial,
    UnscaledRadial
};

inline constexpr uint32 DeadZoneTypeCount = 3;

// Swizzle 순서다(UE EInputAxisSwizzle).
ROCKS_ENUM()
enum class SwizzleOrder : uint8 {
    YXZ,
    ZYX,
    XZY,
    YZX,
    ZXY
};

inline constexpr uint32 SwizzleOrderCount = 5;

// SmoothDelta 보간식이다(UE ENormalizeInputSmoothingType — 15종).
ROCKS_ENUM()
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
// 키 배열을 가져서 이동만 된다(복사는 TryCopyFrom — 생성 깊은 복사). 리플렉션 struct다(바인딩·액션
// 데이터 — 2.4절).
//
// 사용 예:
//   InputModifier deadZone(allocator);
//   deadZone.kind = InputModifierKind::DeadZone;
//   deadZone.lowerThreshold = 0.25f;
ROCKS_STRUCT()
struct ROCKS_INPUTACTIONS_API InputModifier {
    ROCKS_GENERATED_BODY()

    explicit InputModifier(Allocator& allocator) noexcept
        : responseX(allocator), responseY(allocator), responseZ(allocator) {
    }

    // other를 복사한다. 실패(OutOfMemory)면 이것은 그대로다.
    Result<void> TryCopyFrom(const InputModifier& other) noexcept;

    ROCKS_PROPERTY() InputModifierKind kind = InputModifierKind::DeadZone;
    // DeadZone.
    ROCKS_PROPERTY() float lowerThreshold = DefaultDeadZoneLower;
    ROCKS_PROPERTY() float upperThreshold = DefaultDeadZoneUpper;
    ROCKS_PROPERTY() DeadZoneType deadZoneType = DeadZoneType::Radial;
    // Scalar.
    ROCKS_PROPERTY() Vec3d scalar = Vec3d(1.0, 1.0, 1.0);
    // Negate.
    ROCKS_PROPERTY() bool negateX = true;
    ROCKS_PROPERTY() bool negateY = true;
    ROCKS_PROPERTY() bool negateZ = true;
    // Swizzle.
    ROCKS_PROPERTY() SwizzleOrder swizzle = SwizzleOrder::YXZ;
    // ResponseCurveExponential.
    ROCKS_PROPERTY() Vec3d curveExponent = Vec3d(1.0, 1.0, 1.0);
    // ResponseCurveUser(키가 없고 기본값도 없으면 그 축은 0 — UE null 커브).
    ROCKS_PROPERTY() TangentCurve responseX;
    ROCKS_PROPERTY() TangentCurve responseY;
    ROCKS_PROPERTY() TangentCurve responseZ;
    // 축마다 커브 에셋 slot이다(소유 InputAction·InputContext의 curveAssets 번호, 없으면
    // NoCurveAssetSlot — 2.9절 IA9). 모으기가 에셋 커브를 response*에 복사하고 slot을
    // 비운다(평가기는 비운 값만 받는다).
    ROCKS_PROPERTY() uint32 responseXAsset = NoCurveAssetSlot;
    ROCKS_PROPERTY() uint32 responseYAsset = NoCurveAssetSlot;
    ROCKS_PROPERTY() uint32 responseZAsset = NoCurveAssetSlot;
    // FovScaling.
    ROCKS_PROPERTY() float fovScale = 1.0f;
    // SmoothDelta.
    ROCKS_PROPERTY() SmoothDeltaMethod smoothMethod = SmoothDeltaMethod::Lerp;
    ROCKS_PROPERTY() float smoothSpeed = DefaultSmoothDeltaSpeed;
    ROCKS_PROPERTY() float easingExponent = DefaultSmoothDeltaExponent;
};

// value에 modifier를 적용한다. 결과 타입은 value 타입이다(쓰지 않는 성분은 0 — UE
// EnhancedPlayerInput.cpp:1149-1163). state는 그 모디파이어의 상태다.
ROCKS_INPUTACTIONS_API InputValue ApplyInputModifier(const InputModifier& modifier,
                                                     InputModifierState& state,
                                                     const InputValue& value,
                                                     const ModifierContext& context) noexcept;

} // namespace rocks::input
