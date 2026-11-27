#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Coordinates 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// LocalPositionOrigin 값이다(UE ELocalPositionOrigin —
// RE/Public/Materials/MaterialExpressionLocalPosition.h).
ROCKS_ENUM()
enum class LocalPositionOrigin : uint8 {
    Instance = 0,
    InstancePreSkinning = 1,
    Primitive = 2
};

// PositionIncludedOffsets 값이다(UE EPositionIncludedOffsets —
// RE/Public/Materials/MaterialExpressionLocalPosition.h).
ROCKS_ENUM()
enum class PositionIncludedOffsets : uint8 {
    IncludeOffsets = 0,
    ExcludeOffsets = 1
};

// WorldPositionIncludedOffsets 값이다(UE EWorldPositionIncludedOffsets —
// RE/Public/Materials/MaterialExpressionWorldPosition.h).
ROCKS_ENUM()
enum class WorldPositionIncludedOffsets : uint8 {
    Default = 0,
    ExcludeAllShaderOffsets = 1,
    CameraRelative = 2,
    CameraRelativeNoOffsets = 3
};

} // namespace rocks::material
