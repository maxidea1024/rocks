#include <Base/InitializerList.h>
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
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.generated.h>
#include <MaterialSchema/Nodes/Functions/MaterialSampleProperties.h>

#include "MaterialGraphFixture.h"

// MS2e2 시험이다(Docs/Design/MaterialSystem 6.12절 MS2e2): 함수 호출 펼치기(값 전달·입력 형 맞춤),
// FunctionInput 미리보기 기본값·필수 입력, 함수 출력 연결 없음, 호출 오류(참조 없음·에셋 없음·입력
// 색인·재귀·깊이 상한), 함수 안 진단 위치·접두, 쓰지 않는 출력은 번역하지 않음, 파라미터
// scope(MS-T9 반복 호출은 독립·shared는 하나, scope별 V8), MaterialSample, 의존 목록을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_functions_tests_file {

using namespace material_graph_fixture;

namespace {

constexpr uint64 FunctionAssetHigh = 0x4d4146554e435431ull;
constexpr Guid FunctionAssetF{FunctionAssetHigh, 1};
constexpr Guid FunctionAssetG{FunctionAssetHigh, 2};
constexpr Guid SampleAssetM{FunctionAssetHigh, 3};
constexpr Guid FunctionInputIdIn{0x4d41464e494e5054ull, 1};
constexpr Guid FunctionOutputIdOut{0x4d41464e4f555450ull, 1};
constexpr Guid FunctionOutputIdSecond{0x4d41464e4f555450ull, 2};
constexpr Guid FunctionParameterId{0x4d41464e50415241ull, 1};
// 깊이 상한 시험의 함수 수다(주 그래프 호출 깊이 1 — 마지막 호출이 상한 + 1).
constexpr usize FunctionChainLength = 65;

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

// 메모리 provider다(시험 — snapshot 포인터 목록).
class MemorySources final : public MaterialSourceProvider {
public:
    explicit MemorySources(Allocator& allocator) noexcept
        : functions_(allocator), materials_(allocator) {
    }

    bool AddFunction(const MaterialGraphSnapshot& snapshot) noexcept {
        return functions_.TryPushBack(&snapshot).IsOk();
    }

    bool AddMaterial(const MaterialGraphSnapshot& snapshot) noexcept {
        return materials_.TryPushBack(&snapshot).IsOk();
    }

    const MaterialGraphSnapshot* FindFunction(Guid asset) const noexcept override {
        return Find(functions_, asset);
    }

    const MaterialGraphSnapshot* FindMaterial(Guid asset) const noexcept override {
        return Find(materials_, asset);
    }

private:
    static const MaterialGraphSnapshot* Find(const Array<const MaterialGraphSnapshot*>& list,
                                             Guid asset) noexcept {
        for (const MaterialGraphSnapshot* snapshot : list) {
            if (snapshot->asset == asset) {
                return snapshot;
            }
        }

        return nullptr;
    }

    Array<const MaterialGraphSnapshot*> functions_;
    Array<const MaterialGraphSnapshot*> materials_;
};

Result<void> TranslateWithSources(GraphFixture& fixture, const MaterialSourceProvider* sources,
                                  MaterialTranslation& out,
                                  const MaterialTraitsDesc& traits = Traits()) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   traits,           TestAssetId,         {}};
    input.sources = sources;
    return TranslateMaterial(fixture.env.allocator, input, out);
}

// "P_<id>" 동적 핀 키 원문이다.
StringView PinKey(TranslatorEnv& env, Guid id) noexcept {
    auto key = MaterialGuidPinKey(env.names, "P_", id);
    if (!key) {
        return StringView();
    }

    auto lease = env.names.Acquire(key.Value());
    return lease ? lease.Value().View() : StringView();
}

