#include "MaterialBuiltinNodes.h"

#include "MaterialDynamicPins.h"
#include <MaterialSchema/Nodes/Math/BumpOffsetProperties.generated.h>
#include <MaterialSchema/Nodes/Math/BumpOffsetProperties.h>
#include <MaterialSchema/Nodes/Math/ClampProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ClampProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/ConstantBiasScaleProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ConstantBiasScaleProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/FloatToUIntProperties.generated.h>
#include <MaterialSchema/Nodes/Math/FloatToUIntProperties.h>
#include <MaterialSchema/Nodes/Math/IfProperties.generated.h>
#include <MaterialSchema/Nodes/Math/IfProperties.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.generated.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Math/RotateAboutAxisProperties.generated.h>
#include <MaterialSchema/Nodes/Math/RotateAboutAxisProperties.h>
#include <MaterialSchema/Nodes/Math/SineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SineProperties.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.h>
#include <MaterialSchema/Nodes/Math/TangentProperties.generated.h>
#include <MaterialSchema/Nodes/Math/TangentProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Math 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition DynamicBranchPins[] = {
    MaterialInput("Condition", MaterialPinType::Bool, "Condition", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("True", MaterialPinType::Any, "True", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("False", MaterialPinType::Any, "False", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPinDefinition AbsPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition AddPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SubtractPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition MultiplyPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DividePins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(2.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition MinPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition MaxPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ClampPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Min", MaterialPinType::Numeric, "Min", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Max", MaterialPinType::Numeric, "Max", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SaturatePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition OneMinusPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FloorPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition CeilPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RoundPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TruncatePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FracPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FmodPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ModuloPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SignPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition StepPins[] = {
    MaterialInput("Y", MaterialPinType::Numeric, "Y", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("X", MaterialPinType::Numeric, "X", MaterialPinDefault::Scalar(1.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SmoothStepPins[] = {
    MaterialInput("Min", MaterialPinType::Numeric, "Min", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Max", MaterialPinType::Numeric, "Max", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("Value", MaterialPinType::Numeric, "Value", MaterialPinDefault::Scalar(0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition PowerPins[] = {
    MaterialInput("Base", MaterialPinType::Numeric, "Base", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Exponent", MaterialPinType::Numeric, "Exponent",
                  MaterialPinDefault::Scalar(2.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SquareRootPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ExponentialPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition Exponential2Pins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LogarithmPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition Logarithm2Pins[] = {
    MaterialInput("X", MaterialPinType::Numeric, "X", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition Logarithm10Pins[] = {
    MaterialInput("X", MaterialPinType::Numeric, "X", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SinePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition CosinePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition TangentPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArcsinePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArccosinePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArctangentPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition Arctangent2Pins[] = {
    MaterialInput("Y", MaterialPinType::Numeric, "Y", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("X", MaterialPinType::Numeric, "X", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArcsineFastPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArccosineFastPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ArctangentFastPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition Arctangent2FastPins[] = {
    MaterialInput("Y", MaterialPinType::Numeric, "Y", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("X", MaterialPinType::Numeric, "X", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LinearInterpolatePins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("Alpha", MaterialPinType::Numeric, "Alpha", MaterialPinDefault::Scalar(0.5f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition InverseLinearInterpolatePins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(1.0f)),
    MaterialInput("Value", MaterialPinType::Numeric, "Value", MaterialPinDefault::Scalar(0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DotProductPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition CrossProductPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition NormalizePins[] = {
    MaterialInput("VectorInput", MaterialPinType::Numeric, "VectorInput", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition LengthPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DistancePins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition AppendPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ComponentMaskPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition IfPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("AGreaterThanB", MaterialPinType::Any, "AGreaterThanB", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("AEqualsB", MaterialPinType::Any, "AEqualsB", {}, MaterialPinFlags::HideValue),
    MaterialInput("ALessThanB", MaterialPinType::Any, "ALessThanB", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Any, ""),
};

constexpr MaterialPropertyTextDefault SwitchTextDefaults[] = {{"description", "Switch"}};
constexpr MaterialPinDefinition SwitchPins[] = {
    MaterialInput("SwitchValue", MaterialPinType::Numeric, "SwitchValue",
                  MaterialPinDefault::Scalar(0.0f)),
    MaterialInput("Default", MaterialPinType::Numeric, "Default", MaterialPinDefault::Scalar(0.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FloatToUIntPins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::UInt1, ""),
};

constexpr MaterialPinDefinition UIntToFloatPins[] = {
    MaterialInput("Input", MaterialPinType::UInt1, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition ConstantBiasScalePins[] = {
    MaterialInput("Input", MaterialPinType::Numeric, "Input", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPropertyTextDefault ConvertTextDefaults[] = {{"nodeName", "Convert"}};
constexpr MaterialPinDefinition OperatorPins[] = {
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DdxPins[] = {
    MaterialInput("Value", MaterialPinType::Numeric, "Value", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DdyPins[] = {
    MaterialInput("Value", MaterialPinType::Numeric, "Value", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition RotateAboutAxisPins[] = {
    MaterialInput("NormalizedRotationAxis", MaterialPinType::Numeric, "NormalizedRotationAxis", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("RotationAngle", MaterialPinType::Numeric, "RotationAngle", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("PivotPoint", MaterialPinType::Numeric, "PivotPoint", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Position", MaterialPinType::Numeric, "Position", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SphereMaskPins[] = {
    MaterialInput("A", MaterialPinType::Numeric, "A", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("B", MaterialPinType::Numeric, "B", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("Radius", MaterialPinType::Numeric, "Radius", MaterialPinDefault::Scalar(256.0f)),
    MaterialInput("Hardness", MaterialPinType::Numeric, "Hardness",
                  MaterialPinDefault::Scalar(100.0f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition DeriveNormalZPins[] = {
    MaterialInput("InXY", MaterialPinType::Numeric, "InXY", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition FresnelPins[] = {
    MaterialInput("ExponentIn", MaterialPinType::Numeric, "ExponentIn",
                  MaterialPinDefault::Scalar(5.0f)),
    MaterialInput("BaseReflectFractionIn", MaterialPinType::Numeric, "BaseReflectFractionIn",
                  MaterialPinDefault::Scalar(0.04f)),
    MaterialInput("Normal", MaterialPinType::Numeric, "Normal", {}, MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition BumpOffsetPins[] = {
    MaterialInput("Coordinate", MaterialPinType::Numeric, "Coordinate", {},
                  MaterialPinFlags::HideValue),
    MaterialInput("Height", MaterialPinType::Numeric, "Height", {},
                  MaterialPinFlags::Required | MaterialPinFlags::HideValue),
    MaterialInput("HeightRatioInput", MaterialPinType::Numeric, "HeightRatioInput",
                  MaterialPinDefault::Scalar(0.05f)),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition MathDefinitions[] = {
    MaterialNode("Material.DynamicBranch")
        .Pins(DynamicBranchPins)
        .Menu("Dynamic Branch", "Math", "Execution IfThenElse", "Dynamic Branch."),
    MaterialNode("Material.Abs")
        .Pins(AbsPins)
        .Menu("Abs", "Math", "Math",
              "Absolute value material expression for user-defined materials"),
    MaterialNode("Material.Add").Pins(AddPins).Menu("Add", "Math", "Math", "Add."),
    MaterialNode("Material.Subtract")
        .Pins(SubtractPins)
        .Menu("Subtract", "Math", "Math", "Subtract."),
    MaterialNode("Material.Multiply")
        .Pins(MultiplyPins)
        .Menu("Multiply", "Math", "Math", "Multiply."),
    MaterialNode("Material.Divide").Pins(DividePins).Menu("Divide", "Math", "Math", "Divide."),
    MaterialNode("Material.Min").Pins(MinPins).Menu("Min", "Math", "Math", "Min."),
    MaterialNode("Material.Max").Pins(MaxPins).Menu("Max", "Math", "Math", "Max."),
    MaterialNode("Material.Clamp")
        .Properties(&GeneratedType<ClampProperties>::Schema)
        .Pins(ClampPins)
        .Menu("Clamp", "Math", "Math", "Clamp."),
    MaterialNode("Material.Saturate")
        .Pins(SaturatePins)
        .Menu("Saturate", "Math", "Math", "Saturate."),
    MaterialNode("Material.OneMinus").Pins(OneMinusPins).Menu("1-x", "Math", "Math", "1-x."),
    MaterialNode("Material.Floor").Pins(FloorPins).Menu("Floor", "Math", "Math", "Floor."),
    MaterialNode("Material.Ceil").Pins(CeilPins).Menu("Ceil", "Math", "Math", "Ceil."),
    MaterialNode("Material.Round").Pins(RoundPins).Menu("Round", "Math", "Math", "Round."),
    MaterialNode("Material.Truncate")
        .Pins(TruncatePins)
        .Menu("Truncate", "Math", "Math", "Truncate."),
    MaterialNode("Material.Frac").Pins(FracPins).Menu("Frac", "Math", "Math", "Frac."),
    MaterialNode("Material.Fmod").Pins(FmodPins).Menu("Fmod", "Math", "Math", "Fmod."),
    MaterialNode("Material.Modulo").Pins(ModuloPins).Menu("Modulo", "Math", "% Math", "Modulo."),
    MaterialNode("Material.Sign").Pins(SignPins).Menu("Sign", "Math", "Math", "Sign."),
    MaterialNode("Material.Step").Pins(StepPins).Menu("Step", "Math", "Math Utility", "Step."),
    MaterialNode("Material.SmoothStep")
        .Pins(SmoothStepPins)
        .Menu("SmoothStep", "Math", "Math Utility", "SmoothStep."),
    MaterialNode("Material.Power").Pins(PowerPins).Menu("Power", "Math", "Math", "Power."),
    MaterialNode("Material.SquareRoot").Pins(SquareRootPins).Menu("Sqrt", "Math", "Math", "Sqrt."),
    MaterialNode("Material.Exponential").Pins(ExponentialPins).Menu("Exp", "Math", "Math", "Exp."),
    MaterialNode("Material.Exponential2")
        .Pins(Exponential2Pins)
        .Menu("Exp2", "Math", "Math", "Exp2."),
    MaterialNode("Material.Logarithm").Pins(LogarithmPins).Menu("Log", "Math", "Math", "Log."),
    MaterialNode("Material.Logarithm2").Pins(Logarithm2Pins).Menu("Log2", "Math", "Math", "Log2."),
    MaterialNode("Material.Logarithm10")
        .Pins(Logarithm10Pins)
        .Menu("Log10", "Math", "Math", "Log10."),
    MaterialNode("Material.Sine")
        .Properties(&GeneratedType<SineProperties>::Schema)
        .Pins(SinePins)
        .Menu("Sine", "Math", "Math", "Sine."),
    MaterialNode("Material.Cosine")
        .Properties(&GeneratedType<CosineProperties>::Schema)
        .Pins(CosinePins)
        .Menu("Cosine", "Math", "Math", "Cosine."),
    MaterialNode("Material.Tangent")
        .Properties(&GeneratedType<TangentProperties>::Schema)
        .Pins(TangentPins)
        .Menu("Tangent", "Math", "Math", "Tangent."),
    MaterialNode("Material.Arcsine").Pins(ArcsinePins).Menu("Arcsine", "Math", "Math", "Arcsine."),
    MaterialNode("Material.Arccosine")
        .Pins(ArccosinePins)
        .Menu("Arccosine", "Math", "Math", "Arccosine."),
    MaterialNode("Material.Arctangent")
        .Pins(ArctangentPins)
        .Menu("Arctangent", "Math", "Math", "Arctangent."),
    MaterialNode("Material.Arctangent2")
        .Pins(Arctangent2Pins)
        .Menu("Arctangent2", "Math", "Math", "Arctangent2."),
    MaterialNode("Material.ArcsineFast")
        .Pins(ArcsineFastPins)
        .Menu("ArcsineFast", "Math", "Math", "ArcsineFast."),
    MaterialNode("Material.ArccosineFast")
        .Pins(ArccosineFastPins)
        .Menu("ArccosineFast", "Math", "Math", "ArccosineFast."),
    MaterialNode("Material.ArctangentFast")
        .Pins(ArctangentFastPins)
        .Menu("ArctangentFast", "Math", "Math", "ArctangentFast."),
    MaterialNode("Material.Arctangent2Fast")
        .Pins(Arctangent2FastPins)
        .Menu("Arctangent2Fast", "Math", "Math", "Arctangent2Fast."),
    MaterialNode("Material.LinearInterpolate")
        .Pins(LinearInterpolatePins)
        .Menu("Lerp", "Math", "Math Utility", "Lerp."),
    MaterialNode("Material.InverseLinearInterpolate")
        .Properties(&GeneratedType<InverseLinearInterpolateProperties>::Schema)
        .Pins(InverseLinearInterpolatePins)
        .Menu("InvLerp", "Math", "Math Utility", "InvLerp."),
    MaterialNode("Material.DotProduct")
        .Pins(DotProductPins)
        .Menu("Dot", "Math", "Math Vectors", "Dot."),
    MaterialNode("Material.CrossProduct")
        .Pins(CrossProductPins)
        .Menu("Cross", "Math", "Math Vectors", "Cross."),
    MaterialNode("Material.Normalize")
        .Pins(NormalizePins)
        .Menu("Normalize", "Math", "Math Vectors", "Normalize."),
    MaterialNode("Material.Length").Pins(LengthPins).Menu("Length", "Math", "Utility", "Length."),
    MaterialNode("Material.Distance")
        .Pins(DistancePins)
        .Menu("Distance", "Math", "Utility", "Distance."),
    MaterialNode("Material.Append")
        .Pins(AppendPins)
        .Menu("Append", "Math", "Math Vectors AppendVector", "Append."),
    MaterialNode("Material.ComponentMask")
        .Properties(&GeneratedType<ComponentMaskProperties>::Schema)
        .Pins(ComponentMaskPins)
        .Menu("Component Mask", "Math", "Math Vectors", "Component Mask."),
    MaterialNode("Material.If")
        .Properties(&GeneratedType<IfProperties>::Schema)
        .Pins(IfPins)
        .Menu("If", "Math", "Math", "If."),
    MaterialNode("Material.Switch")
        .Properties(&GeneratedType<SwitchProperties>::Schema)
        .TextDefaults(SwitchTextDefaults)
        .Pins(SwitchPins)
        .DynamicPins(&SwitchDynamicPins)
        .Menu("Switch", "Math", "Math", "Switch."),
    MaterialNode("Material.FloatToUInt")
        .Properties(&GeneratedType<FloatToUIntProperties>::Schema)
        .Pins(FloatToUIntPins)
        .Menu("FloatToUInt", "Math", "Math", "FloatToUInt."),
    MaterialNode("Material.UIntToFloat")
        .Pins(UIntToFloatPins)
        .Menu("UIntToFloat", "Math", "Math", "UIntToFloat."),
    MaterialNode("Material.ConstantBiasScale")
        .Properties(&GeneratedType<ConstantBiasScaleProperties>::Schema)
        .Pins(ConstantBiasScalePins)
        .Menu("ConstantBiasScale", "Math", "Utility", "ConstantBiasScale."),
    MaterialNode("Material.Convert")
        .Properties(&GeneratedType<ConvertProperties>::Schema)
        .TextDefaults(ConvertTextDefaults)
        .DynamicPins(&ConvertDynamicPins)
        .Menu("Convert", "Math", "", "Convert."),
    MaterialNode("Material.Operator")
        .Properties(&GeneratedType<OperatorProperties>::Schema)
        .Pins(OperatorPins)
        .DynamicPins(&OperatorDynamicPins)
        .Menu("Operator", "Math", "operator Math", "Operator."),
    MaterialNode("Material.Ddx").Pins(DdxPins).Menu("Ddx", "Math", "Utility DDX", "Ddx."),
    MaterialNode("Material.Ddy").Pins(DdyPins).Menu("Ddy", "Math", "Utility DDY", "Ddy."),
    MaterialNode("Material.RotateAboutAxis")
        .Properties(&GeneratedType<RotateAboutAxisProperties>::Schema)
        .Pins(RotateAboutAxisPins)
        .Menu("RotateAboutAxis", "Math", "Utility", "RotateAboutAxis."),
    MaterialNode("Material.SphereMask")
        .Pins(SphereMaskPins)
        .Menu("SphereMask", "Math", "Utility", "SphereMask."),
    MaterialNode("Material.DeriveNormalZ")
        .Pins(DeriveNormalZPins)
        .Menu("DeriveNormalZ", "Math", "Vectors", "DeriveNormalZ."),
    MaterialNode("Material.Fresnel")
        .Pins(FresnelPins)
        .Menu("Fresnel", "Math", "Utility",
              "Allows the artists to quickly set up a Fresnel term. Returns: pow(1 - max(Normal "
              "dot Camera,0),Exponent)")
        .Features(ToMask(MaterialContextFeature::Vertex)),
    MaterialNode("Material.BumpOffset")
        .Properties(&GeneratedType<BumpOffsetProperties>::Schema)
        .Pins(BumpOffsetPins)
        .Menu("BumpOffset", "Math", "Utility", "BumpOffset.")
        .Features(ToMask(MaterialContextFeature::Vertex)),
};

} // namespace

Span<const MaterialNodeDefinition> MathNodeDefinitions() noexcept {
    return MathDefinitions;
}

} // namespace rocks::material::detail
