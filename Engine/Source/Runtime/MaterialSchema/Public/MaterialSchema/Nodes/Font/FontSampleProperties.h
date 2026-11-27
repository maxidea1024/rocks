#pragma once

#include <MaterialSchema/MaterialAssetReference.h>
#include <Meta/Annotations.h>

// Font Sample 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionFontSample — RE/Public/Materials/MaterialExpressionFontSample.h).
// 이름·기본값은 UE와 같다(b 접두를 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct FontSampleProperties {
    ROCKS_GENERATED_BODY()

    // TODO(MPQ-Font): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 Font다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference font;
    ROCKS_PROPERTY() int32 fontTexturePage {};
};

} // namespace rocks::material
