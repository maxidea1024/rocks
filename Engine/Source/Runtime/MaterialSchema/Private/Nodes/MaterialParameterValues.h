#pragma once

#include <MaterialSchema/MaterialNodeDefinition.h>

// 파라미터 노드의 값 hook이다(Docs/Design/MaterialSystem 12.8절 MS3d1 — UE GetParameterValue).
// 모든 파라미터 노드는 parameterName·parameterId·group·sortPriority를 가지고, 기본값 필드만 모양이
// 다르다(노드 정의 파일이 이 hook을 ParameterValue로 단다).
namespace rocks::material::detail {

// Scalar(defaultValue float, custom primitive data).
Result<bool> ScalarParameterValue(NodePropertyReader& properties,
                                  MaterialParameterMetadata& out) noexcept;

// Vector(defaultValue LinearColor, custom primitive data).
Result<bool> VectorParameterValue(NodePropertyReader& properties,
                                  MaterialParameterMetadata& out) noexcept;

// ChannelMaskParameter(Vector, defaultValue LinearColor — UE처럼 custom primitive data를 보지
// 않는다).
Result<bool> ChannelMaskParameterValue(NodePropertyReader& properties,
                                       MaterialParameterMetadata& out) noexcept;

// DoubleVector(defaultValue Vec4d).
Result<bool> DoubleVectorParameterValue(NodePropertyReader& properties,
                                        MaterialParameterMetadata& out) noexcept;

// StaticBool·StaticSwitch(StaticSwitch, defaultValue bool → 0·1).
Result<bool> StaticSwitchParameterValue(NodePropertyReader& properties,
                                        MaterialParameterMetadata& out) noexcept;

// StaticComponentMask(defaultR·G·B·A → 0·1).
Result<bool> StaticComponentMaskParameterValue(NodePropertyReader& properties,
                                               MaterialParameterMetadata& out) noexcept;

// CurveAtlasRowParameter(Scalar, curve·atlas — UE bUsedAsAtlasPosition, custom primitive data).
Result<bool> CurveAtlasRowParameterValue(NodePropertyReader& properties,
                                         MaterialParameterMetadata& out) noexcept;

// 텍스처 파라미터(Texture, texture).
Result<bool> TextureParameterValue(NodePropertyReader& properties,
                                   MaterialParameterMetadata& out) noexcept;

// 텍스처 컬렉션 파라미터(TextureCollection, textureCollection).
Result<bool> TextureCollectionParameterValue(NodePropertyReader& properties,
                                             MaterialParameterMetadata& out) noexcept;

// sparse volume 텍스처 파라미터(SparseVolumeTexture, sparseVolumeTexture).
Result<bool> SparseVolumeTextureParameterValue(NodePropertyReader& properties,
                                               MaterialParameterMetadata& out) noexcept;

// runtime virtual 텍스처 파라미터(RuntimeVirtualTexture, virtualTexture).
Result<bool> RuntimeVirtualTextureParameterValue(NodePropertyReader& properties,
                                                 MaterialParameterMetadata& out) noexcept;

// font 파라미터(Font, font).
Result<bool> FontParameterValue(NodePropertyReader& properties,
                                MaterialParameterMetadata& out) noexcept;

} // namespace rocks::material::detail
