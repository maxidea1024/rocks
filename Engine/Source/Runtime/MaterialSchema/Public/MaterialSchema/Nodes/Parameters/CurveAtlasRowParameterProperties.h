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
    // TODO(MPQ-CurveAsset): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 CurveLinearColor다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference curve;
    // TODO(MPQ-CurveAsset): 대상 에셋 클래스가 생기면 그 클래스로 검사한다(19.4.3절).
    // 기대 클래스는 CurveLinearColorAtlas다(19.4.3절).
    ROCKS_PROPERTY() MaterialAssetReference atlas;
};

} // namespace rocks::material
