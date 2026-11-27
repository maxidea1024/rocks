#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Functions 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// BlendInputRelevance 값이다(UE EBlendInputRelevance —
// RE/Public/Materials/MaterialExpressionFunctionInput.h).
ROCKS_ENUM()
enum class BlendInputRelevance : uint8 {
    General = 0,
    Top = 1,
    Bottom = 2
};

// FunctionInputType 값이다(UE EFunctionInputType —
// RE/Public/Materials/MaterialExpressionFunctionInput.h).
ROCKS_ENUM()
enum class FunctionInputType : uint8 {
    Scalar = 0,
    Vector2 = 1,
    Vector3 = 2,
    Vector4 = 3,
    Texture2D = 4,
    TextureCube = 5,
    Texture2DArray = 6,
    Texture3D = 7,
    StaticBool = 8,
    MaterialAttributes = 9,
    TextureExternal = 10,
    Bool = 11,
    Surface = 12
};

} // namespace rocks::material
