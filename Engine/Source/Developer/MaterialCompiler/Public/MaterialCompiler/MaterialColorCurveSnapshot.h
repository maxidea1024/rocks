#pragma once

#include <CurveAssets/ColorCurveAdjustments.h>
#include <Curves/TangentCurve.h>

// 번역 입력의 색 커브 snapshot이다(Docs/Design/MaterialSystem 19.4.2절, 6.13절 MS2f4 — ColorRamp).
// 에셋 객체(ColorCurveAsset) 대신 네 채널 커브와 색 조정만 본다. 쿠킹·에디터가 에셋에서 채운다.
//
// 사용 예:
//   MaterialColorCurveSnapshot snapshot{&curve->r, &curve->g, &curve->b, &curve->a,
//                                       curve->Adjustments()};
//
// 수명: 가리키는 커브는 번역이 끝날 때까지 살아 있고 바뀌지 않아야 한다.
namespace rocks::material {

struct MaterialColorCurveSnapshot {
    const TangentCurve* r = nullptr;
    const TangentCurve* g = nullptr;
    const TangentCurve* b = nullptr;
    const TangentCurve* a = nullptr;
    ColorCurveAdjustments adjustments;
};

} // namespace rocks::material
