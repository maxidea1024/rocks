#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialSchema/MaterialPinType.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <Meta/GeneratedType.h>
#include <Meta/TypeSchema.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// 엔진 노드의 동적 핀 함수다(Docs/Design/MaterialSystem 19.13.3절). UE가 속성에 따라 입력·출력을
// 바꾸는 노드(GetInput·GetInputName·GetOutputs 재정의)를 같은 조건으로 계산한다. 속성은
// NodePropertyReader로 Record를 직접 읽는다. 동적 핀은 입력 기본값을 갖지 않는다(값 편집 숨김):
// 연결이 없을 때의 값은 속성(UE Const* 칸)이 준다.
//
// 핀 키 규칙(19.13.3절, NodeGraph GD2):
// - 사용자가 이름을 붙이는 핀(Custom·Switch 입력, 레이어 입력, 속성 이름)은 그 이름이다. 이름이
// 비어
//   있으면 "Input<n>"·"Output<n>"이다(n은 0부터, 10진).
// - 함수 호출 입출력은 "P_<함수 입출력 노드 id 32자 hex>", 속성 Get/Set은 "A_<속성 id 32자 hex>"다.
// - 그 밖은 UE 입력 멤버 이름이다(MipValue·CoordinatesDX 등).
//
// 표시 이름은 정적 문자열이나 노드 속성 바이트를 가리킨다(NodeGraph 6.2절).
namespace rocks::material::detail {

// 텍스처 샘플 노드의 mip 입력이다(UE UMaterialExpressionTextureSample::GetInput — MipValueMode가
// MipLevel·MipBias면 MipValue, Derivative면 CoordinatesDX·CoordinatesDY).
Result<void> AddTextureSampleMipPins(const nodegraph::DynamicPinsInput& input,
                                     nodegraph::PinList& out, const TypeSchema& schema) noexcept;

// sparse volume texture 샘플의 mip 입력이다(UE UMaterialExpressionSparseVolumeTextureSample).
Result<void> AddSparseVolumeTextureMipPins(const nodegraph::DynamicPinsInput& input,
                                           nodegraph::PinList& out,
                                           const TypeSchema& schema) noexcept;

// runtime virtual texture 샘플의 mip 입력이다(UE UMaterialExpressionRuntimeVirtualTextureSample::
// GetInput — MipLevel·MipBias면 MipValue, DerivativeUV·DerivativeWorld면 DDX·DDY).
Result<void> AddRuntimeVirtualTextureMipPins(const nodegraph::DynamicPinsInput& input,
                                             nodegraph::PinList& out,
                                             const TypeSchema& schema) noexcept;

// 텍스처 객체 노드의 출력이다. 타입은 texture 참조의 기대 클래스로 정한다(UE
// UMaterialExpressionTextureObject::GetOutputValueType — 비어 있으면 Texture2D).
Result<void> AddTextureObjectOutputPin(const nodegraph::DynamicPinsInput& input,
                                       nodegraph::PinList& out, const TypeSchema& schema) noexcept;

template <class P>
Result<void> TextureSampleMipPins(const nodegraph::DynamicPinsInput& input,
                                  nodegraph::PinList& out) noexcept {
    return AddTextureSampleMipPins(input, out, GeneratedType<P>::Schema());
}

template <class P>
Result<void> SparseVolumeTextureSampleMipPins(const nodegraph::DynamicPinsInput& input,
                                              nodegraph::PinList& out) noexcept {
    return AddSparseVolumeTextureMipPins(input, out, GeneratedType<P>::Schema());
}

template <class P>
Result<void> RuntimeVirtualTextureMipPins(const nodegraph::DynamicPinsInput& input,
                                          nodegraph::PinList& out) noexcept {
    return AddRuntimeVirtualTextureMipPins(input, out, GeneratedType<P>::Schema());
}

template <class P>
Result<void> TextureObjectOutputPins(const nodegraph::DynamicPinsInput& input,
                                     nodegraph::PinList& out) noexcept {
    return AddTextureObjectOutputPin(input, out, GeneratedType<P>::Schema());
}

// Custom(임의 HLSL): 입력은 inputs, 출력은 Result(outputType)와 additionalOutputs다.
Result<void> CustomDynamicPins(const nodegraph::DynamicPinsInput& input,
                               nodegraph::PinList& out) noexcept;
// Switch: 정적 SwitchValue·Default 뒤에 inputs의 분기 입력이다.
Result<void> SwitchDynamicPins(const nodegraph::DynamicPinsInput& input,
                               nodegraph::PinList& out) noexcept;
// Convert: convertInputs·convertOutputs의 성분 폭대로 입출력이다.
Result<void> ConvertDynamicPins(const nodegraph::DynamicPinsInput& input,
                                nodegraph::PinList& out) noexcept;
// Operator: dynamicInputs 수만큼 입력 A, B, …다(UE GetInputName — 27번째부터 "Input <n>").
Result<void> OperatorDynamicPins(const nodegraph::DynamicPinsInput& input,
                                 nodegraph::PinList& out) noexcept;
// TransformPosition: 주기 world·1인칭 공간을 쓸 때만 그 보조 입력이다.
Result<void> TransformPositionDynamicPins(const nodegraph::DynamicPinsInput& input,
                                          nodegraph::PinList& out) noexcept;
// TextureObjectFromCollection: 출력 타입은 textureType이다.
Result<void> TextureObjectFromCollectionDynamicPins(const nodegraph::DynamicPinsInput& input,
                                                    nodegraph::PinList& out) noexcept;
// DynamicParameter: 출력 Param1–Param4(표시 이름은 paramNames)와 RGB·RGBA다.
Result<void> DynamicParameterOutputPins(const nodegraph::DynamicPinsInput& input,
                                        nodegraph::PinList& out) noexcept;
// 함수 호출: 함수 입출력 사본(inputs·outputs)이다.
Result<void> FunctionCallDynamicPins(const nodegraph::DynamicPinsInput& input,
                                     nodegraph::PinList& out) noexcept;
// 함수 입력: 미리보기 입력과 출력의 타입은 inputType이다.
Result<void> FunctionInputDynamicPins(const nodegraph::DynamicPinsInput& input,
                                      nodegraph::PinList& out) noexcept;
// Aggregate: attributeNames마다 입력·출력 한 쌍이다.
Result<void> AggregateDynamicPins(const nodegraph::DynamicPinsInput& input,
                                  nodegraph::PinList& out) noexcept;
// GetMaterialAttributes: attributeGetTypes마다 출력이다.
Result<void> GetAttributesDynamicPins(const nodegraph::DynamicPinsInput& input,
                                      nodegraph::PinList& out) noexcept;
// SetMaterialAttributes: attributeSetTypes마다 입력이다.
Result<void> SetAttributesDynamicPins(const nodegraph::DynamicPinsInput& input,
                                      nodegraph::PinList& out) noexcept;
// LayerStack: 레이어 함수 입력 사본(layerInputs)이다.
Result<void> LayerStackDynamicPins(const nodegraph::DynamicPinsInput& input,
                                   nodegraph::PinList& out) noexcept;
// MaterialCacheOutput: 표본이면 Primitive·UV 입력과 속성 출력, 아니면 속성 입력이다.
Result<void> MaterialCacheDynamicPins(const nodegraph::DynamicPinsInput& input,
                                      nodegraph::PinList& out) noexcept;

} // namespace rocks::material::detail
