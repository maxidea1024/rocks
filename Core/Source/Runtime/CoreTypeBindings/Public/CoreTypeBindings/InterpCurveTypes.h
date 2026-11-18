#pragma once

#include <Core/Math/InterpCurve.h>
#include <Core/Result.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <Meta/Annotations.h>

// Core InterpCurve의 리플렉션 미러 선언이다(Docs/Design/CurveData.ko.md 12.1절, D19 — UE 5.8
// NoExportTypes.h의 FInterpCurve*·FInterpCurvePoint* USTRUCT 대응). 대상은 UE에 USTRUCT가 있는 다섯
// 형식과 같은 정밀도다(UE 5의 FVector2D·FVector·FQuat는 double — UE MathFwd.h 47–50):
//   InterpCurve<float>       ↔ FInterpCurveFloat
//   InterpCurve<Vec2d>       ↔ FInterpCurveVector2D
//   InterpCurve<Vec3d>       ↔ FInterpCurveVector
//   InterpCurve<Quatd>       ↔ FInterpCurveQuat
//   InterpCurve<LinearColor> ↔ FInterpCurveLinearColor
// 와 각 InterpCurvePoint<T>, enum InterpCurveMode(UE EInterpCurveMode)다. FInterpCurveTwoVectors는
// Rocks 값 형식이 없어 두지 않는다. 다른 형식(double·float 벡터 등)은 소비자가 생길 때 미러를
// 더한다.
//
// - 키 struct 미러는 공개 멤버(inValue·outValue·arriveTangent·leaveTangent·mode)를 같은 이름·형식·
//   순서로 나열하고 Atomic(키 하나가 통째 단위 — Curves 키 struct와 같다)이다.
// - 커브 미러는 private 멤버(points_·looped_·loopKeyOffset_)를 C++ 이름으로 나열한다. 생성 코드는
//   InterpCurve가 선언한 GeneratedType friend로 닿는다(접근자 미러가 아니다). 도구·JSON 이름은
//   Name= "points"·"looped"·"loopKeyOffset"이다.
// - 커브 미러의 Validate(검증 hook — Docs/Design/CurveData.ko.md 9.1절)가 InterpCurve
//   불변식을 지킨다: 키 입력값이 유한하고 비내림차순, 루프 offset이 유한, 루프가 켜져 있으면
//   offset이 양수다(키 없는 루프 커브도 받는다 — Reset이 남기는 값). graph decode·JSON import·대입
//   묶음·경로 쓰기가 어기는 값을 게시하지 않는다(InvalidArgument).
//
// 사용 예(이 모듈에 의존하는 모듈의 리플렉션 클래스):
//   ROCKS_CLASS()
//   class Spline : public rocks::Object {
//   public:
//       explicit Spline(rocks::Allocator& allocator) noexcept : path(allocator) {}
//       ROCKS_PROPERTY() rocks::InterpCurve<rocks::Vec3d> path;   // 미러 InterpCurveVectorBinding
//   };
// 미러 타입(…Binding)을 필드 형식으로 직접 쓰지 않는다 — 실제 타입을 쓴다.
namespace rocks::core_type_bindings {

// 키에서 다음 키까지 구간의 보간 방식이다(UE EInterpCurveMode와 같은 번호 —
// Core/Math/InterpCurvePoint.h).
ROCKS_ENUM(Binds = "rocks::InterpCurveMode")
enum class InterpCurveModeBinding : uint8 {
    Linear = 0,
    CurveAuto = 1,
    Constant = 2,
    CurveUser = 3,
    CurveBreak = 4,
    CurveAutoClamped = 5,
};

// 커브 미러의 검증 hook 본문이다(형식마다 같은 규칙). 할당하지 않고 어느 스레드에서나 부를 수 있다.
template <typename T>
Result<void> ValidateInterpCurve(const InterpCurve<T>& curve) noexcept {
    const auto points = curve.Points();
    for (usize index = 0; index < points.Size(); ++index) {
        const float input = points[index].inValue;
        if (!math::IsFinite(input) || (index > 0 && points[index - 1].inValue > input)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 루프면 offset이 유한한 양수다. 키를 모두 지운 루프 커브(Reset·TryRemovePoint는 루프를
    // 남긴다 — UE와 같다)도 받는다.
    const float offset = curve.LoopKeyOffset();
    if (!math::IsFinite(offset) || (curve.IsLooped() && !(offset > 0.0f))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// ---- InterpCurve<float> (UE FInterpCurvePointFloat·FInterpCurveFloat) ----

ROCKS_STRUCT(Binds = "rocks::InterpCurvePoint<float>", Atomic, Version = 1)
struct InterpCurvePointFloatBinding {
    ROCKS_PROPERTY() float inValue = 0;
    ROCKS_PROPERTY() float outValue = 0;
    ROCKS_PROPERTY() float arriveTangent = 0;
    ROCKS_PROPERTY() float leaveTangent = 0;
    ROCKS_PROPERTY() InterpCurveMode mode = InterpCurveMode::Linear;
};

ROCKS_STRUCT(Binds = "rocks::InterpCurve<float>", Version = 1)
struct InterpCurveFloatBinding {
    ROCKS_PROPERTY(Name = "points") Array<InterpCurvePoint<float>> points_;
    ROCKS_PROPERTY(Name = "looped") bool looped_ = false;
    ROCKS_PROPERTY(Name = "loopKeyOffset") float loopKeyOffset_ = 0;

    static Result<void> Validate(const InterpCurve<float>& curve) noexcept {
        return ValidateInterpCurve(curve);
    }
};

// ---- InterpCurve<Vec2d> (UE FInterpCurvePointVector2D·FInterpCurveVector2D) ----

ROCKS_STRUCT(Binds = "rocks::InterpCurvePoint<rocks::Vec2d>", Atomic, Version = 1)
struct InterpCurvePointVector2dBinding {
    ROCKS_PROPERTY() float inValue = 0;
    ROCKS_PROPERTY() Vec2d outValue;
    ROCKS_PROPERTY() Vec2d arriveTangent;
    ROCKS_PROPERTY() Vec2d leaveTangent;
    ROCKS_PROPERTY() InterpCurveMode mode = InterpCurveMode::Linear;
};

ROCKS_STRUCT(Binds = "rocks::InterpCurve<rocks::Vec2d>", Version = 1)
struct InterpCurveVector2dBinding {
    ROCKS_PROPERTY(Name = "points") Array<InterpCurvePoint<Vec2d>> points_;
    ROCKS_PROPERTY(Name = "looped") bool looped_ = false;
    ROCKS_PROPERTY(Name = "loopKeyOffset") float loopKeyOffset_ = 0;

    static Result<void> Validate(const InterpCurve<Vec2d>& curve) noexcept {
        return ValidateInterpCurve(curve);
    }
};

// ---- InterpCurve<Vec3d> (UE FInterpCurvePointVector·FInterpCurveVector — 위치면 cm) ----

ROCKS_STRUCT(Binds = "rocks::InterpCurvePoint<rocks::Vec3d>", Atomic, Version = 1)
struct InterpCurvePointVectorBinding {
    ROCKS_PROPERTY() float inValue = 0;
    ROCKS_PROPERTY() Vec3d outValue;
    ROCKS_PROPERTY() Vec3d arriveTangent;
    ROCKS_PROPERTY() Vec3d leaveTangent;
    ROCKS_PROPERTY() InterpCurveMode mode = InterpCurveMode::Linear;
};

ROCKS_STRUCT(Binds = "rocks::InterpCurve<rocks::Vec3d>", Version = 1)
struct InterpCurveVectorBinding {
    ROCKS_PROPERTY(Name = "points") Array<InterpCurvePoint<Vec3d>> points_;
    ROCKS_PROPERTY(Name = "looped") bool looped_ = false;
    ROCKS_PROPERTY(Name = "loopKeyOffset") float loopKeyOffset_ = 0;

    static Result<void> Validate(const InterpCurve<Vec3d>& curve) noexcept {
        return ValidateInterpCurve(curve);
    }
};

// ---- InterpCurve<Quatd> (UE FInterpCurvePointQuat·FInterpCurveQuat — 접선 칸은 SQUAD 제어점) ----

ROCKS_STRUCT(Binds = "rocks::InterpCurvePoint<rocks::Quatd>", Atomic, Version = 1)
struct InterpCurvePointQuatBinding {
    ROCKS_PROPERTY() float inValue = 0;
    ROCKS_PROPERTY() Quatd outValue;
    ROCKS_PROPERTY() Quatd arriveTangent;
    ROCKS_PROPERTY() Quatd leaveTangent;
    ROCKS_PROPERTY() InterpCurveMode mode = InterpCurveMode::Linear;
};

ROCKS_STRUCT(Binds = "rocks::InterpCurve<rocks::Quatd>", Version = 1)
struct InterpCurveQuatBinding {
    ROCKS_PROPERTY(Name = "points") Array<InterpCurvePoint<Quatd>> points_;
    ROCKS_PROPERTY(Name = "looped") bool looped_ = false;
    ROCKS_PROPERTY(Name = "loopKeyOffset") float loopKeyOffset_ = 0;

    static Result<void> Validate(const InterpCurve<Quatd>& curve) noexcept {
        return ValidateInterpCurve(curve);
    }
};

// ---- InterpCurve<LinearColor> (UE FInterpCurvePointLinearColor·FInterpCurveLinearColor) ----

ROCKS_STRUCT(Binds = "rocks::InterpCurvePoint<rocks::LinearColor>", Atomic, Version = 1)
struct InterpCurvePointLinearColorBinding {
    ROCKS_PROPERTY() float inValue = 0;
    ROCKS_PROPERTY() LinearColor outValue;
    ROCKS_PROPERTY() LinearColor arriveTangent;
    ROCKS_PROPERTY() LinearColor leaveTangent;
    ROCKS_PROPERTY() InterpCurveMode mode = InterpCurveMode::Linear;
};

ROCKS_STRUCT(Binds = "rocks::InterpCurve<rocks::LinearColor>", Version = 1)
struct InterpCurveLinearColorBinding {
    ROCKS_PROPERTY(Name = "points") Array<InterpCurvePoint<LinearColor>> points_;
    ROCKS_PROPERTY(Name = "looped") bool looped_ = false;
    ROCKS_PROPERTY(Name = "loopKeyOffset") float loopKeyOffset_ = 0;

    static Result<void> Validate(const InterpCurve<LinearColor>& curve) noexcept {
        return ValidateInterpCurve(curve);
    }
};

} // namespace rocks::core_type_bindings
