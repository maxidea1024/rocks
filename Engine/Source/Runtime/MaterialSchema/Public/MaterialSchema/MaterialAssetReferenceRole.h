#pragma once

#include <Base/Types.h>

// 노드 속성 에셋 참조의 역할이다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언" — 12.8절 MS3c).
// Build 의존 hook과 쿠킹 입력 모으기가 이 역할로 대상을 다룬다.
namespace rocks::material {

enum class MaterialAssetReferenceRole : uint8 {
    // 텍스처 에셋(Texture 파생 — referencedTextures, 텍스처 사실).
    Texture = 0,
    // 머티리얼 함수(MaterialFunctionBase — Build 의존, 그래프 펼침).
    Function = 1,
    // 다른 머티리얼(MaterialSample — Build 의존, 그래프 펼침).
    Material = 2,
    // 색 커브(ColorCurveAsset — Build 의존, 번역이 평가).
    ColorCurve = 3,
    // 색 커브 atlas(ColorCurveAtlas — Build 의존 행 목록이자 텍스처).
    CurveAtlas = 4,
    // 파라미터 컬렉션(ParameterCollection — Hard, layout).
    Collection = 5
};

} // namespace rocks::material
