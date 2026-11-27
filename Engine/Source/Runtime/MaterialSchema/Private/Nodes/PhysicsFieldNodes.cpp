#include "MaterialBuiltinNodes.h"

#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsIntegerFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsIntegerFieldProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsScalarFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsScalarFieldProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsVectorFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/EvaluatePhysicsVectorFieldProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsIntegerFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsIntegerFieldProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsScalarFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsScalarFieldProperties.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsVectorFieldProperties.generated.h>
#include <MaterialSchema/Nodes/PhysicsField/SamplePhysicsVectorFieldProperties.h>
#include <MaterialSchema/MaterialGraphSchema.h>

// Physics Field 범주 노드 정의다(Docs/Design/MaterialSystem 19.6·19.13절). 핀 키와 순서는
// UE 입력 멤버·출력 목록과 같고, 기본값은 UE 입력 덮기 상수다. 처음 작성은 UE 5.8 헤더에서 뽑았고
// 이후는 손으로 유지한다(19.13.1절).
namespace rocks::material::detail {

namespace {

constexpr MaterialPinDefinition SamplePhysicsVectorFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SamplePhysicsScalarFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition SamplePhysicsIntegerFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition EvaluatePhysicsVectorFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition EvaluatePhysicsScalarFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialPinDefinition EvaluatePhysicsIntegerFieldPins[] = {
    MaterialInput("WorldPosition", MaterialPinType::Numeric, "WorldPosition", {},
                  MaterialPinFlags::HideValue),
    MaterialOutput("Result", MaterialPinType::Numeric, ""),
};

constexpr MaterialNodeDefinition PhysicsFieldDefinitions[] = {
    MaterialNode("Material.SamplePhysicsVectorField")
        .Properties(&GeneratedType<SamplePhysicsVectorFieldProperties>::Schema)
        .Pins(SamplePhysicsVectorFieldPins)
        .Menu("SamplePhysicsVectorField", "Physics Field", "Utility",
              "Material expresions to sample the global physics field")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
    MaterialNode("Material.SamplePhysicsScalarField")
        .Properties(&GeneratedType<SamplePhysicsScalarFieldProperties>::Schema)
        .Pins(SamplePhysicsScalarFieldPins)
        .Menu("SamplePhysicsScalarField", "Physics Field", "Utility", "SamplePhysicsScalarField.")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
    MaterialNode("Material.SamplePhysicsIntegerField")
        .Properties(&GeneratedType<SamplePhysicsIntegerFieldProperties>::Schema)
        .Pins(SamplePhysicsIntegerFieldPins)
        .Menu("SamplePhysicsIntegerField", "Physics Field", "Utility", "SamplePhysicsIntegerField.")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
    MaterialNode("Material.EvaluatePhysicsVectorField")
        .Properties(&GeneratedType<EvaluatePhysicsVectorFieldProperties>::Schema)
        .Pins(EvaluatePhysicsVectorFieldPins)
        .Menu("Evaluate Physics Vector Field", "Physics Field", "EvalPhysicsVectorField",
              "Evaluate Physics Vector Field.")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
    MaterialNode("Material.EvaluatePhysicsScalarField")
        .Properties(&GeneratedType<EvaluatePhysicsScalarFieldProperties>::Schema)
        .Pins(EvaluatePhysicsScalarFieldPins)
        .Menu("Evaluate Physics Scalar Field", "Physics Field", "EvalPhysicsScalarField",
              "Evaluate Physics Scalar Field.")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
    MaterialNode("Material.EvaluatePhysicsIntegerField")
        .Properties(&GeneratedType<EvaluatePhysicsIntegerFieldProperties>::Schema)
        .Pins(EvaluatePhysicsIntegerFieldPins)
        .Menu("Evaluate Physics Integer Field", "Physics Field", "EvalPhysicsIntegerField",
              "Evaluate Physics Integer Field.")
        .Features(ToMask(MaterialContextFeature::PhysicsField)),
};

} // namespace

Span<const MaterialNodeDefinition> PhysicsFieldNodeDefinitions() noexcept {
    return PhysicsFieldDefinitions;
}

} // namespace rocks::material::detail