Guid AddFunctionInput(GraphFixture& fixture, Guid id, FunctionInputType type,
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

Guid AddFunctionOutput(GraphFixture& fixture, Guid id, StringView name = "Result") noexcept {
    FunctionOutputProperties properties(fixture.env.allocator);
    properties.outputName = fixture.env.Intern(name);
    properties.id = id;
    return fixture.Add("Material.FunctionOutput", fixture.env.Save(properties));
}

// 함수 호출 노드다(입력 inputs·출력 outputs id — 이름은 시험에 쓰지 않는다).
Guid AddCall(GraphFixture& fixture, Guid asset, Span<const Guid> inputs,
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

Guid AddScalarParameter(GraphFixture& fixture, Guid id, StringView name, bool shared) noexcept {
    ScalarParameterProperties properties;
    properties.parameterName = fixture.env.Intern(name);
    properties.parameterId = id;
    properties.defaultValue = 0.5f;
    properties.shared = shared;
    return fixture.Add("Material.ScalarParameter", fixture.env.Save(properties));
}

// Unlit 표면 emissive로 값을 내보낸다.
void EmitEmissive(GraphFixture& fixture, Guid node, StringView pin) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(node, pin, unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
}

// Surface Emissive 결과의 첫 성분 상수다(아니면 -1).
float EmissiveConstant(const MaterialTranslation& out) noexcept {
    for (const IrResult& result : out.module.Results()) {
        if (result.entry == IrEntry::Surface && result.slot == StringView("Emissive")) {
            const IrValue& value = ValueOf(out, result.value);
            return value.op == IrOp::Constant ? FloatOf(value, 0) : -1.0f;
        }
    }

    return -1.0f;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

// 진단 message가 node 위치(핀 없음)에 있는가.
bool HasMessageAt(const MaterialTranslation& out, Guid node, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.node == node && diagnostic.message.View() == message) {
            return diagnostic.pin == Name() && diagnostic.graph == MainGraphId;
        }
    }

    return false;
}

// 함수 F: In(Scalar) → Out(id FunctionOutputIdOut).
void BuildPassThrough(FunctionFixture& function, bool usePreview = false,
                      float preview = 0.0f) noexcept {
    const Guid input = AddFunctionInput(function.graph, FunctionInputIdIn,
                                        FunctionInputType::Scalar, usePreview, preview);
    const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
    function.graph.Connect(input, "Result", output, "A");
}

} // namespace

// 호출 쪽 값이 함수 입력 → 출력으로 지나가고, 읽은 함수가 의존 목록에 남는다.
TEST(MaterialFunctions_CallPassesValue) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, FunctionAssetF, "F");
    BuildPassThrough(function);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    GraphFixture main(env);
    const Guid inputs[] = {FunctionInputIdIn};
    const Guid outputs[] = {FunctionOutputIdOut};
    const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
    main.Connect(AddConstant(main, 0.25f), "Result", call, PinKey(env, FunctionInputIdIn));
    EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
    REQUIRE(main.ready);
    REQUIRE(function.graph.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    CHECK(EmissiveConstant(out) == 0.25f);
    REQUIRE(out.dependencies.Size() == 1);
    CHECK(out.dependencies[0].role == MaterialDependencyRole::Function);
    CHECK(out.dependencies[0].asset == FunctionAssetF);
}

// 호출 쪽 연결이 없으면: usePreviewValueAsDefault면 previewValue·Preview 연결, 아니면 필수 입력
// 오류다(가장 바깥 호출 노드 위치, "(Function F) " 접두 — FunctionInput은 노드 접두가 없다).
TEST(MaterialFunctions_InputDefaults) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const Guid inputs[] = {FunctionInputIdIn};
    const Guid outputs[] = {FunctionOutputIdOut};
    {
        FunctionFixture function(env, FunctionAssetF, "F");
        BuildPassThrough(function, true, 0.5f);
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        REQUIRE(out.succeeded);
        CHECK(EmissiveConstant(out) == 0.5f);
    }

    {
        FunctionFixture function(env, FunctionAssetF, "F");
        BuildPassThrough(function, true, 0.5f);
        nodegraph::GraphNode& input = function.graph.Main().nodes[0];
        function.graph.Connect(AddConstant(function.graph, 0.75f), "Result", input.id, "Preview");
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        REQUIRE(out.succeeded);
        CHECK(EmissiveConstant(out) == 0.75f);
    }

    {
        FunctionFixture function(env, FunctionAssetF, "F");
        BuildPassThrough(function);
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, call));
        CHECK(HasMessageAt(out, call, "(Function F) Missing function input 'In'"));
    }

    {
        // 텍스처 입력은 미리보기 상수가 없다(UE CompilePreviewValue는 텍스처 객체 — Rocks는 Preview
        // 연결이 필요하다).
        FunctionFixture function(env, FunctionAssetF, "F");
        const Guid input =
            AddFunctionInput(function.graph, FunctionInputIdIn, FunctionInputType::Texture2D, true);
        const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
        function.graph.Connect(input, "Result", output, "A");
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        const Guid sample = main.Add("Material.TextureSample");
        main.Connect(call, PinKey(env, FunctionOutputIdOut), sample, "TextureObject");
        EmitEmissive(main, sample, "RGB");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(!out.succeeded);
        CHECK(HasMessageAt(out, call,
                           "(Function F) Missing Preview connection for function input 'In'"));
    }
}

