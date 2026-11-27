#pragma once

// 색 커브의 조정 값이다(Docs/Design/CurveAssets 2.4절, UE UCurveLinearColor의 Adjust* 일곱).
namespace rocks {

// 색 조정 일곱이다(UE AdjustHue·AdjustSaturation·AdjustBrightness·AdjustBrightnessCurve·
// AdjustVibrance·AdjustMinAlpha·AdjustMaxAlpha — 기본값은 UE 생성자 값이고 모두 항등이다). 리플렉션
// struct가 아니다: ColorCurveAsset·ColorCurveValue가 같은 이름의 영속 필드를 갖고 Adjustments()로
// 이 값을 만들어 평가 함수(ColorCurveEvaluate.h)에 넘긴다.
//
// 사용 예:
//   ColorCurveAdjustments warm;
//   warm.hue = 20.0f;
//   const LinearColor color = EvaluateAdjustedColorCurve(r, g, b, a, warm, time, false);
struct ColorCurveAdjustments {
    // 색상 이동(도).
    float hue = 0.0f;
    // 채도 배율.
    float saturation = 1.0f;
    // 명도 배율.
    float brightness = 1.0f;
    // 명도 지수(1이면 그대로).
    float brightnessCurve = 1.0f;
    // 낮은 채도를 올리는 활기([0, 1]로 잘라 쓴다).
    float vibrance = 0.0f;
    // 알파 재매핑 범위(A = lerp(minAlpha, maxAlpha, 커브 A)).
    float minAlpha = 0.0f;
    float maxAlpha = 1.0f;
};

} // namespace rocks
