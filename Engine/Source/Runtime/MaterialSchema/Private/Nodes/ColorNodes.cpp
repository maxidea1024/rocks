#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <MaterialSchema/Nodes/Color/DesaturationProperties.generated.h>
#include <MaterialSchema/Nodes/Color/DesaturationProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Color 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

// 속성 에셋 참조 선언이다(Docs/Design/MaterialSystem 19.4.3절 "참조 선언").
constexpr MaterialAssetReferenceField CurveReference[] = {
    {"colorCurve", MaterialAssetReferenceRole::ColorCurve}};

constexpr MaterialPinDefinition DesaturationPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Fraction", MaterialPinType::Numeric, "Fraction", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition HsvToRgbPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RgbToHsvPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition BlackBodyPins[] = {
    MaterialInput("Temp", MaterialPinType::Numeric, "Temp", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ColorRampPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", MaterialPinDefault::Scalar(0.5f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SrgbToWorkingColorSpacePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition ColorDefinitions[] = {
    MaterialNode("Material.Desaturation")
        .Properties(&GeneratedType<DesaturationProperties>::Schema)
        .Pins(DesaturationPins)
        .Menu("Desaturation", "Color", "Color Utility", "Desaturation."),
    MaterialNode("Material.HsvToRgb")
        .Pins(HsvToRgbPins)
        .Menu("HSVToRGB", "Color", "Math", "HSVToRGB."),
    MaterialNode("Material.RgbToHsv")
        .Pins(RgbToHsvPins)
        .Menu("RGBToHSV", "Color", "Math", "RGBToHSV."),
    MaterialNode("Material.BlackBody")
        .Pins(BlackBodyPins)
        .Menu("BlackBody", "Color", "Utility", "BlackBody."),
    MaterialNode("Material.ColorRamp")
        .AssetReferences(CurveReference)
        .Properties(&GeneratedType<ColorRampProperties>::Schema)
        .Pins(ColorRampPins)
        .Menu("Color Ramp", "Color", "Color", "Color Ramp."),
    MaterialNode("Material.SrgbToWorkingColorSpace")
        .Pins(SrgbToWorkingColorSpacePins)
        .Menu("Srgb To Working Color Space", "Color", "Color SRGBColorToWorkingColorSpace",
              "Srgb To Working Color Space."),
};

} // namespace

Span<const MaterialNodeDefinition> ColorNodeDefinitions() noexcept {
    return ColorDefinitions;
}

} // namespace rocks::material::detail
