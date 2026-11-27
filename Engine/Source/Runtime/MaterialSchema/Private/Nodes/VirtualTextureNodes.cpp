#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleParameterProperties.generated.h>
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleParameterProperties.h>
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleProperties.generated.h>
#include <MaterialSchema/Nodes/VirtualTexture/RuntimeVirtualTextureSampleProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Virtual Texture 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition RuntimeVirtualTextureSamplePins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("BaseColor", MaterialPinType::Float3, "BaseColor"),
    MaterialOutput("Specular", MaterialPinType::Numeric, "Specular"),
    MaterialOutput("Roughness", MaterialPinType::Numeric, "Roughness"),
    MaterialOutput("Normal", MaterialPinType::Numeric, "Normal"),
    MaterialOutput("WorldHeight", MaterialPinType::Numeric, "WorldHeight"),
    MaterialOutput("Mask", MaterialPinType::Numeric, "Mask"),
    MaterialOutput("Displacement", MaterialPinType::Numeric, "Displacement"),
    MaterialOutput("Mask4", MaterialPinType::Numeric, "Mask4"),
};

constexpr StringView RuntimeVirtualTextureSampleParameterNonSemantic[] = {"virtualTexture", "group",
                                                                          "sortPriority"};
constexpr MaterialPinDefinition RuntimeVirtualTextureSampleParameterPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("BaseColor", MaterialPinType::Float3, "BaseColor"),
    MaterialOutput("Specular", MaterialPinType::Numeric, "Specular"),
    MaterialOutput("Roughness", MaterialPinType::Numeric, "Roughness"),
    MaterialOutput("Normal", MaterialPinType::Numeric, "Normal"),
    MaterialOutput("WorldHeight", MaterialPinType::Numeric, "WorldHeight"),
    MaterialOutput("Mask", MaterialPinType::Numeric, "Mask"),
    MaterialOutput("Displacement", MaterialPinType::Numeric, "Displacement"),
    MaterialOutput("Mask4", MaterialPinType::Numeric, "Mask4"),
};

constexpr MaterialPinDefinition RuntimeVirtualTextureCustomDataPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition VirtualTextureDefinitions[] = {
    MaterialNode("Material.RuntimeVirtualTextureSample")
        .Properties(&GeneratedType<RuntimeVirtualTextureSampleProperties>::Schema)
        .Pins(RuntimeVirtualTextureSamplePins)
        .DynamicPins(&RuntimeVirtualTextureMipPins<RuntimeVirtualTextureSampleProperties>)
        .Menu("Runtime Virtual Texture Sample", "Virtual Texture", "VirtualTexture",
              "Material expression for sampling from a runtime virtual texture.")
        .Missing(MaterialPrerequisite::VirtualTexture),
    MaterialNode("Material.RuntimeVirtualTextureSampleParameter")
        .Properties(&GeneratedType<RuntimeVirtualTextureSampleParameterProperties>::Schema,
                    RuntimeVirtualTextureSampleParameterNonSemantic)
        .Pins(RuntimeVirtualTextureSampleParameterPins)
        .DynamicPins(&RuntimeVirtualTextureMipPins<RuntimeVirtualTextureSampleParameterProperties>)
        .Menu("Runtime Virtual Texture Sample Parameter", "Virtual Texture",
              "VirtualTexture Parameters", "Runtime Virtual Texture Sample Parameter.")
        .Flags(MaterialNodeFlags::Parameter)
        .Missing(MaterialPrerequisite::VirtualTexture),
    MaterialNode("Material.RuntimeVirtualTextureCustomData")
        .Pins(RuntimeVirtualTextureCustomDataPins)
        .Menu("RuntimeVirtualTextureCustomData", "Virtual Texture", "VirtualTexture",
              "RuntimeVirtualTextureCustomData.")
        .Features(ToMask(MaterialContextFeature::RuntimeVirtualTexture)),
};

} // namespace

Span<const MaterialNodeDefinition> VirtualTextureNodeDefinitions() noexcept {
    return VirtualTextureDefinitions;
}

} // namespace rocks::material::detail