// 호출 쪽 값은 입력 형으로 맞춘다(넓은 벡터 → 앞 성분, 좁은 벡터는 V1).
TEST(MaterialFunctions_InputCast) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, FunctionAssetF, "F");
    const Guid input =
        AddFunctionInput(function.graph, FunctionInputIdIn, FunctionInputType::Vector3);
    const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
    function.graph.Connect(input, "Result", output, "A");
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    const Guid inputs[] = {FunctionInputIdIn};
    const Guid outputs[] = {FunctionOutputIdOut};

    GraphFixture main(env);
    const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
    Constant2Properties narrow;
    narrow.r = 0.1f;
    narrow.g = 0.2f;
    main.Connect(main.Add("Material.Constant2", env.Save(narrow)), "Result", call,
                 PinKey(env, FunctionInputIdIn));
    EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, call));
}

// 함수 출력 연결 없음·참조 없음·에셋 없음·호출 사본의 입력 색인 오류는 호출 노드 위치다.
TEST(MaterialFunctions_CallErrors) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const Guid inputs[] = {FunctionInputIdIn};
    const Guid outputs[] = {FunctionOutputIdOut};
    {
        FunctionFixture function(env, FunctionAssetF, "F");
        AddFunctionInput(function.graph, FunctionInputIdIn, FunctionInputType::Scalar);
        AddFunctionOutput(function.graph, FunctionOutputIdOut);
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, call));
        CHECK(HasMessageAt(out, call, "Missing function output connection 'Result'"));
    }

    {
        GraphFixture main(env);
        const Guid call = AddCall(main, Guid{}, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, nullptr, out));
        CHECK(HasMessageAt(out, call, "Missing Material Function"));
    }

    {
        GraphFixture main(env);
        const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, nullptr, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, call));
        CHECK(HasMessageAt(out, call, "Material function asset is not available"));
        CHECK(out.dependencies.IsEmpty());
    }

    {
        FunctionFixture function(env, FunctionAssetF, "F");
        BuildPassThrough(function);
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid stale[] = {FunctionInputIdIn, Guid{0x5354414c45ull, 9}};
        const Guid call = AddCall(main, FunctionAssetF, stale, outputs);
        EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, call));
        CHECK(HasMessageAt(out, call, "Function (F) call input with index 1 is unset."));
    }

    {
        // 쓰지 않는 호출 노드의 오류는 남지 않는다(UE도 쓰는 식만 compile한다).
        GraphFixture main(env);
        AddCall(main, FunctionAssetF, inputs, outputs);
        EmitEmissive(main, AddConstant(main, 0.5f), "Result");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, nullptr, out));
        CHECK(out.succeeded);
    }
}

// 함수 안 노드의 진단은 가장 바깥 호출 노드에 "(Function F|G) (Node <종류>) " 접두로 낸다. 쓰지
// 않는 출력의 오류는 남지 않는다.
TEST(MaterialFunctions_NestedDiagnostics) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture inner(env, FunctionAssetG, "G");
    const Guid broken = inner.graph.Add("Material.Abs");
    const Guid used = AddFunctionOutput(inner.graph, FunctionOutputIdOut);
    inner.graph.Connect(broken, "Result", used, "A");
    const Guid unused = AddFunctionOutput(inner.graph, FunctionOutputIdSecond, "Second");
    inner.graph.Connect(inner.graph.Add("Material.Sine"), "Result", unused, "A");

    FunctionFixture outer(env, FunctionAssetF, "F");
    const Guid innerOutputs[] = {FunctionOutputIdOut, FunctionOutputIdSecond};
    const Guid innerCall = AddCall(outer.graph, FunctionAssetG, {}, innerOutputs);
    const Guid outerOutput = AddFunctionOutput(outer.graph, FunctionOutputIdOut);
    outer.graph.Connect(innerCall, PinKey(env, FunctionOutputIdOut), outerOutput, "A");

    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(outer.snapshot));
    REQUIRE(sources.AddFunction(inner.snapshot));
    GraphFixture main(env);
    const Guid outputs[] = {FunctionOutputIdOut};
    const Guid call = AddCall(main, FunctionAssetF, {}, outputs);
    EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, call));
    CHECK(HasMessageAt(out, call, "(Function F|G) (Node Abs) Missing input"));
    REQUIRE(out.dependencies.Size() == 2);
    CHECK(out.dependencies[0].asset == FunctionAssetF);
    CHECK(out.dependencies[1].asset == FunctionAssetG);
}

