#pragma once

#include <Base/Guid.h>
#include <Core/Text/Name.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/Nodes/Parameters/ParametersNodeEnums.h>
#include <Meta/Annotations.h>

// Curve Atlas Row Parameter 노드의 속성이다(Docs/Design/MaterialSystem 19.6·19.13절, UE
// UMaterialExpressionCurveAtlasRowParameter —
// RE/Public/Materials/MaterialExpressionCurveAtlasRowParameter.h). 이름·기본값은 UE와 같다(b 접두를
// 떼고 camelCase).
namespace rocks::material {

ROCKS_STRUCT()
struct CurveAtlasRowParameterProperties {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() Name parameterName;
    ROCKS_PROPERTY() Guid parameterId {};
    // 함수 안에서 호출 위치마다가 아니라 전역 scope 하나다(Docs/Design/MaterialSystem 9.2절 MD5
    // "공유" 표시 — 6.12절 MS2e2).
    ROCKS_PROPERTY() bool shared = false;
    ROCKS_PROPERTY() Name group;
    ROCKS_PROPERTY() int32 sortPriority = 32;
    ROCKS_PROPERTY() float defaultValue {};
    ROCKS_PROPERTY() ScalarParameterControlType controlType = ScalarParameterControlType::Numeric;
    ROCKS_PROPERTY() float sliderMin = 0.0f;
    ROCKS_PROPERTY() float sliderMax = 0.0f;
    // 기대 클래스는 Enum다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference enumeration;
    ROCKS_PROPERTY() uint8 enumerationIndex = 0;
    ROCKS_PROPERTY() bool useCustomPrimitiveData = false;
    ROCKS_PROPERTY() uint8 primitiveDataIndex = 0;
    // 기대 클래스는 ColorCurveAsset(UE CurveLinearColor)이다(19.4.3절 — 번역은 atlas snapshot의 행
    // 목록에서 이 id를 찾는다, 6.10절 MS2c5).
    ROCKS_PROPERTY() MaterialAssetReference curve;
    // 기대 클래스는 ColorCurveAtlas(UE CurveLinearColorAtlas)이다(19.4.3절 — 번역은 원본 공급
    // FindCurveAtlas로 찾는다, 6.10절 MS2c5).
    ROCKS_PROPERTY() MaterialAssetReference atlas;
};

} // namespace rocks::material
