#pragma once

#include <Core/Math/Color.h>
#include <CurveAssets/ColorCurveAdjustments.h>
#include <CurveAssets/ColorCurveEvaluate.h>
#include <CurveAssets/CurveAsset.h>

// 색 커브 에셋이다(Docs/Design/CurveAssets 2.4절, UE UCurveLinearColor 대응).
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
namespace rocks {

// R·G·B·A TangentCurve와 색 조정 일곱을 담는다. 색은 선형(sRGB 아님)이다. 새 에셋은 빈 커브다(UE
// 편집기의 검정→흰색 기본 키는 편집기 새로 만들기가 넣는다 — 설계 CAD4).
//
// 사용 예:
//   const LinearColor tint = curve->Evaluate(age);           // 조정 적용
//   const LinearColor raw = curve->EvaluateUnadjusted(age);  // 커브 값 그대로
ROCKS_CLASS()
class ROCKS_CURVEASSETS_API ColorCurveAsset final : public CurveAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit ColorCurveAsset(Allocator& allocator) noexcept
        : CurveAsset(allocator), r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    // 조정 적용 색이다(UE GetLinearColorValue — 원본 RGB가 LDR일 때만 명도를 [0, 1]로 자른다).
    LinearColor Evaluate(float time) const noexcept {
        return EvaluateAdjustedColorCurve(r, g, b, a, Adjustments(), time, false);
    }

    // 조정 적용 색이다(UE GetClampedLinearColorValue — 명도를 늘 [0, 1]로 자른다).
    LinearColor EvaluateClamped(float time) const noexcept {
        return EvaluateAdjustedColorCurve(r, g, b, a, Adjustments(), time, true);
    }

    // 조정 없는 색이다(UE GetUnadjustedLinearColorValue — 알파 키가 없으면 A = 1).
    LinearColor EvaluateUnadjusted(float time) const noexcept {
        return EvaluateUnadjustedColorCurve(r, g, b, a, time);
    }

    // 쿠킹용 결정적 판이다(설계 2.4.5절 — atlas 굽기·머티리얼 상수). Evaluate·EvaluateUnadjusted와
    // 같은 식이고 수학만 결정적이다(호스트와 무관한 비트, 런타임 판과 몇 ulp 다를 수 있다).
    LinearColor EvaluateDeterministic(float time) const noexcept {
        return EvaluateAdjustedColorCurveDeterministic(r, g, b, a, Adjustments(), time, false);
    }

    LinearColor EvaluateUnadjustedDeterministic(float time) const noexcept {
        return EvaluateUnadjustedColorCurveDeterministic(r, g, b, a, time);
    }

    // a에 키가 있는지다(UE HasAnyAlphaKeys).
    bool HasAlphaKeys() const noexcept {
        return a.KeyCount() > 0;
    }

    // 조정 필드 일곱을 한 값으로 모은다.
    ColorCurveAdjustments Adjustments() const noexcept;

    usize ChannelCount() const noexcept override;
    const TangentCurve& Channel(usize index) const noexcept override;
    TangentCurve& MutableChannel(usize index) noexcept override;
    StringView ChannelName(usize index) const noexcept override;

    ROCKS_PROPERTY() TangentCurve r;
    ROCKS_PROPERTY() TangentCurve g;
    ROCKS_PROPERTY() TangentCurve b;
    ROCKS_PROPERTY() TangentCurve a;
    // 색 조정(UE Category="Color" — 편집 범위는 UE ClampMin·ClampMax, 평가는 값을 그대로 쓴다).
    // 색상 이동(도, 편집 0–359).
    ROCKS_PROPERTY() float adjustHue = 0.0f;
    // 채도 배율(편집 0–1).
    ROCKS_PROPERTY() float adjustSaturation = 1.0f;
    // 명도 배율(편집 0–1).
    ROCKS_PROPERTY() float adjustBrightness = 1.0f;
    // 명도 지수(편집 0–1, 1이면 그대로).
    ROCKS_PROPERTY() float adjustBrightnessCurve = 1.0f;
    // 낮은 채도를 올리는 활기(편집 0–1).
    ROCKS_PROPERTY() float adjustVibrance = 0.0f;
    // 알파 재매핑 범위(편집 0–1 — A = lerp(min, max, 커브 A)).
    ROCKS_PROPERTY() float adjustMinAlpha = 0.0f;
    ROCKS_PROPERTY() float adjustMaxAlpha = 1.0f;
};

} // namespace rocks
