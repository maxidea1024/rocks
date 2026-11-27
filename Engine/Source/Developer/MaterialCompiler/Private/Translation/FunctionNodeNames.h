#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 함수·레이어 펼치기가 쓰는 노드 종류 이름·핀 키·용도 규칙이다(Docs/Design/MaterialSystem 6.12절
// MS2e2·MS2e3). TranslateFunctions·TranslateLayers가 함께 쓴다.
namespace rocks::material::detail {

inline constexpr StringView FunctionCallTypeName = "Material.FunctionCall";
inline constexpr StringView FunctionInputTypeName = "Material.FunctionInput";
inline constexpr StringView FunctionOutputTypeName = "Material.FunctionOutput";
inline constexpr StringView MaterialSampleTypeName = "Material.MaterialSample";
inline constexpr StringView LayerOutputTypeName = "Material.LayerOutput";
inline constexpr StringView AttributeLayersTypeName = "Material.AttributeLayers";
inline constexpr StringView LayerStackTypeName = "Material.LayerStack";
inline constexpr StringView NamedRerouteUsageTypeName = "Material.NamedRerouteUsage";
inline constexpr StringView NamedRerouteDeclarationTypeName = "Material.NamedRerouteDeclaration";
// 이름 있는 재경유 선언의 입력 핀 키다.
inline constexpr StringView NamedRerouteDeclarationInputPin = "Input";

// UE IsStaticExpression이 참인 노드 종류다(6.12절 MS2e3 LayerStack 검사).
inline constexpr StringView StaticNodeTypeNames[] = {
    "Material.StaticBool", "Material.StaticSwitch", "Material.StaticSwitchParameter",
    "Material.StaticComponentMaskParameter", "Material.StaticBoolParameter"};

// 레이어·혼합 함수의 MaterialAttributes 입출력 수 규칙이다(UE AcceptableNum* —
// MaterialExpressionMaterialAttributeLayers.h:21–23).
inline constexpr usize AcceptableLayerAttributeInputs = 1;
inline constexpr usize AcceptableBlendAttributeInputs = 2;
inline constexpr usize AcceptableLayerBlendAttributeOutputs = 1;

// 검사 문구 접두다(UE MaterialAttributeLayers::LogError — 띄어 쓰지 않는다).
inline constexpr StringView LayerValidationPrefix = "LayerValidation:";

} // namespace rocks::material::detail
