#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Coordinates/LocalPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/LocalPositionProperties.h>
#include <MaterialSchema/Nodes/Coordinates/PannerProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/PannerProperties.h>
#include <MaterialSchema/Nodes/Coordinates/RotatorProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/RotatorProperties.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/WorldPositionProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Coordinates 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition TextureCoordinatePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition WorldPositionPins[] = {
    MaterialOutput("XYZ", MaterialPinType::Float3, "XYZ"),
    MaterialOutput("XY", MaterialPinType::Float2, "XY"),
    MaterialOutput("Z", MaterialPinType::Float1, "Z"),
};

constexpr MaterialPinDefinition LocalPositionPins[] = {
    MaterialOutput("XYZ", MaterialPinType::Float3, "XYZ"),
    MaterialOutput("XY", MaterialPinType::Float2, "XY"),
    MaterialOutput("Z", MaterialPinType::Float1, "Z"),
};

constexpr MaterialPinDefinition ScreenPositionPins[] = {
    MaterialOutput("ViewportUV", MaterialPinType::Numeric, "ViewportUV"),
    MaterialOutput("PixelPosition", MaterialPinType::Numeric, "PixelPosition"),
};

constexpr MaterialPinDefinition ViewSizePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SceneTexelSizePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PixelDepthPins[] = {
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition PannerPins[] = {
    MaterialInput("Coordinate", MaterialPinType::Numeric, "Coordinate", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Time", MaterialPinType::Numeric, "Time", {}, MaterialPinFlags::HideValue),
    MaterialInput("Speed", MaterialPinType::Numeric, "Speed",
                  MaterialPinDefault::Vector2(0.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RotatorPins[] = {
    MaterialInput("Coordinate", MaterialPinType::Numeric, "Coordinate", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Time", MaterialPinType::Numeric, "Time", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TruncateWorldPositionPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LightmapUvsPins[] = {
    MaterialOutput("Result", MaterialPinType::Float2, ""),
};

constexpr MaterialPinDefinition ObjectRadiusPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition XrPassthroughCameraUvPins[] = {
    MaterialInput("Coordinates", MaterialPinType::Numeric, "Coordinates", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition CoordinatesDefinitions[] = {
    MaterialNode("Material.TextureCoordinate")
        .Properties(&GeneratedType<TextureCoordinateProperties>::Schema)
        .Pins(TextureCoordinatePins)
        .Menu("Texture Coordinate", "Coordinates", "Coordinates", "Texture Coordinate.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.WorldPosition")
        .Properties(&GeneratedType<WorldPositionProperties>::Schema)
        .Pins(WorldPositionPins)
        .Menu("World Position", "Coordinates", "Coordinates", "World Position.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.LocalPosition")
        .Properties(&GeneratedType<LocalPositionProperties>::Schema)
        .Pins(LocalPositionPins)
        .Menu("Local Position", "Coordinates", "Coordinates", "Local Position.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.ScreenPosition")
        .Pins(ScreenPositionPins)
        .Menu("ScreenPosition", "Coordinates", "Coordinates", "ScreenPosition.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.ViewSize")
        .Pins(ViewSizePins)
        .Menu("ViewSize", "Coordinates", "Coordinates", "ViewSize.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.SceneTexelSize")
        .Pins(SceneTexelSizePins)
        .Menu("SceneTexelSize", "Coordinates", "Coordinates", "SceneTexelSize.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.PixelDepth")
        .Pins(PixelDepthPins)
        .Menu("PixelDepth", "Coordinates", "Depth",
              "Gives the depth of the current pixel being drawn for use in a material")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.Panner")
        .Properties(&GeneratedType<PannerProperties>::Schema)
        .Pins(PannerPins)
        .Menu("Panner", "Coordinates", "Coordinates", "Panner.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.Rotator")
        .Properties(&GeneratedType<RotatorProperties>::Schema)
        .Pins(RotatorPins)
        .Menu("Rotator", "Coordinates", "Coordinates", "Rotator.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.TruncateWorldPosition")
        .Pins(TruncateWorldPositionPins)
        .Menu("Truncate World Position", "Coordinates", "Coordinates TruncateLWC",
              "Truncate World Position."),
    MaterialNode("Material.LightmapUvs")
        .Pins(LightmapUvsPins)
        .Menu("LightmapUVs", "Coordinates", "Coordinates LightmapUVs",
              "A material expression that routes LightmapUVs to the material.")
        .Features(ToMask(MaterialContextFeature::Lightmap)),
    MaterialNode("Material.ObjectRadius")
        .Pins(ObjectRadiusPins)
        .Menu("Object Radius", "Coordinates", "Coordinates", "Object Radius.")
        .Features(ToMask(MaterialContextFeature::Primitive)),
    MaterialNode("Material.XrPassthroughCameraUv")
        .Pins(XrPassthroughCameraUvPins)
        .Menu("Xr Passthrough Camera Uv", "Coordinates", "Coordinates MapARPassthroughCameraUV",
              "A material expression that maps viewport UVs to AR passthrough camera UVs taking "
              "into account aspect ratio and device rotation.")
        .Features(ToMask(MaterialContextFeature::Xr)),
};

} // namespace

Span<const MaterialNodeDefinition> CoordinatesNodeDefinitions() noexcept {
    return CoordinatesDefinitions;
}

} // namespace rocks::material::detail
