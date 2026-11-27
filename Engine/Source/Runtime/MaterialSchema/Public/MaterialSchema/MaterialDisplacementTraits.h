#pragma once

#include <Meta/Annotations.h>

// displacement·테셀레이션 기본 속성 묶음이다(Docs/Design/MaterialSystem 19.9절). 쓰는 pass가 아직
// 없다.
// TODO(MPQ-VirtualizedGeometry): 가상화 geometry·테셀레이션 pass가 생기면 그 pass가 읽는다. 그 전에
// 기본값이 아닌 값이 있으면 쿠킹이 PrerequisiteMissing을 낸다. 기본값은 UE FDisplacementScaling·
// FDisplacementFadeRange(RE/Classes/Engine/EngineTypes.h:3444–3510)와 같다.
namespace rocks::material {

ROCKS_STRUCT()
struct MaterialDisplacementTraits {
    ROCKS_GENERATED_BODY()

    // UE bEnableTessellation.
    ROCKS_PROPERTY() bool enableTessellation = false;
    // 크기(cm)와 중립 높이(UE DisplacementScaling).
    ROCKS_PROPERTY() float magnitude = 4.0f;
    ROCKS_PROPERTY() float center = 0.5f;
    // 화면 크기에 따른 감쇠(UE bEnableDisplacementFade·DisplacementFadeRange, 픽셀).
    ROCKS_PROPERTY() bool enableFade = false;
    ROCKS_PROPERTY() float fadeStartSizePixels = 4.0f;
    ROCKS_PROPERTY() float fadeEndSizePixels = 1.0f;
};

} // namespace rocks::material
