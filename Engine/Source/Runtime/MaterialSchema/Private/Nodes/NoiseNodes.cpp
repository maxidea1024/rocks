#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Noise/NoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/NoiseProperties.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.generated.h>
#include <MaterialSchema/Nodes/Noise/VectorNoiseProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Noise 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition NoisePins[] = {
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("FilterWidth", MaterialPinType::Numeric, "FilterWidth", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition VectorNoisePins[] = {
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ScalarBlueNoisePins[] = {
    MaterialOutput("Result", MaterialPinType::Float1, ""),
};

constexpr MaterialPinDefinition SobolPins[] = {
    MaterialInput("Cell", MaterialPinType::Numeric, "Cell", {}, MaterialPinFlags::HideValue),
    MaterialInput("Index", MaterialPinType::Numeric, "Index", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Seed", MaterialPinType::Numeric, "Seed",
                  MaterialPinDefault::Vector2(0.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TemporalSobolPins[] = {
    MaterialInput("Index", MaterialPinType::Numeric, "Index", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Seed", MaterialPinType::Numeric, "Seed",
                  MaterialPinDefault::Vector2(0.0f, 0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition NoiseDefinitions[] = {
    MaterialNode("Material.Noise")
        .Properties(&GeneratedType<NoiseProperties>::Schema)
        .Pins(NoisePins)
        .Menu("Noise", "Noise", "Utility", "Noise."),
    MaterialNode("Material.VectorNoise")
        .Properties(&GeneratedType<VectorNoiseProperties>::Schema)
        .Pins(VectorNoisePins)
        .Menu("Vector Noise", "Noise", "Utility", "Vector Noise."),
    MaterialNode("Material.ScalarBlueNoise")
        .Pins(ScalarBlueNoisePins)
        .Menu("Scalar Blue Noise", "Noise", "Noise", "Scalar Blue Noise.")
        .Features(ToMask(MaterialContextFeature::View)),
    MaterialNode("Material.Sobol").Pins(SobolPins).Menu("Sobol", "Noise", "Utility", "Sobol."),
    MaterialNode("Material.TemporalSobol")
        .Pins(TemporalSobolPins)
        .Menu("Temporal Sobol", "Noise", "Utility", "Temporal Sobol.")
        .Features(ToMask(MaterialContextFeature::View)),
};

} // namespace

Span<const MaterialNodeDefinition> NoiseNodeDefinitions() noexcept {
    return NoiseDefinitions;
}

} // namespace rocks::material::detail
