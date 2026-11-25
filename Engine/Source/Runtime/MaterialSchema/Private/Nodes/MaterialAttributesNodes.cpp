#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AggregateProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/BlendProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/GetAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/SetAttributesProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/ShadingModelProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Material Attributes 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition AggregatePins[] = {
    MaterialInput("PrototypeInput", MaterialPinType::Any, "Prototype", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition BlendPins[] = {
    MaterialInput("A", MaterialPinType::Any, "A", {}, MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Any, "B", {}, MaterialPinFlags::HideValue),
    MaterialInput("Alpha", MaterialPinType::Float1, "Alpha", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition MakeAttributesPins[] = {
    MaterialInput("BaseColor", MaterialPinType::Numeric, "BaseColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Metallic", MaterialPinType::Numeric, "Metallic", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Specular", MaterialPinType::Numeric, "Specular", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Numeric, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Anisotropy", MaterialPinType::Numeric, "Anisotropy", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("EmissiveColor", MaterialPinType::Numeric, "EmissiveColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Opacity", MaterialPinType::Numeric, "Opacity", {}, MaterialPinFlags::HideValue),
    MaterialInput("OpacityMask", MaterialPinType::Numeric, "OpacityMask", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Normal", MaterialPinType::Numeric, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialInput("Tangent", MaterialPinType::Numeric, "Tangent", {}, MaterialPinFlags::HideValue),
    MaterialInput("WorldPositionOffset", MaterialPinType::Numeric, "WorldPositionOffset", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("SubsurfaceColor", MaterialPinType::Numeric, "SubsurfaceColor", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoat", MaterialPinType::Numeric, "ClearCoat", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ClearCoatRoughness", MaterialPinType::Numeric, "ClearCoatRoughness", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("AmbientOcclusion", MaterialPinType::Numeric, "AmbientOcclusion", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Refraction", MaterialPinType::Numeric, "Refraction", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_0", MaterialPinType::Numeric, "CustomizedUVs_0", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_1", MaterialPinType::Numeric, "CustomizedUVs_1", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_2", MaterialPinType::Numeric, "CustomizedUVs_2", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_3", MaterialPinType::Numeric, "CustomizedUVs_3", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_4", MaterialPinType::Numeric, "CustomizedUVs_4", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_5", MaterialPinType::Numeric, "CustomizedUVs_5", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_6", MaterialPinType::Numeric, "CustomizedUVs_6", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("CustomizedUVs_7", MaterialPinType::Numeric, "CustomizedUVs_7", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("PixelDepthOffset", MaterialPinType::Numeric, "PixelDepthOffset", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("ShadingModel", MaterialPinType::ShadingModel, "ShadingModel", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Displacement", MaterialPinType::Numeric, "Displacement", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::MaterialAttributes, ""),
};

constexpr MaterialPinDefinition BreakAttributesPins[] = {
    MaterialInput("MaterialAttributes", MaterialPinType::MaterialAttributes, "Attr", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("BaseColor", MaterialPinType::Float3, "BaseColor"),
    MaterialOutput("Metallic", MaterialPinType::Float1, "Metallic"),
    MaterialOutput("Specular", MaterialPinType::Float1, "Specular"),
    MaterialOutput("Roughness", MaterialPinType::Float1, "Roughness"),
    MaterialOutput("Anisotropy", MaterialPinType::Float1, "Anisotropy"),
    MaterialOutput("EmissiveColor", MaterialPinType::Float3, "EmissiveColor"),
    MaterialOutput("Opacity", MaterialPinType::Float1, "Opacity"),
    MaterialOutput("OpacityMask", MaterialPinType::Float1, "OpacityMask"),
    MaterialOutput("Normal", MaterialPinType::Float3, "Normal"),
    MaterialOutput("Tangent", MaterialPinType::Float3, "Tangent"),
    MaterialOutput("WorldPositionOffset", MaterialPinType::Float3, "WorldPositionOffset"),
    MaterialOutput("SubsurfaceColor", MaterialPinType::Float3, "SubsurfaceColor"),
    MaterialOutput("ClearCoat", MaterialPinType::Float1, "ClearCoat"),
    MaterialOutput("ClearCoatRoughness", MaterialPinType::Float1, "ClearCoatRoughness"),
    MaterialOutput("AmbientOcclusion", MaterialPinType::Float1, "AmbientOcclusion"),
    MaterialOutput("Refraction", MaterialPinType::Float2, "Refraction"),
    MaterialOutput("CustomizedUV0", MaterialPinType::Float2, "CustomizedUV0"),
    MaterialOutput("CustomizedUV1", MaterialPinType::Float2, "CustomizedUV1"),
    MaterialOutput("CustomizedUV2", MaterialPinType::Float2, "CustomizedUV2"),
    MaterialOutput("CustomizedUV3", MaterialPinType::Float2, "CustomizedUV3"),
    MaterialOutput("CustomizedUV4", MaterialPinType::Float2, "CustomizedUV4"),
    MaterialOutput("CustomizedUV5", MaterialPinType::Float2, "CustomizedUV5"),
    MaterialOutput("CustomizedUV6", MaterialPinType::Float2, "CustomizedUV6"),
    MaterialOutput("CustomizedUV7", MaterialPinType::Float2, "CustomizedUV7"),
    MaterialOutput("PixelDepthOffset", MaterialPinType::Float1, "PixelDepthOffset"),
    MaterialOutput("ShadingModel", MaterialPinType::ShadingModel, "ShadingModel"),
    MaterialOutput("Displacement", MaterialPinType::Float1, "Displacement"),
};

constexpr MaterialPinDefinition GetAttributesPins[] = {
    MaterialInput("MaterialAttributes", MaterialPinType::MaterialAttributes, "MaterialAttributes",
                  {}, MaterialPinFlags::HideValue),
    MaterialOutput("MaterialAttributesOut", MaterialPinType::MaterialAttributes,
                   "MaterialAttributes"),
};

constexpr MaterialPinDefinition SetAttributesPins[] = {
    MaterialInput("MaterialAttributes", MaterialPinType::MaterialAttributes, "MaterialAttributes",
                  {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::MaterialAttributes, ""),
};

constexpr MaterialPinDefinition BlendAttributesPins[] = {
    MaterialInput("A", MaterialPinType::MaterialAttributes, "A", {}, MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::MaterialAttributes, "B", {}, MaterialPinFlags::HideValue),
    MaterialInput("Alpha", MaterialPinType::Float1, "Alpha", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::MaterialAttributes, ""),
};

constexpr MaterialPinDefinition AttributeLayersPins[] = {
    MaterialInput("Input", MaterialPinType::MaterialAttributes, "Input", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::MaterialAttributes, ""),
};

constexpr MaterialPinDefinition LayerStackPins[] = {
    MaterialOutput("Result", MaterialPinType::MaterialAttributes, ""),
};

constexpr MaterialPinDefinition ShadingModelPins[] = {
    MaterialOutput("Result", MaterialPinType::ShadingModel, ""),
};

constexpr MaterialNodeDefinition MaterialAttributesDefinitions[] = {
    MaterialNode("Material.Aggregate")
        .Properties(&GeneratedType<AggregateProperties>::Schema)
        .Pins(AggregatePins)
        .DynamicPins(&AggregateDynamicPins)
        .Menu("Aggregate", "Material Attributes", "Aggregates", "Aggregate."),
    MaterialNode("Material.Blend")
        .Properties(&GeneratedType<BlendProperties>::Schema)
        .Pins(BlendPins)
        .Menu("Blend", "Material Attributes", "Math", "Blend."),
    MaterialNode("Material.MakeAttributes")
        .Pins(MakeAttributesPins)
        .Menu("Make Attributes", "Material Attributes",
              "Material Attributes MakeMaterialAttributes", "Make Attributes."),
    MaterialNode("Material.BreakAttributes")
        .Pins(BreakAttributesPins)
        .Menu("Break Attributes", "Material Attributes",
              "Material Attributes BreakMaterialAttributes", "Break Attributes."),
    MaterialNode("Material.GetAttributes")
        .Properties(&GeneratedType<GetAttributesProperties>::Schema)
        .Pins(GetAttributesPins)
        .DynamicPins(&GetAttributesDynamicPins)
        .Menu("Get Attributes", "Material Attributes", "Material Attributes GetMaterialAttributes",
              "Get Attributes."),
    MaterialNode("Material.SetAttributes")
        .Properties(&GeneratedType<SetAttributesProperties>::Schema)
        .Pins(SetAttributesPins)
        .DynamicPins(&SetAttributesDynamicPins)
        .Menu("Set Attributes", "Material Attributes", "Material Attributes SetMaterialAttributes",
              "Set Attributes."),
    MaterialNode("Material.BlendAttributes")
        .Properties(&GeneratedType<BlendAttributesProperties>::Schema)
        .Pins(BlendAttributesPins)
        .Menu("Blend Attributes", "Material Attributes",
              "Material Attributes BlendMaterialAttributes", "Blend Attributes."),
    MaterialNode("Material.AttributeLayers")
        .Properties(&GeneratedType<AttributeLayersProperties>::Schema)
        .Pins(AttributeLayersPins)
        .Menu("Attribute Layers", "Material Attributes",
              "Material Attributes Parameters MaterialAttributeLayers", "Attribute Layers."),
    MaterialNode("Material.LayerStack")
        .Properties(&GeneratedType<LayerStackProperties>::Schema)
        .Pins(LayerStackPins)
        .DynamicPins(&LayerStackDynamicPins)
        .Menu("Layer Stack", "Material Attributes", "", "Layer Stack.")
        .Flags(MaterialNodeFlags::Experimental),
    MaterialNode("Material.ShadingModel")
        .Properties(&GeneratedType<ShadingModelProperties>::Schema)
        .Pins(ShadingModelPins)
        .Menu("Shading Model", "Material Attributes", "Shading Model", "Shading Model."),
};

} // namespace

Span<const MaterialNodeDefinition> MaterialAttributesNodeDefinitions() noexcept {
    return MaterialAttributesDefinitions;
}

} // namespace rocks::material::detail
