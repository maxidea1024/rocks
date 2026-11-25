#pragma once

#include "MaterialTranslatorEnv.h"
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <MaterialCompiler/MaterialDiagnosticCode.h>
#include <MaterialCompiler/MaterialTranslator.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialPinValue.generated.h>
#include <MaterialSchema/MaterialPinValue.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.generated.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Math/SineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SineProperties.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.generated.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/StructRecord.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 머티리얼 번역·생성 시험의 그래프 픽스처다(Docs/Design/MaterialSystem 6.8·7.7절). MaterialSchema
// 타입과 엔진 노드를 등록한 환경(TranslatorEnv), 주 그래프와 출력 노드가 있는 문서(GraphFixture),
// 번역·결과 조회 도우미를 준다. 노드 속성과 핀 사용자 값은 리플렉션 codec으로 Record에 쓴다(실제
// wire).
namespace material_graph_fixture {

using namespace rocks;
using namespace rocks::material;

inline constexpr Guid MainGraphId{0x4d41544752415048ull, 1};
inline constexpr Guid OutputNodeId{0x4d41544f55545055ull, 1};
inline constexpr Guid TestAssetId{0x4d41544153534554ull, 7};
// 노드 id 상위 값이다(하위는 1부터 차례).
inline constexpr uint64 NodeIdHigh = 0x4d41544e4f444531ull;
// 시험용 2π / 1(Sine 주기 1의 계수 — UE 2.0f * (float)UE_PI / Period)다.
inline constexpr float TwoPiFloat = 2.0f * static_cast<float>(3.14159265358979323846);

// 주 그래프 하나와 출력 노드 하나인 문서다.
struct GraphFixture {
    explicit GraphFixture(TranslatorEnv& environment) noexcept
        : env(environment), document(environment.allocator) {
        document.domain = env.Intern("Material");
        document.mainGraph = MainGraphId;
        nodegraph::Graph graph(env.allocator);
        graph.id = MainGraphId;
        ready = document.graphs.TryPushBack(Move(graph)).IsOk() &&
                AddWithId("Material.Output", OutputNodeId, Array<uint8>(env.allocator));
    }

    nodegraph::Graph& Main() noexcept {
        return document.graphs[0];
    }

    bool AddWithId(StringView type, Guid id, const Array<uint8>& properties) noexcept {
        nodegraph::GraphNode node(env.allocator);
        node.id = id;
        node.type = env.Intern(type);
        node.typeSchemaVersion = 1;
        if (!node.properties.bytes.TryAppend(properties.View())) {
            return false;
        }

        return Main().nodes.TryPushBack(Move(node)).IsOk();
    }

    Guid Add(StringView type) noexcept {
        return Add(type, Array<uint8>(env.allocator));
    }

    Guid Add(StringView type, const Array<uint8>& properties) noexcept {
        const Guid id{NodeIdHigh, ++nextId};
        ready = AddWithId(type, id, properties) && ready;
        return id;
    }

    nodegraph::GraphNode* Find(Guid id) noexcept {
        for (nodegraph::GraphNode& node : Main().nodes) {
            if (node.id == id) {
                return &node;
            }
        }

        return nullptr;
    }

    void Connect(Guid from, StringView fromPin, Guid to, StringView toPin) noexcept {
        nodegraph::GraphEdge edge;
        edge.fromNode = from;
        edge.fromPin = env.Intern(fromPin);
        edge.toNode = to;
        edge.toPin = env.Intern(toPin);
        ready = Main().edges.TryPushBack(edge).IsOk() && ready;
    }

    void SetValue(Guid node, StringView pin, const MaterialPinValue& value) noexcept {
        nodegraph::GraphNode* target = Find(node);
        nodegraph::PinValueEntry entry(env.allocator);
        entry.pin = env.Intern(pin);
        const Array<uint8> bytes = env.Save(value);
        ready = target != nullptr && !bytes.IsEmpty() &&
                entry.value.bytes.TryAppend(bytes.View()).IsOk() &&
                target->inputDefaults.TryPushBack(Move(entry)).IsOk() && ready;
    }

