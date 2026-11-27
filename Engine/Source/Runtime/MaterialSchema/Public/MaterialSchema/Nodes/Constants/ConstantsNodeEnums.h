#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// Constants 범주 노드 속성의 열거다(Docs/Design/MaterialSystem 19.13.2절). 값 번호와 뜻은
// UE 5.8 열거와 같고 이름은 UE 접두를 뗀 나머지다. UE의 숨은 값과 _MAX 표지는 두지 않는다. 저장
// 형식이라 바꾸지 않는다.
namespace rocks::material {

// ExposedViewProperty 값이다(UE EMaterialExposedViewProperty —
// RE/Public/Materials/MaterialExpressionViewProperty.h).
ROCKS_ENUM()
enum class ExposedViewProperty : uint8 {
    BufferSize = 0,
    FieldOfView = 1,
    TanHalfFieldOfView = 2,
    ViewSize = 3,
    ViewPosition = 4,
    CameraPosition = 5,
    ViewportOffset = 6,
    TemporalSampleCount = 7,
    TemporalSampleIndex = 8,
    TemporalSampleOffset = 9,
    RuntimeVirtualTextureOutputLevel = 10,
    RuntimeVirtualTextureOutputDerivative = 11,
    PreExposure = 12,
    RuntimeVirtualTextureMaxLevel = 13,
    ResolutionFraction = 14,
    PostVolumeUserFlags = 15,
    FirstPersonFieldOfView = 16,
    FirstPersonTanHalfFieldOfView = 17,
    FirstPersonScale = 18,
    NearPlane = 19
};

} // namespace rocks::material
