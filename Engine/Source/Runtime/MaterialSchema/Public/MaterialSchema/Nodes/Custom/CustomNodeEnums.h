#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Custom 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// CustomClipInstruction 값이다(UE ECustomMaterialClipInstruction —
// RE/Public/Materials/MaterialExpressionCustom.h).
ROCKS_ENUM()
enum class CustomClipInstruction : uint8 {
    Search = 0,
    Yes = 1,
    No = 2
};

// CustomOutputType 값이다(UE ECustomMaterialOutputType —
// RE/Public/Materials/MaterialExpressionCustom.h).
ROCKS_ENUM()
enum class CustomOutputType : uint8 {
    Float1 = 0,
    Float2 = 1,
    Float3 = 2,
    Float4 = 3,
    MaterialAttributes = 4
};

} // namespace rocks::material