// 호출 경로에 같은 함수가 다시 나오면 V3다(그 호출 노드 — 함수 안이면 바깥 호출 위치).
TEST(MaterialFunctions_Recursion) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, FunctionAssetF, "F");
    const Guid outputs[] = {FunctionOutputIdOut};
    const Guid self = AddCall(function.graph, FunctionAssetF, {}, outputs);
    const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
    function.graph.Connect(self, PinKey(env, FunctionOutputIdOut), output, "A");
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    GraphFixture main(env);
    const Guid call = AddCall(main, FunctionAssetF, {}, outputs);
    EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::FunctionCycle, call));
    CHECK(HasMessageAt(out, call, "(Function F) Material function call is recursive: F"));
}

// 호출 깊이가 MaxFunctionCallDepth(64)를 넘으면 V9다.
TEST(MaterialFunctions_DepthLimit) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    // 함수 k는 함수 k + 1을 부른다(마지막은 상수).
    Array<FunctionFixture*> chain(env.allocator);
    REQUIRE(chain.TryReserve(FunctionChainLength).IsOk());
    MemorySources sources(env.allocator);
    const Guid outputs[] = {FunctionOutputIdOut};
    bool ready = true;
    for (usize at = 0; at < FunctionChainLength; ++at) {
        FunctionFixture* function =
            new FunctionFixture(env, Guid{FunctionAssetHigh, 100 + at}, "C");
        const Guid output = AddFunctionOutput(function->graph, FunctionOutputIdOut);
        const Guid value =
            at + 1 < FunctionChainLength
                ? AddCall(function->graph, Guid{FunctionAssetHigh, 101 + at}, {}, outputs)
                : AddConstant(function->graph, 0.5f);
        function->graph.Connect(value,
                                at + 1 < FunctionChainLength ? PinKey(env, FunctionOutputIdOut)
                                                             : StringView("Result"),
                                output, "A");
        ready = chain.TryPushBack(function).IsOk() && sources.AddFunction(function->snapshot) &&
                function->graph.ready && ready;
    }

    GraphFixture main(env);
    const Guid call = AddCall(main, Guid{FunctionAssetHigh, 100}, {}, outputs);
    EmitEmissive(main, call, PinKey(env, FunctionOutputIdOut));
    MaterialTranslation out(env.allocator);
    const bool translated = ready && TranslateWithSources(main, &sources, out).IsOk();
    for (FunctionFixture* function : chain) {
        delete function;
    }

    REQUIRE(translated);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::BudgetExceeded, call));
    CHECK(out.dependencies.Size() == FunctionChainLength - 1);
}

