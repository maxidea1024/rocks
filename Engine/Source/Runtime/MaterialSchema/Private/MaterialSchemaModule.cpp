#include "Nodes/MaterialBuiltinNodes.h"

#include <Core/Module/ModuleEntry.h>
#include <MaterialSchema/MaterialSchema.module.generated.h>
#include <MaterialSchema/MaterialSchemaModule.h>

// MaterialSchema 모듈의 진입점이다(Docs/Design/ModuleLoader 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, MaterialSchema)

namespace rocks::material {

Result<void> RegisterMaterialSchemaTypes(ModuleRegistration& registration) noexcept {
    return generated::MaterialSchema::RegisterModuleTypes(registration);
}

Span<const Span<const MaterialNodeDefinition>> BuiltinMaterialNodeGroups() noexcept {
    // 함수 지역 정적 배열은 첫 호출 때 한 번 만든다(정의 배열 자체는 각 파일의 constexpr data다).
    static const Span<const MaterialNodeDefinition> groups[] = {
        detail::OutputNodeDefinitions(),
        detail::UtilityNodeDefinitions(),
        detail::MathNodeDefinitions(),
        detail::MaterialAttributesNodeDefinitions(),
        detail::ConstantsNodeDefinitions(),
        detail::ColorNodeDefinitions(),
        detail::CoordinatesNodeDefinitions(),
        detail::VectorsNodeDefinitions(),
        detail::FontNodeDefinitions(),
        detail::CustomNodeDefinitions(),
        detail::SwitchesNodeDefinitions(),
        detail::ParametersNodeDefinitions(),
        detail::TextureNodeDefinitions(),
        detail::DepthNodeDefinitions(),
        detail::WaterNodeDefinitions(),
        detail::DecalsNodeDefinitions(),
        detail::ParticlesNodeDefinitions(),
        detail::LightingNodeDefinitions(),
        detail::SkyNodeDefinitions(),
        detail::PostProcessNodeDefinitions(),
        detail::DistanceFieldsNodeDefinitions(),
        detail::NoiseNodeDefinitions(),
        detail::FunctionsNodeDefinitions(),
        detail::SurfaceNodeDefinitions(),
        detail::OutputsNodeDefinitions(),
        detail::VirtualTextureNodeDefinitions(),
        detail::MeshPaintNodeDefinitions(),
        detail::PhysicsFieldNodeDefinitions(),
        detail::SpeedTreeNodeDefinitions(),
    };

    return groups;
}

} // namespace rocks::material
