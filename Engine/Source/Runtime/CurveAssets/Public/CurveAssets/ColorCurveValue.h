#pragma once

#include <Core/Math/Color.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <CurveAssets/ColorCurveAdjustments.h>
#include <CurveAssets/ColorCurveEvaluate.h>
#include <Curves/TangentCurve.h>
#include <Meta/Annotations.h>

// 색 커브의 평탄 값이다(Docs/Design/CurveAssets 2.5절). 참조가 없어 복사·배열 원소·Map 값에 쓸 수
// 있다. ColorCurveAsset·ColorCurveSource의 사본(평탄화) 대상이다.
namespace rocks {

// R·G·B·A TangentCurve와 색 조정 일곱, 조정을 적용할지다. 에셋에서 평탄화하면 adjusted = 참(에셋의
// 조정 적용 평가), 인라인 원천에서 평탄화하면 거짓(원값 — UE FRuntimeCurveLinearColor 인라인)이다.
//
// 사용 예:
//   ColorCurveValue gradient(allocator);
//   const LinearColor color = gradient.Evaluate(0.5f);
ROCKS_STRUCT()
struct ROCKS_CURVEASSETS_API ColorCurveValue {
    ROCKS_GENERATED_BODY()

    explicit ColorCurveValue(Allocator& allocator) noexcept
        : r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    // adjusted면 조정 적용 색(UE GetLinearColorValue), 아니면 조정 없는 색이다.
    LinearColor Evaluate(float time) const noexcept;

    // adjusted면 명도를 늘 자르는 조정 적용 색(UE GetClampedLinearColorValue), 아니면 조정 없는
    // 색이다.
    LinearColor EvaluateClamped(float time) const noexcept;

    // 조정 필드 일곱을 한 값으로 모은다.
    ColorCurveAdjustments Adjustments() const noexcept;

    // 조정 필드 일곱과 adjusted를 한 번에 정한다(평탄화 — 에셋이면 그 조정과 참, 인라인이면 기본
    // 조정과 거짓).
    void SetAdjustments(const ColorCurveAdjustments& adjustments, bool apply) noexcept;

    // other를 깊이 복사한다(커브 채널마다 이 값의 그 채널 allocator로). 실패(할당)면 이 값은
    // 그대로다.
    Result<void> TryCopyFrom(const ColorCurveValue& other) noexcept;

    ROCKS_PROPERTY() TangentCurve r;
    ROCKS_PROPERTY() TangentCurve g;
    ROCKS_PROPERTY() TangentCurve b;
    ROCKS_PROPERTY() TangentCurve a;
    ROCKS_PROPERTY() float adjustHue = 0.0f;
    ROCKS_PROPERTY() float adjustSaturation = 1.0f;
    ROCKS_PROPERTY() float adjustBrightness = 1.0f;
    ROCKS_PROPERTY() float adjustBrightnessCurve = 1.0f;
    ROCKS_PROPERTY() float adjustVibrance = 0.0f;
    ROCKS_PROPERTY() float adjustMinAlpha = 0.0f;
    ROCKS_PROPERTY() float adjustMaxAlpha = 1.0f;
    ROCKS_PROPERTY() bool adjusted = false;
};

} // namespace rocks
