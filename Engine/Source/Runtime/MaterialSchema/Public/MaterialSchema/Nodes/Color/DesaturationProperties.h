#pragma once

#include <Core/Math/Color.h>
#include <Meta/Annotations.h>

// Desaturation 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionDesaturation — RE/Public/Materials/MaterialExpressionDesaturation.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct DesaturationProperties {
    ROCKS_GENERATED_BODY()

    // UE 작업 색공간(기본 sRGB·Rec.709 선형)의 휘도 계수다(UE FColorSpace::GetLuminanceFactors).
    ROCKS_PROPERTY()
    LinearColor luminanceFactors = LinearColor(0.2126390f, 0.7151687f, 0.0721923f, 1.0f);
};

} // namespace rocks::material
