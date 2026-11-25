#pragma once

#include <Core/Containers/Span.h>
#include <MaterialSchema/MaterialNodeDefinition.h>

// 엔진 노드 정의 묶음이다(Docs/Design/MaterialSystem 19.6절). 범주 파일마다 함수 하나가 정적 정의
// 배열을 돌려준다. BuiltinMaterialNodeGroups(MaterialSchemaModule.cpp)가 모은다.
namespace rocks::material::detail {

Span<const MaterialNodeDefinition> OutputNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> UtilityNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> MathNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> MaterialAttributesNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> ConstantsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> ColorNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> CoordinatesNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> VectorsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> FontNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> CustomNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> SwitchesNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> ParametersNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> TextureNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> DepthNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> WaterNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> DecalsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> ParticlesNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> LightingNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> SkyNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> PostProcessNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> DistanceFieldsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> NoiseNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> FunctionsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> SurfaceNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> OutputsNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> VirtualTextureNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> MeshPaintNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> PhysicsFieldNodeDefinitions() noexcept;
Span<const MaterialNodeDefinition> SpeedTreeNodeDefinitions() noexcept;

} // namespace rocks::material::detail