// MS-T9: 같은 함수를 두 번 부르면 공유가 아닌 파라미터는 호출마다 따로, shared는 하나다. V8은
// scope별이다.
TEST(MaterialFunctions_ParameterScopes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const Guid outputs[] = {FunctionOutputIdOut};
    for (const bool shared : {false, true}) {
        FunctionFixture function(env, FunctionAssetF, "F");
        const Guid parameter = AddScalarParameter(function.graph, FunctionParameterId, "P", shared);
        const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
        function.graph.Connect(parameter, "Result", output, "A");
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        const Guid first = AddCall(main, FunctionAssetF, {}, outputs);
        const Guid second = AddCall(main, FunctionAssetF, {}, outputs);
        const Guid add = main.Add("Material.Add");
        main.Connect(first, PinKey(env, FunctionOutputIdOut), add, "A");
        main.Connect(second, PinKey(env, FunctionOutputIdOut), add, "B");
        EmitEmissive(main, add, "Result");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        REQUIRE(out.succeeded);
        CHECK(out.module.Parameters().Size() == (shared ? 1u : 2u));
        for (const IrParameter& item : out.module.Parameters()) {
            CHECK(!(item.id == FunctionParameterId));
        }

        // 같은 함수의 정체는 번역마다 같다(쿠킹 키·인스턴스 override).
        MaterialTranslation again(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, again));
        REQUIRE(again.module.Parameters().Size() == out.module.Parameters().Size());
        CHECK(again.module.Parameters()[0].id == out.module.Parameters()[0].id);
    }

    // 주 그래프 VectorParameter "P"와 함수 안 ScalarParameter "P": 공유가 아니면 다른 scope라 V8이
    // 아니고, shared면 전역 scope라 V8이다.
    for (const bool shared : {false, true}) {
        FunctionFixture function(env, FunctionAssetF, "F");
        const Guid parameter = AddScalarParameter(function.graph, FunctionParameterId, "P", shared);
        const Guid output = AddFunctionOutput(function.graph, FunctionOutputIdOut);
        function.graph.Connect(parameter, "Result", output, "A");
        MemorySources sources(env.allocator);
        REQUIRE(sources.AddFunction(function.snapshot));
        GraphFixture main(env);
        VectorParameterProperties vector(env.allocator);
        vector.parameterName = env.Intern("P");
        vector.parameterId = Guid{0x56454354ull, 1};
        const Guid tint = main.Add("Material.VectorParameter", env.Save(vector));
        const Guid call = AddCall(main, FunctionAssetF, {}, outputs);
        const Guid add = main.Add("Material.Add");
        main.Connect(call, PinKey(env, FunctionOutputIdOut), add, "A");
        main.Connect(tint, "RGB", add, "B");
        EmitEmissive(main, add, "Result");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasError(out, MaterialDiagnosticCode::ParameterConflict) == shared);
    }
}

// MaterialSample은 참조 머티리얼 출력 노드 Surface의 그래프를 펼친다(그 출력 노드·custom 출력은 이
// 머티리얼의 출력이 아니다). 자기 참조는 V3다.
TEST(MaterialFunctions_MaterialSample) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture referenced(env);
    EmitEmissive(referenced, AddConstant(referenced, 0.125f), "Result");
    MaterialGraphSnapshot snapshot;
    snapshot.asset = SampleAssetM;
    snapshot.name = "M";
    snapshot.document = &referenced.document;
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddMaterial(snapshot));

    GraphFixture main(env);
    MaterialSampleProperties properties;
    properties.materialReference.assetId = AssetId{SampleAssetM};
    const Guid sample = main.Add("Material.MaterialSample", env.Save(properties));
    main.Connect(sample, "Front Material", OutputNodeId, "Surface");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    CHECK(EmissiveConstant(out) == 0.125f);
    REQUIRE(out.dependencies.Size() == 1);
    CHECK(out.dependencies[0].role == MaterialDependencyRole::Material);

    GraphFixture self(env);
    MaterialSampleProperties selfProperties;
    selfProperties.materialReference.assetId = AssetId{TestAssetId};
    const Guid selfSample = self.Add("Material.MaterialSample", env.Save(selfProperties));
    self.Connect(selfSample, "Front Material", OutputNodeId, "Surface");
    MaterialTranslation selfOut(env.allocator);
    REQUIRE(TranslateWithSources(self, &sources, selfOut));
    CHECK(HasOnlyErrorAt(selfOut, MaterialDiagnosticCode::FunctionCycle, selfSample));
    CHECK(HasMessage(
        selfOut, "Material Sample cannot reference this material or one of it's child instances."));
}

// 호출 입력이 그 호출의 출력에 기대면 순환(V2)이다.
TEST(MaterialFunctions_CallCycle) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, FunctionAssetF, "F");
    BuildPassThrough(function);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    GraphFixture main(env);
    const Guid inputs[] = {FunctionInputIdIn};
    const Guid outputs[] = {FunctionOutputIdOut};
    const Guid call = AddCall(main, FunctionAssetF, inputs, outputs);
    const Guid sine = main.Add("Material.Sine");
    main.Connect(call, PinKey(env, FunctionOutputIdOut), sine, "Input");
    main.Connect(sine, "Result", call, PinKey(env, FunctionInputIdIn));
    EmitEmissive(main, sine, "Result");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(!out.succeeded);
    CHECK(HasError(out, MaterialDiagnosticCode::Cycle));
}

} // namespace material_functions_tests_file
