#include <Core/Math/Easing.h>
#include <Core/Math/InterpTo.h>
#include <Core/Math/MathUtility.h>
#include <InputActions/InputModifier.h>
#include <Platform/PlatformMath.h>

// 모디파이어다(Docs/Design/InputActions 3.1절 — UE InputModifiers.cpp).
namespace rocks::input {

namespace {

// UE FMath::Sign과 같다(NaN이면 0 — math::Sign은 NaN을 낸다). 값이 NaN이면 dead zone 결과를 0으로
// 두어 상태(SmoothDelta)에 NaN이 들어가지 않게 한다.
float ModifierSign(float value) noexcept {
    return value > 0.0f ? 1.0f : (value < 0.0f ? -1.0f : 0.0f);
}

double ModifierSign(double value) noexcept {
    return value > 0.0 ? 1.0 : (value < 0.0 ? -1.0 : 0.0);
}

// UE DeadZoneLambda: min(1, max(0, |a| − L) / (U − L)) · sign(a) (cpp:188-192). float 계산이다.
double ModifierScaledDeadZone(double axis, float lower, float upper) noexcept {
    const float value = static_cast<float>(axis);
    return math::Min(1.0f, math::Max(0.0f, math::Abs(value) - lower) / (upper - lower)) *
           ModifierSign(value);
}

// UE UnscaledDeadZoneLambda: |a| < L이면 0, 아니면 min(|a|, U) · sign(a) (cpp:194-201).
double ModifierUnscaledDeadZone(double axis, float lower, float upper) noexcept {
    const float value = static_cast<float>(axis);
    const float absolute = math::Abs(value);
    return absolute < lower ? 0.0f : math::Min(absolute, upper) * ModifierSign(value);
}

// 안전 정규화(길이² < SMALL_NUMBER(1e-8)면 0 — UE GetSafeNormal).
constexpr double ModifierSafeNormalTolerance = 1.0e-8;

Vec3d ModifierSafeNormal(const Vec3d& value) noexcept {
    const double squared = value.LengthSquared();
    if (squared < ModifierSafeNormalTolerance) {
        return Vec3d();
    }

    return value * (1.0 / math::Sqrt(squared));
}

Vec3d ModifierSafeNormal2D(const Vec3d& value) noexcept {
    const double squared = value.x * value.x + value.y * value.y;
    if (squared < ModifierSafeNormalTolerance) {
        return Vec3d();
    }

    const double scale = 1.0 / math::Sqrt(squared);
    return Vec3d(value.x * scale, value.y * scale, 0.0);
}

Vec3d ApplyDeadZone(const InputModifier& modifier, const InputValue& input) noexcept {
    Vec3d value = input.value;
    const float lower = modifier.lowerThreshold;
    const float upper = modifier.upperThreshold;
    const bool unscaled = modifier.deadZoneType == DeadZoneType::UnscaledRadial;
    auto shape = [&](double axis) noexcept {
        return unscaled ? ModifierUnscaledDeadZone(axis, lower, upper)
                        : ModifierScaledDeadZone(axis, lower, upper);
    };

    if (modifier.deadZoneType == DeadZoneType::Axial) {
        return Vec3d(ModifierScaledDeadZone(value.x, lower, upper),
                     ModifierScaledDeadZone(value.y, lower, upper),
                     ModifierScaledDeadZone(value.z, lower, upper));
    }

    switch (input.type) {
    case InputValueType::Axis3D:
        return ModifierSafeNormal(value) * shape(value.Length());
    case InputValueType::Axis2D:
        return ModifierSafeNormal2D(value) *
               shape(math::Sqrt(value.x * value.x + value.y * value.y));
    default:
        value.x = shape(value.x);
        return value;
    }
}

// sign(a) · |a|^e, e가 1이면 그대로다(UE cpp:350-366).
double ApplyExponentialCurve(double axis, double exponent) noexcept {
    if (exponent == 1.0) {
        return axis;
    }

    return ModifierSign(axis) * platform::PlatformMath::Pow(math::Abs(axis), exponent);
}

// UE UInputModifierSmooth::ModifyRaw(cpp:270-333) — 표본 수는 늘 1이다(UE TODO와 같다).
InputValue ApplySmooth(InputModifierState& state, InputValue current,
                       double deltaSeconds) noexcept {
    constexpr uint32 sampleCount = 1;
    auto clear = [&]() noexcept {
        state.zeroTime = 0.0;
        state.averageValue = InputValue();
        state.samples = 0;
        state.totalSampleTime = SmoothInitialSampleTime;
    };

    if (state.averageValue.MagnitudeSquared() != 0.0) {
        state.totalSampleTime += deltaSeconds;
        state.samples += sampleCount;
    }

    if (deltaSeconds >= SmoothResetDeltaSeconds) {
        // 아주 긴 프레임은 결과를 흐리지 않게 모두 지운다.
        clear();
        return current;
    }

    if (state.samples == 0 || !(state.totalSampleTime > 0.0)) {
        return current;
    }

    const double samplingTime = state.totalSampleTime / static_cast<double>(state.samples);
    if (current.MagnitudeSquared() != 0.0) {
        state.zeroTime = 0.0;
        if (state.averageValue.MagnitudeSquared() != 0.0 &&
            deltaSeconds < samplingTime * static_cast<double>(sampleCount + 1)) {
            // 프레임당 표본 수가 일정하도록 고른다.
            current.value = current.value * (deltaSeconds / (samplingTime * sampleCount));
        }

        state.averageValue = InputValue(current.type, current.value * (1.0 / sampleCount));
        return current;
    }

    // 움직임이 없다: 표본 간격보다 짧으면 평균으로 채운다.
    if (state.zeroTime < samplingTime) {
        current = InputValue(current.type, state.averageValue.ConvertTo(current.type).value *
                                               (deltaSeconds / samplingTime));
    } else {
        clear();
    }

    state.zeroTime += deltaSeconds;
    return current;
}

Vec3d ModifierLerp(const Vec3d& from, const Vec3d& to, double alpha) noexcept {
    // UE LerpStable: a·(1 − t) + b·t.
    return from * (1.0 - alpha) + to * alpha;
}

Vec3d ApplySmoothDelta(const InputModifier& modifier, InputModifierState& state,
                       const Vec3d& current, double deltaSeconds) noexcept {
    const Vec3d target = ModifierSafeNormal(current - state.oldValue);
    state.oldValue = current;
    const float speed = modifier.smoothSpeed;
    const float exponent = modifier.easingExponent;
    switch (modifier.smoothMethod) {
    case SmoothDeltaMethod::Lerp:
        state.delta = ModifierLerp(state.delta, target, speed);
        break;
    case SmoothDeltaMethod::InterpTo:
        state.delta =
            math::VInterpTo(state.delta, target, deltaSeconds, static_cast<double>(speed));
        break;
    case SmoothDeltaMethod::InterpConstantTo:
        state.delta =
            math::VInterpConstantTo(state.delta, target, deltaSeconds, static_cast<double>(speed));
        break;
    case SmoothDeltaMethod::CircularIn:
        state.delta = ModifierLerp(state.delta, target, math::CircularInAlpha(speed));
        break;
    case SmoothDeltaMethod::CircularOut:
        state.delta = ModifierLerp(state.delta, target, math::CircularOutAlpha(speed));
        break;
    case SmoothDeltaMethod::CircularInOut:
        state.delta = ModifierLerp(state.delta, target, math::CircularInOutAlpha(speed));
        break;
    case SmoothDeltaMethod::EaseIn:
        state.delta = ModifierLerp(state.delta, target, math::EaseInAlpha(speed, exponent));
        break;
    case SmoothDeltaMethod::EaseOut:
        state.delta = ModifierLerp(state.delta, target, math::EaseOutAlpha(speed, exponent));
        break;
    case SmoothDeltaMethod::EaseInOut:
        state.delta = ModifierLerp(state.delta, target, math::EaseInOutAlpha(speed, exponent));
        break;
    case SmoothDeltaMethod::ExpoIn:
        state.delta = ModifierLerp(state.delta, target, math::ExpoInAlpha(speed));
        break;
    case SmoothDeltaMethod::ExpoOut:
        state.delta = ModifierLerp(state.delta, target, math::ExpoOutAlpha(speed));
        break;
    case SmoothDeltaMethod::ExpoInOut:
        state.delta = ModifierLerp(state.delta, target, math::ExpoInOutAlpha(speed));
        break;
    case SmoothDeltaMethod::SinIn:
        state.delta = ModifierLerp(state.delta, target, math::SinInAlpha(speed));
        break;
    case SmoothDeltaMethod::SinOut:
        state.delta = ModifierLerp(state.delta, target, math::SinOutAlpha(speed));
        break;
    case SmoothDeltaMethod::SinInOut:
        state.delta = ModifierLerp(state.delta, target, math::SinInOutAlpha(speed));
        break;
    default:
        state.delta = target;
        break;
    }

    return state.delta;
}

} // namespace

Result<void> InputModifier::TryCopyFrom(const InputModifier& other) noexcept {
    // 커브 셋을 임시본에 먼저 복사하고 모두 성공하면 옮긴다(실패면 이것은 그대로다).
    TangentCurve x(responseX.GetAllocator());
    TangentCurve y(responseY.GetAllocator());
    TangentCurve z(responseZ.GetAllocator());
    if (auto copied = x.TryCopyFrom(other.responseX); !copied) {
        return copied;
    }

    if (auto copied = y.TryCopyFrom(other.responseY); !copied) {
        return copied;
    }

    if (auto copied = z.TryCopyFrom(other.responseZ); !copied) {
        return copied;
    }

    responseX = static_cast<TangentCurve&&>(x);
    responseY = static_cast<TangentCurve&&>(y);
    responseZ = static_cast<TangentCurve&&>(z);

    kind = other.kind;
    lowerThreshold = other.lowerThreshold;
    upperThreshold = other.upperThreshold;
    deadZoneType = other.deadZoneType;
    scalar = other.scalar;
    negateX = other.negateX;
    negateY = other.negateY;
    negateZ = other.negateZ;
    swizzle = other.swizzle;
    curveExponent = other.curveExponent;
    fovScale = other.fovScale;
    smoothMethod = other.smoothMethod;
    smoothSpeed = other.smoothSpeed;
    easingExponent = other.easingExponent;
    return Result<void>::Success();
}

InputValue ApplyInputModifier(const InputModifier& modifier, InputModifierState& state,
                              const InputValue& value, const ModifierContext& context) noexcept {
    const bool boolean = value.type == InputValueType::Boolean;
    Vec3d result = value.value;
    switch (modifier.kind) {
    case InputModifierKind::DeadZone:
        // lower ≥ upper는 검증기가 거부한다(2.6절). 여기 오면 값을 그대로 둔다(0/0 NaN 방지).
        if (!boolean && modifier.upperThreshold > modifier.lowerThreshold) {
            result = ApplyDeadZone(modifier, value);
        }

        break;
    case InputModifierKind::Scalar:
        if (!boolean) {
            result = Vec3d(result.x * modifier.scalar.x, result.y * modifier.scalar.y,
                           result.z * modifier.scalar.z);
        }

        break;
    case InputModifierKind::ScaleByDeltaTime:
        if (!boolean) {
            result = result * context.deltaSeconds;
        }

        break;
    case InputModifierKind::Negate:
        result =
            Vec3d(modifier.negateX ? -result.x : result.x, modifier.negateY ? -result.y : result.y,
                  modifier.negateZ ? -result.z : result.z);
        break;
    case InputModifierKind::Swizzle:
        switch (modifier.swizzle) {
        case SwizzleOrder::YXZ:
            result = Vec3d(result.y, result.x, result.z);
            break;
        case SwizzleOrder::ZYX:
            result = Vec3d(result.z, result.y, result.x);
            break;
        case SwizzleOrder::XZY:
            result = Vec3d(result.x, result.z, result.y);
            break;
        case SwizzleOrder::YZX:
            result = Vec3d(result.y, result.z, result.x);
            break;
        case SwizzleOrder::ZXY:
            result = Vec3d(result.z, result.x, result.y);
            break;
        }

        break;
    case InputModifierKind::ResponseCurveExponential:
        // 타입이 쓰는 축만(UE switch fallthrough — Boolean은 바꾸지 않는다).
        if (value.type == InputValueType::Axis3D) {
            result.z = ApplyExponentialCurve(result.z, modifier.curveExponent.z);
        }

        if (value.type == InputValueType::Axis3D || value.type == InputValueType::Axis2D) {
            result.y = ApplyExponentialCurve(result.y, modifier.curveExponent.y);
        }

        if (!boolean) {
            result.x = ApplyExponentialCurve(result.x, modifier.curveExponent.x);
        }

        break;
    case InputModifierKind::ResponseCurveUser:
        // Boolean·1D는 x만, 2D는 y까지, 3D는 z까지(UE cpp:368-385).
        if (value.type == InputValueType::Axis3D) {
            result.z = modifier.responseZ.Evaluate(static_cast<float>(result.z), 0.0f);
        }

        if (value.type == InputValueType::Axis3D || value.type == InputValueType::Axis2D) {
            result.y = modifier.responseY.Evaluate(static_cast<float>(result.y), 0.0f);
        }

        result.x = modifier.responseX.Evaluate(static_cast<float>(result.x), 0.0f);
        break;
    case InputModifierKind::FovScaling:
        // UE cpp:391-419: 플레이어가 없으면 그대로, 카메라가 없으면 fovScale만, 있으면 FOV 비율도.
        if (context.hasPlayer) {
            float scale = modifier.fovScale;
            if (context.fov > 0.0f) {
                const float baseTan = math::Tan(math::DegreesToRadians(FovScalingBaseFov * 0.5f));
                const float tan = math::Tan(math::DegreesToRadians(context.fov * 0.5f));
                scale *= tan / baseTan;
            }

            result = result * static_cast<double>(scale);
        }

        break;
    case InputModifierKind::ToWorldSpace:
        if (value.type == InputValueType::Axis3D) {
            result = Vec3d(result.z, result.x, result.y);
        } else if (value.type == InputValueType::Axis2D) {
            result = Vec3d(result.y, result.x, result.z);
        }

        break;
    case InputModifierKind::Smooth:
        return ApplySmooth(state, value, context.deltaSeconds);
    case InputModifierKind::SmoothDelta:
        if (!boolean) {
            result = ApplySmoothDelta(modifier, state, value.value, context.deltaSeconds);
        }

        break;
    }

    return InputValue(value.type, result);
}

} // namespace rocks::input