    TranslatorEnv& env;
    nodegraph::GraphDocument document;
    uint64 nextId = 0;
    bool ready = false;
};

inline MaterialTraitsDesc Traits(BlendMode blend = BlendMode::Opaque,
                                 MaterialDomain domain = MaterialDomain::Surface) noexcept {
    MaterialTraitsDesc traits;
    traits.domain = domain;
    traits.blendMode = blend;
    return traits;
}

inline Result<void> Translate(GraphFixture& fixture, const MaterialTraitsDesc& traits,
                              MaterialTranslation& out) noexcept {
    const MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                         traits, TestAssetId};
    return TranslateMaterial(fixture.env.allocator, input, out);
}

inline const IrResult* FindResult(const MaterialTranslation& out, StringView slot) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.slot == slot) {
            return &result;
        }
    }

    return nullptr;
}

inline const IrValue& ValueOf(const MaterialTranslation& out, IrValueId id) noexcept {
    return out.module.Values()[id];
}

inline float FloatOf(const IrValue& value, usize component) noexcept {
    const uint32 bits = static_cast<uint32>(value.immediate[component]);
    float result = 0.0f;
    platform::MemoryCopy(&result, &bits, sizeof(result));
    return result;
}

// slot 결과가 성분 값 expected인 Float 상수인가.
inline bool IsFloatResult(const MaterialTranslation& out, StringView slot,
                          Span<const float> expected) noexcept {
    const IrResult* result = FindResult(out, slot);
    if (result == nullptr) {
        return false;
    }

    const IrValue& value = ValueOf(out, result->value);
    if (value.op != IrOp::Constant || value.type != IrFloat(static_cast<uint8>(expected.Size()))) {
        return false;
    }

    for (usize index = 0; index < expected.Size(); ++index) {
        if (FloatOf(value, index) != expected[index]) {
            return false;
        }
    }

    return true;
}

inline usize CountDiagnostics(const MaterialTranslation& out, MaterialDiagnosticCode code,
                              nodegraph::GraphDiagnosticSeverity severity) noexcept {
    usize count = 0;
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.code == ToDiagnosticCode(code) && diagnostic.severity == severity) {
            ++count;
        }
    }

    return count;
}

inline bool HasError(const MaterialTranslation& out, MaterialDiagnosticCode code) noexcept {
    return CountDiagnostics(out, code, nodegraph::GraphDiagnosticSeverity::Error) > 0;
}

// node 위치의 그 코드 진단이 있는가.
inline bool HasErrorAt(const MaterialTranslation& out, MaterialDiagnosticCode code,
                       Guid node) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.code == ToDiagnosticCode(code) && diagnostic.node == node &&
            diagnostic.severity == nodegraph::GraphDiagnosticSeverity::Error) {
            return diagnostic.asset == TestAssetId && diagnostic.graph == MainGraphId;
        }
    }

    return false;
}

// 오류 진단 수다.
inline usize CountErrors(const MaterialTranslation& out) noexcept {
    usize count = 0;
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        count += diagnostic.severity == nodegraph::GraphDiagnosticSeverity::Error ? 1 : 0;
    }

    return count;
}

// 오류가 정확히 하나이고 그것이 node 위치의 code인가(오류가 아래로 번지지 않는다 — 6.8절).
inline bool HasOnlyErrorAt(const MaterialTranslation& out, MaterialDiagnosticCode code,
                           Guid node) noexcept {
    return CountErrors(out) == 1 && HasErrorAt(out, code, node);
}

inline Guid AddConstant(GraphFixture& fixture, float value) noexcept {
    ConstantProperties properties;
    properties.r = value;
    return fixture.Add("Material.Constant", fixture.env.Save(properties));
}

// 접히지 않는 float 스칼라 값(sin(value))의 Sine 노드다.
inline Guid AddVarying(GraphFixture& fixture, float value) noexcept {
    const Guid constant = AddConstant(fixture, value);
    SineProperties properties;
    properties.period = 0.0f;
    const Guid sine = fixture.Add("Material.Sine", fixture.env.Save(properties));
    fixture.Connect(constant, "Result", sine, "Input");
    return sine;
}

inline MaterialPinValue PinValue(float x, uint8 count) noexcept {
    MaterialPinValue value;
    value.x = x;
    value.y = x;
    value.z = x;
    value.w = x;
    value.componentCount = count;
    return value;
}

} // namespace material_graph_fixture
