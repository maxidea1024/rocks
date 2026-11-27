#pragma once

#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <MaterialCompiler/MaterialGraphSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialSchema/MaterialGuidPinKey.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallInput.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallOutput.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionCallProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionInputProperties.h>
#include <MaterialSchema/Nodes/Functions/FunctionOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/FunctionOutputProperties.h>
#include <MaterialSchema/Nodes/Functions/LayerOutputProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/LayerOutputProperties.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.h>

#include "MaterialGraphFixture.h"
#include "MaterialMemorySources.h"

// 함수·레이어 번역 시험의 픽스처다(Docs/Design/MaterialSystem 6.12절 MS2e2·MS2e3): 함수 문서,
// 메모리 provider, 원본 공급 번역, 호출·입출력 노드 도우미, 결과·진단 조회.
namespace material_function_fixture {

using namespace rocks;
using namespace rocks::material;
using namespace material_graph_fixture;

// 함수 문서 하나다(도메인 MaterialFunction, 출력 노드 없는 주 그래프).
struct FunctionFixture {
    FunctionFixture(TranslatorEnv& environment, Guid asset, StringView name) noexcept
        : graph(environment) {
        graph.document.domain = environment.Intern("MaterialFunction");
        graph.Main().nodes.Clear();
        snapshot.asset = asset;
        snapshot.name = name;
        snapshot.document = &graph.document;
    }

    GraphFixture graph;
    MaterialGraphSnapshot snapshot;
};

inline Result<void> TranslateWithSources(GraphFixture& fixture,
                                         const MaterialSourceProvider* sources,
                                         MaterialTranslation& out,
                                         const MaterialTraitsDesc& traits = Traits()) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   traits,           TestAssetId,         {}};
    input.sources = sources;
    return TranslateMaterial(fixture.env.allocator, input, out);
}

// "P_<id>" 동적 핀 키 원문이다.
inline StringView PinKey(TranslatorEnv& env, Guid id) noexcept {
    auto key = MaterialGuidPinKey(env.names, "P_", id);
    if (!key) {
        return StringView();
    }

    auto lease = env.names.Acquire(key.Value());
    return lease ? lease.Value().View() : StringView();
}

inline Guid AddFunctionInput(GraphFixture& fixture, Guid id, FunctionInputType type,
                             bool usePreview = false, float preview = 0.0f,
                             StringView name = "In") noexcept {
    FunctionInputProperties properties(fixture.env.allocator);
    properties.inputName = fixture.env.Intern(name);
    properties.inputType = type;
    properties.usePreviewValueAsDefault = usePreview;
    properties.previewValue = Vec4{preview, preview, preview, preview};
    properties.id = id;
    return fixture.Add("Material.FunctionInput", fixture.env.Save(properties));
}

inline Guid AddFunctionOutput(GraphFixture& fixture, Guid id, StringView name = "Result") noexcept {
    FunctionOutputProperties properties(fixture.env.allocator);
    properties.outputName = fixture.env.Intern(name);
    properties.id = id;
    return fixture.Add("Material.FunctionOutput", fixture.env.Save(properties));
}

// 함수 호출 노드다(입력 inputs·출력 outputs id — 이름은 시험에 쓰지 않는다).
inline Guid AddCall(GraphFixture& fixture, Guid asset, Span<const Guid> inputs,
                    Span<const Guid> outputs) noexcept {
    FunctionCallProperties properties(fixture.env.allocator);
    properties.materialFunction.assetId = AssetId{asset};
    bool ready = true;
    for (Guid id : inputs) {
        FunctionCallInput input;
        input.id = id;
        input.inputName = fixture.env.Intern("In");
        input.inputType = FunctionInputType::Scalar;
        ready = properties.inputs.TryPushBack(input).IsOk() && ready;
    }

    for (Guid id : outputs) {
        FunctionCallOutput output;
        output.id = id;
        output.outputName = fixture.env.Intern("Result");
        ready = properties.outputs.TryPushBack(output).IsOk() && ready;
    }

    fixture.ready = ready && fixture.ready;
    return fixture.Add("Material.FunctionCall", fixture.env.Save(properties));
}

inline Guid AddScalarParameter(GraphFixture& fixture, Guid id, StringView name,
                               bool shared) noexcept {
    ScalarParameterProperties properties;
    properties.parameterName = fixture.env.Intern(name);
    properties.parameterId = id;
    properties.defaultValue = 0.5f;
    properties.shared = shared;
    return fixture.Add("Material.ScalarParameter", fixture.env.Save(properties));
}

// Unlit 표면 emissive로 값을 내보낸다.
inline void EmitEmissive(GraphFixture& fixture, Guid node, StringView pin) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(node, pin, unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
}

// Surface Emissive 결과의 첫 성분 상수다(아니면 -1).
inline float EmissiveConstant(const MaterialTranslation& out) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == IrEntry::Surface && result.slot == StringView("Emissive")) {
            const IrValue& value = ValueOf(out, result.value);
            return value.op == IrOp::Constant ? FloatOf(value, 0) : -1.0f;
        }
    }

    return -1.0f;
}

inline bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

// 진단 message가 node 위치(핀 없음)에 있는가.
inline bool HasMessageAt(const MaterialTranslation& out, Guid node, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.node == node && diagnostic.message.View() == message) {
            return diagnostic.pin == Name() && diagnostic.graph == MainGraphId;
        }
    }

    return false;
}

// LayerOutput 노드다(레이어 함수 출력 — 6.12절 MS2e3).
inline Guid AddLayerOutput(GraphFixture& fixture, Guid id, StringView name = "Result") noexcept {
    LayerOutputProperties properties(fixture.env.allocator);
    properties.outputName = fixture.env.Intern(name);
    properties.id = id;
    return fixture.Add("Material.LayerOutput", fixture.env.Save(properties));
}

} // namespace material_function_fixture
