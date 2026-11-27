#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Sky/SkyAtmosphereAerialPerspectiveProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereAerialPerspectiveProperties.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDirectionProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDirectionProperties.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDiskLuminanceProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightDiskLuminanceProperties.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightIlluminanceOnGroundProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightIlluminanceOnGroundProperties.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightIlluminanceProperties.generated.h>
#include <MaterialSchema/Nodes/Sky/SkyAtmosphereLightIlluminanceProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Sky 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SkyAtmosphereLightDirectionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereLightIlluminancePins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereLightIlluminanceOnGroundPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereLightDiskLuminancePins[] = {
    MaterialInput("DiskAngularDiameterOverride", MaterialPinType::Numeric,
                  "DiskAngularDiameterOverride", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereAerialPerspectivePins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereDistantLightScatteredLuminancePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyAtmosphereViewLuminancePins[] = {
    MaterialInput("WorldDirection", MaterialPinType::Numeric, "WorldDirection", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SkyLightEnvironmentSamplePins[] = {
    MaterialInput("Direction", MaterialPinType::Numeric, "Direction", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Roughness", MaterialPinType::Numeric, "Roughness", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition SkyDefinitions[] = {
    MaterialNode("Material.SkyAtmosphereLightDirection")
        .Properties(&GeneratedType<SkyAtmosphereLightDirectionProperties>::Schema)
        .Pins(SkyAtmosphereLightDirectionPins)
        .Menu("Sky Atmosphere Light Direction", "Sky", "Sky", "Sky Atmosphere Light Direction.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereLightIlluminance")
        .Properties(&GeneratedType<SkyAtmosphereLightIlluminanceProperties>::Schema)
        .Pins(SkyAtmosphereLightIlluminancePins)
        .Menu("Sky Atmosphere Light Illuminance", "Sky", "Sky", "Sky Atmosphere Light Illuminance.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereLightIlluminanceOnGround")
        .Properties(&GeneratedType<SkyAtmosphereLightIlluminanceOnGroundProperties>::Schema)
        .Pins(SkyAtmosphereLightIlluminanceOnGroundPins)
        .Menu("Sky Atmosphere Light Illuminance On Ground", "Sky", "Sky",
              "Sky Atmosphere Light Illuminance On Ground.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereLightDiskLuminance")
        .Properties(&GeneratedType<SkyAtmosphereLightDiskLuminanceProperties>::Schema)
        .Pins(SkyAtmosphereLightDiskLuminancePins)
        .Menu("Sky Atmosphere Light Disk Luminance", "Sky", "Sky",
              "Sky Atmosphere Light Disk Luminance.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereAerialPerspective")
        .Properties(&GeneratedType<SkyAtmosphereAerialPerspectiveProperties>::Schema)
        .Pins(SkyAtmosphereAerialPerspectivePins)
        .Menu("SkyAtmosphereAerialPerspective", "Sky", "Sky", "SkyAtmosphereAerialPerspective.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereDistantLightScatteredLuminance")
        .Pins(SkyAtmosphereDistantLightScatteredLuminancePins)
        .Menu("SkyAtmosphereDistantLightScatteredLuminance", "Sky", "Sky",
              "SkyAtmosphereDistantLightScatteredLuminance.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyAtmosphereViewLuminance")
        .Pins(SkyAtmosphereViewLuminancePins)
        .Menu("SkyAtmosphereViewLuminance", "Sky", "Sky", "SkyAtmosphereViewLuminance.")
        .Features(ToMask(MaterialContextFeature::Sky)),
    MaterialNode("Material.SkyLightEnvironmentSample")
        .Pins(SkyLightEnvironmentSamplePins)
        .Menu("Sky Light Environment Sample", "Sky", "Sky SkyLightEnvMapSample",
              "Sky Light Environment Sample.")
        .Features(ToMask(MaterialContextFeature::Sky)),
};

} // namespace

Span<const MaterialNodeDefinition> SkyNodeDefinitions() noexcept {
    return SkyDefinitions;
}

} // namespace rocks::material::detail
