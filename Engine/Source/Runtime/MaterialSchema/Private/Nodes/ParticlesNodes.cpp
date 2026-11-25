#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Particles/DynamicParameterProperties.h>
#include <MaterialSchema/Nodes/Particles/ParticlePositionProperties.generated.h>
#include <MaterialSchema/Nodes/Particles/ParticlePositionProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Particles 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition ParticleColorPins[] = {
    MaterialOutput("RGB", MaterialPinType::Float3, "RGB"),
    MaterialOutput("R", MaterialPinType::Float1, "R"),
    MaterialOutput("G", MaterialPinType::Float1, "G"),
    MaterialOutput("B", MaterialPinType::Float1, "B"),
    MaterialOutput("A", MaterialPinType::Float1, "A"),
    MaterialOutput("RGBA", MaterialPinType::Float4, "RGBA"),
};

constexpr MaterialPinDefinition ParticleDirectionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleMacroUvPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleMotionBlurFadePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticlePositionPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleRadiusPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleRandomPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleRelativeTimePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleSizePins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleSpeedPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ParticleSpriteRotationPins[] = {
    MaterialOutput("Rad", MaterialPinType::Float1, "Rad"),
    MaterialOutput("Deg", MaterialPinType::Float1, "Deg"),
};

constexpr MaterialPinDefinition ParticleSubUvPropertiesPins[] = {
    MaterialOutput("TextureCoordinate0", MaterialPinType::Float2, "TextureCoordinate0"),
    MaterialOutput("TextureCoordinate1", MaterialPinType::Float2, "TextureCoordinate1"),
    MaterialOutput("Blend", MaterialPinType::Numeric, "Blend"),
};

constexpr MaterialPinDefinition SphericalParticleOpacityPins[] = {
    MaterialInput("Density", MaterialPinType::Numeric, "Density", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition ParticlesDefinitions[] = {
    MaterialNode("Material.DynamicParameter")
        .Properties(&GeneratedType<DynamicParameterProperties>::Schema)
        .DynamicPins(&DynamicParameterOutputPins)
        .Menu("Dynamic Parameter", "Particles", "Particles Parameters", "Dynamic Parameter.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleColor")
        .Pins(ParticleColorPins)
        .Menu("Particle Color", "Particles", "Particles Constants", "Particle Color.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleDirection")
        .Pins(ParticleDirectionPins)
        .Menu("Particle Direction", "Particles", "Particles Constants", "Particle Direction.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleMacroUv")
        .Pins(ParticleMacroUvPins)
        .Menu("Particle Macro Uv", "Particles", "Particles ParticleMacroUV",
              "This UV node generates texture coordinates in view space centered on the particle "
              "system's MacroUVPosition, with tiling controlled by the particle system's "
              "MacroUVRadius. It is useful for mapping a 'macro' noise texture in a continuous "
              "manner onto all particles of a particle system.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleMotionBlurFade")
        .Pins(ParticleMotionBlurFadePins)
        .Menu("Particle Motion Blur Fade", "Particles", "Particles Constants",
              "Particle Motion Blur Fade.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticlePosition")
        .Properties(&GeneratedType<ParticlePositionProperties>::Schema)
        .Pins(ParticlePositionPins)
        .Menu("Particle Position", "Particles", "Particles Coordinates Vectors ParticlePositionWS",
              "Particle Position.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleRadius")
        .Pins(ParticleRadiusPins)
        .Menu("Particle Radius", "Particles", "Particles Constants", "Particle Radius.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleRandom")
        .Pins(ParticleRandomPins)
        .Menu("Particle Random Value", "Particles", "Particles Constants", "Particle Random Value.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleRelativeTime")
        .Pins(ParticleRelativeTimePins)
        .Menu("Particle Relative Time", "Particles", "Particles Constants",
              "Particle Relative Time.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleSize")
        .Pins(ParticleSizePins)
        .Menu("Particle Size", "Particles", "Particles Constants", "Particle Size.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleSpeed")
        .Pins(ParticleSpeedPins)
        .Menu("Particle Speed", "Particles", "Particles Constants", "Particle Speed.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleSpriteRotation")
        .Pins(ParticleSpriteRotationPins)
        .Menu("Particle Sprite Rotation", "Particles", "Particles Constants",
              "Particle Sprite Rotation.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.ParticleSubUvProperties")
        .Pins(ParticleSubUvPropertiesPins)
        .Menu("Particle Sub Uv Properties", "Particles",
              "Particles Coordinates ParticleSubUVProperties", "Particle Sub Uv Properties.")
        .Features(ToMask(MaterialContextFeature::Particle)),
    MaterialNode("Material.SphericalParticleOpacity")
        .Pins(SphericalParticleOpacityPins)
        .Menu("Spherical Particle Opacity", "Particles", "Particles", "Spherical Particle Opacity.")
        .Features(ToMask(MaterialContextFeature::Particle)),
};

} // namespace

Span<const MaterialNodeDefinition> ParticlesNodeDefinitions() noexcept {
    return ParticlesDefinitions;
}

} // namespace rocks::material::detail
