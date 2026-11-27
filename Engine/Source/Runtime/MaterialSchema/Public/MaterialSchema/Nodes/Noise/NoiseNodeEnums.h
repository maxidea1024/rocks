#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Noise 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// NoiseFunction 값이다(UE ENoiseFunction — RE/Public/Materials/MaterialExpressionNoise.h).
ROCKS_ENUM()
enum class NoiseFunction : uint8 {
    SimplexTex = 0,
    GradientTex = 1,
    GradientTex3D = 2,
    GradientALU = 3,
    ValueALU = 4,
    VoronoiALU = 5
};

// VectorNoiseFunction 값이다(UE EVectorNoiseFunction —
// RE/Public/Materials/MaterialExpressionVectorNoise.h).
ROCKS_ENUM()
enum class VectorNoiseFunction : uint8 {
    CellnoiseALU = 0,
    VectorALU = 1,
    GradientALU = 2,
    CurlALU = 3,
    VoronoiALU = 4
};

} // namespace rocks::material
