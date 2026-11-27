#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Vectors/ActorPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/ActorPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/BoundsProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/BoundsProperties.h>
#include <MaterialSchema/Nodes/Vectors/ObjectPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/ObjectPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/ReflectionVectorProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/ReflectionVectorProperties.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformPositionProperties.h>
#include <MaterialSchema/Nodes/Vectors/TransformVectorProperties.generated.h>
#include <MaterialSchema/Nodes/Vectors/TransformVectorProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Vectors 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition CameraPositionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition CameraVectorPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ObjectPositionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ActorPositionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ObjectOrientationPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ObjectBoundsPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ObjectLocalBoundsPins[] = {
    MaterialOutput("Half Extents", MaterialPinType::Float3, "Half Extents"),
    MaterialOutput("Extents", MaterialPinType::Float3, "Extents"),
    MaterialOutput("Min", MaterialPinType::Float3, "Min"),
    MaterialOutput("Max", MaterialPinType::Float3, "Max"),
};

constexpr MaterialPinDefinition BoundsPins[] = {
    MaterialOutput("Half Extents", MaterialPinType::Float3, "Half Extents"),
    MaterialOutput("Extents", MaterialPinType::Float3, "Extents"),
    MaterialOutput("Min", MaterialPinType::Float3, "Min"),
    MaterialOutput("Max", MaterialPinType::Float3, "Max"),
};

constexpr MaterialPinDefinition PreSkinnedLocalBoundsPins[] = {
    MaterialOutput("Half Extents", MaterialPinType::Float3, "Half Extents"),
    MaterialOutput("Extents", MaterialPinType::Float3, "Extents"),
    MaterialOutput("Min", MaterialPinType::Float3, "Min"),
    MaterialOutput("Max", MaterialPinType::Float3, "Max"),
};

constexpr MaterialPinDefinition PreSkinnedPositionPins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
};

constexpr MaterialPinDefinition PreSkinnedNormalPins[] = {
    MaterialOutput("Result", MaterialPinType::Float3, ""),
};

constexpr MaterialPinDefinition VertexNormalPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition VertexTangentPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PixelNormalPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ReflectionVectorPins[] = {
    MaterialInput("CustomWorldNormal", MaterialPinType::Numeric, "CustomWorldNormal", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LightVectorPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TransformVectorPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TransformPositionPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition VectorsDefinitions[] = {
    MaterialNode("Material.CameraPosition")
        .Pins(CameraPositionPins)
        .Menu("Camera Position", "Vectors", "Vectors Coordinates CameraPositionWS",
              "Camera Position.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.CameraVector")
        .Pins(CameraVectorPins)
        .Menu("Camera Vector", "Vectors", "Vectors CameraVectorWS", "Camera Vector.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.ObjectPosition")
        .Properties(&GeneratedType<ObjectPositionProperties>::Schema)
        .Pins(ObjectPositionPins)
        .Menu("Object Position", "Vectors", "Vectors Coordinates ObjectPositionWS",
              "Object Position.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.ActorPosition")
        .Properties(&GeneratedType<ActorPositionProperties>::Schema)
        .Pins(ActorPositionPins)
        .Menu("Actor Position", "Vectors", "Vectors Coordinates ActorPositionWS", "Actor Position.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.ObjectOrientation")
        .Pins(ObjectOrientationPins)
        .Menu("ObjectOrientation", "Vectors", "Vectors Coordinates", "ObjectOrientation.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.ObjectBounds")
        .Pins(ObjectBoundsPins)
        .Menu("Object Bounds", "Vectors", "Vectors", "Object Bounds.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.ObjectLocalBounds")
        .Pins(ObjectLocalBoundsPins)
        .Menu("Object Local Bounds", "Vectors", "Vectors", "Object Local Bounds.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.Bounds")
        .Properties(&GeneratedType<BoundsProperties>::Schema)
        .Pins(BoundsPins)
        .Menu("Bounds", "Vectors", "Vectors", "Bounds.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.PreSkinnedLocalBounds")
        .Pins(PreSkinnedLocalBoundsPins)
        .Menu("Pre-Skinned Local Bounds", "Vectors", "Vectors", "Pre-Skinned Local Bounds.")
        .Features(ToMask(MaterialContextFeature::Skin)),
    MaterialNode("Material.PreSkinnedPosition")
        .Pins(PreSkinnedPositionPins)
        .Menu("Pre-Skinned Local Position", "Vectors", "Vectors", "Pre-Skinned Local Position.")
        .Features(ToMask(MaterialContextFeature::Skin)),
    MaterialNode("Material.PreSkinnedNormal")
        .Pins(PreSkinnedNormalPins)
        .Menu("Pre-Skinned Local Normal", "Vectors", "Vectors", "Pre-Skinned Local Normal.")
        .Features(ToMask(MaterialContextFeature::Skin)),
    MaterialNode("Material.VertexNormal")
        .Pins(VertexNormalPins)
        .Menu("Vertex Normal", "Vectors", "Vectors Coordinates VertexNormalWS", "Vertex Normal.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.VertexTangent")
        .Pins(VertexTangentPins)
        .Menu("Vertex Tangent", "Vectors", "Vectors Coordinates VertexTangentWS", "Vertex Tangent.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.PixelNormal")
        .Pins(PixelNormalPins)
        .Menu("Pixel Normal", "Vectors", "Vectors Coordinates PixelNormalWS", "Pixel Normal.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.ReflectionVector")
        .Properties(&GeneratedType<ReflectionVectorProperties>::Schema)
        .Pins(ReflectionVectorPins)
        .Menu("Reflection Vector", "Vectors", "Vectors ReflectionVectorWS", "Reflection Vector.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.LightVector")
        .Pins(LightVectorPins)
        .Menu("Light Vector", "Vectors", "Vectors", "Light Vector.")
        .Features(ToMask(MaterialContextFeature::Light)),
    MaterialNode("Material.TransformVector")
        .Properties(&GeneratedType<TransformVectorProperties>::Schema)
        .Pins(TransformVectorPins)
        .Menu("Transform Vector", "Vectors", "Vectors Transform", "Transform Vector.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.TransformPosition")
        .Properties(&GeneratedType<TransformPositionProperties>::Schema)
        .Pins(TransformPositionPins)
        .DynamicPins(&TransformPositionDynamicPins)
        .Menu("Transform Position", "Vectors", "Vectors", "Transform Position.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
};

} // namespace

Span<const MaterialNodeDefinition> VectorsNodeDefinitions() noexcept {
    return VectorsDefinitions;
}

} // namespace rocks::material::detail
