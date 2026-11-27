#pragma once

#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>

#include "MaterialGraphFixture.h"

// 생성 원문 골든(MaterialSourceWriterTests — Docs/Design/MaterialSystem 7.7절)과 MS2 완료 컴파일
// 모음(MaterialShaderCompileTests — 7.8절)이 함께 쓰는 Math 그래프다(두 시험이 같은 그래프를 본다).
namespace material_graph_fixture {

// 여러 연산 철자를 지나는 그래프다(7.7절):
// sin·cos·Append·ComponentMask·Power(Select)·If·DynamicBranch
// (Branch)·Clamp·ArcsineFast·FloatToUInt(Convert)·Operator 비교.
// wave는 float 스칼라 입력 노드(출력 "Result")다. 골든은 sin(0.5)(AddVarying), 컴파일 모음은 실행
// 값 probe를 넣는다(상수면 FXC가 분기 조건을 접어 [branch] 경고 X3554를 낸다 — 7.8절).
inline void BuildMathGraph(GraphFixture& fixture, Guid wave) noexcept {
    TranslatorEnv& env = fixture.env;
    CosineProperties flat;
    flat.period = 0.0f;
    const Guid cosine = fixture.Add("Material.Cosine", env.Save(flat));
    fixture.Connect(wave, "Result", cosine, "Input");

    // WPO = float3(x, x, cos(x)).
    const Guid pair = fixture.Add("Material.Append");
    fixture.Connect(wave, "Result", pair, "A");
    fixture.Connect(wave, "Result", pair, "B");
    const Guid triple = fixture.Add("Material.Append");
    fixture.Connect(pair, "Result", triple, "A");
    fixture.Connect(cosine, "Result", triple, "B");
    fixture.Connect(triple, "Result", OutputNodeId, "WorldPositionOffset");

    // OpacityMask = clamp(asinFast(pow(triple.z)), 0, 1).
    ComponentMaskProperties blue;
    blue.b = true;
    const Guid component = fixture.Add("Material.ComponentMask", env.Save(blue));
    fixture.Connect(triple, "Result", component, "Input");
    const Guid power = fixture.Add("Material.Power");
    fixture.Connect(component, "Result", power, "Base");
    const Guid arcsine = fixture.Add("Material.ArcsineFast");
    fixture.Connect(power, "Result", arcsine, "Input");
    const Guid clamp = fixture.Add("Material.Clamp");
    fixture.Connect(arcsine, "Result", clamp, "Input");
    fixture.Connect(clamp, "Result", OutputNodeId, "OpacityMask");

    // AO = if(x, 0 → 0.25, 0.5, 0.75).
    const Guid choice = fixture.Add("Material.If");
    fixture.Connect(wave, "Result", choice, "A");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", choice, "AGreaterThanB");
    fixture.Connect(AddConstant(fixture, 0.75f), "Result", choice, "ALessThanB");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", choice, "AEqualsB");
    fixture.Connect(choice, "Result", OutputNodeId, "AmbientOcclusion");

    // PDO = branch(x < 1, float(uint(trunc(x))), 2).
    OperatorProperties less(env.allocator);
    less.operatorKind = OperatorKind::LessThan;
    (void)less.dynamicInputs.TryPushBack(OperatorInput{});
    (void)less.dynamicInputs.TryPushBack(OperatorInput{});
    const Guid test = fixture.Add("Material.Operator", env.Save(less));
    fixture.Connect(wave, "Result", test, "A");
    const Guid toUInt = fixture.Add("Material.FloatToUInt");
    fixture.Connect(wave, "Result", toUInt, "Input");
    const Guid toFloat = fixture.Add("Material.UIntToFloat");
    fixture.Connect(toUInt, "Result", toFloat, "Input");
    const Guid branch = fixture.Add("Material.DynamicBranch");
    fixture.Connect(test, "Result", branch, "Condition");
    fixture.Connect(toFloat, "Result", branch, "True");
    fixture.Connect(AddConstant(fixture, 2.0f), "Result", branch, "False");
    fixture.Connect(branch, "Result", OutputNodeId, "PixelDepthOffset");
}

inline void BuildMathGraph(GraphFixture& fixture) noexcept {
    BuildMathGraph(fixture, AddVarying(fixture, 0.5f));
}

} // namespace material_graph_fixture
