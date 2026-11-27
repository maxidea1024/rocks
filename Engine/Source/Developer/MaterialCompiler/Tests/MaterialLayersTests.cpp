#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/AttributeLayersProperties.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackDefaults.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackInput.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackInput.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackLayer.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.generated.h>
#include <MaterialSchema/Nodes/MaterialAttributes/LayerStackProperties.h>

#include "MaterialFunctionFixture.h"

// MS2e3 시험이다(Docs/Design/MaterialSystem 6.12절 MS2e3): AttributeLayers(단일 레이어·혼합 사슬·
// relevance·불투명 레이어·통과), 검사 문구(용도·개수·노드 둘·함수 안), 레이어마다 파라미터 scope,
// 함수 용도 검사, LayerStack(혼합 없는 사슬·목록 검사·layerInputs), 의존 목록을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_layers_tests_file {

using namespace material_graph_fixture;
using namespace material_function_fixture;

namespace {

constexpr uint64 LayerAssetHigh = 0x4d414c4159455231ull;
constexpr Guid LayerLow{LayerAssetHigh, 1};
constexpr Guid LayerHigh{LayerAssetHigh, 2};
constexpr Guid BlendTop{LayerAssetHigh, 3};
constexpr Guid BlendBottom{LayerAssetHigh, 4};
constexpr Guid LayerPass{LayerAssetHigh, 5};
constexpr Guid LayerParameter{LayerAssetHigh, 6};
constexpr Guid FunctionWithLayers{LayerAssetHigh, 7};
constexpr Guid LayerOutputId{0x4d414c4f55545055ull, 1};
constexpr Guid LayerInputId{0x4d414c494e505554ull, 1};
constexpr Guid LayerInputSecondId{0x4d414c494e505554ull, 2};
constexpr Guid LayerParameterId{0x4d414c5041524d31ull, 1};
constexpr uint64 LayerIdHigh = 0x4c41594552494431ull;

// 레이어 항목 하나다(시험 입력).
struct LayerSpec {
    Guid layer;
    Guid blend;
    bool visible = true;
};

// 상수 BaseColor 레이어 함수다: MakeAttributes(BaseColor = value) → LayerOutput.
void BuildConstantLayer(FunctionFixture& function, float value) noexcept {
    function.snapshot.usage = MaterialFunctionUsage::Layer;
    const Guid make = function.graph.Add("Material.MakeAttributes");
    function.graph.Connect(AddConstant(function.graph, value), "Result", make, "BaseColor");
    const Guid output = AddLayerOutput(function.graph, LayerOutputId);
    function.graph.Connect(make, "Result", output, "A");
}

// 통과 레이어 함수다: FunctionInput(MaterialAttributes) → LayerOutput.
void BuildPassLayer(FunctionFixture& function) noexcept {
    function.snapshot.usage = MaterialFunctionUsage::Layer;
    const Guid input =
        AddFunctionInput(function.graph, LayerInputId, FunctionInputType::MaterialAttributes);
    const Guid output = AddLayerOutput(function.graph, LayerOutputId);
    function.graph.Connect(input, "Result", output, "A");
}

// relevance·정렬 순을 준 MaterialAttributes 입력이다.
Guid AddBlendInput(GraphFixture& fixture, Guid id, BlendInputRelevance relevance,
                   int32 sortPriority, StringView name) noexcept {
    FunctionInputProperties properties(fixture.env.allocator);
    properties.inputName = fixture.env.Intern(name);
    properties.inputType = FunctionInputType::MaterialAttributes;
    properties.blendInputRelevance = relevance;
    properties.sortPriority = sortPriority;
    properties.id = id;
    return fixture.Add("Material.FunctionInput", fixture.env.Save(properties));
}

// 혼합 함수다: 정렬 순 0이 Top, 1이 Bottom인 두 입력 중 keepTop이면 Top, 아니면 Bottom을 낸다
// (relevance가 정렬 순보다 앞선다).
void BuildBlend(FunctionFixture& function, bool keepTop) noexcept {
    function.snapshot.usage = MaterialFunctionUsage::LayerBlend;
    const Guid top =
        AddBlendInput(function.graph, LayerInputId, BlendInputRelevance::Top, 0, "Top");
    const Guid bottom =
        AddBlendInput(function.graph, LayerInputSecondId, BlendInputRelevance::Bottom, 1, "Bottom");
    const Guid output = AddLayerOutput(function.graph, LayerOutputId);
    function.graph.Connect(keepTop ? top : bottom, "Result", output, "A");
}

void FillLayers(TranslatorEnv& env, LayerStackDefaults& defaults, Span<const LayerSpec> specs,
                bool& ready) noexcept {
    for (usize at = 0; at < specs.Size(); ++at) {
        LayerStackLayer layer(env.allocator);
        layer.layerId = Guid{LayerIdHigh, at + 1};
        layer.layer.assetId = AssetId{specs[at].layer};
        layer.blend.assetId = AssetId{specs[at].blend};
        layer.visible = specs[at].visible;
        ready = defaults.layers.TryPushBack(Move(layer)).IsOk() && ready;
    }
}

Guid AddAttributeLayers(GraphFixture& fixture, Span<const LayerSpec> specs) noexcept {
    AttributeLayersProperties properties(fixture.env.allocator);
    bool ready = true;
    FillLayers(fixture.env, properties.defaultLayers, specs, ready);
    fixture.ready = ready && fixture.ready;
    return fixture.Add("Material.AttributeLayers", fixture.env.Save(properties));
}

Guid AddLayerStack(GraphFixture& fixture, Span<const LayerSpec> specs, Span<const Guid> layers,
                   Span<const Guid> blends, Span<const StringView> inputs = {}) noexcept {
    LayerStackProperties properties(fixture.env.allocator);
    bool ready = true;
    FillLayers(fixture.env, properties.defaultLayers, specs, ready);
    for (Guid asset : layers) {
        MaterialAssetReference reference;
        reference.assetId = AssetId{asset};
        ready = properties.availableLayers.TryPushBack(reference).IsOk() && ready;
    }

    for (Guid asset : blends) {
        MaterialAssetReference reference;
        reference.assetId = AssetId{asset};
        ready = properties.availableBlends.TryPushBack(reference).IsOk() && ready;
    }

    for (StringView name : inputs) {
        LayerStackInput input;
        input.inputName = fixture.env.Intern(name);
        input.inputType = FunctionInputType::Vector3;
        ready = properties.layerInputs.TryPushBack(input).IsOk() && ready;
    }

    fixture.ready = ready && fixture.ready;
    return fixture.Add("Material.LayerStack", fixture.env.Save(properties));
}

// 레이어 노드 결과의 BaseColor를 Unlit emissive로 내보낸다.
void EmitBaseColor(GraphFixture& fixture, Guid layers) noexcept {
    const Guid breaker = fixture.Add("Material.BreakAttributes");
    fixture.Connect(layers, "Result", breaker, "MaterialAttributes");
    EmitEmissive(fixture, breaker, "BaseColor");
}

bool LayersNear(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < 1e-5f && difference > -1e-5f;
}

} // namespace

// 단일 레이어: 결과는 레이어 함수의 첫 출력이고, 레이어 함수가 의존 목록에 남는다.
TEST(MaterialLayers_SingleLayer) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture low(env, LayerLow, "Low");
    BuildConstantLayer(low, 0.3f);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(low.snapshot));
    GraphFixture main(env);
    const LayerSpec specs[] = {{LayerLow, Guid{}}};
    EmitBaseColor(main, AddAttributeLayers(main, specs));
    REQUIRE(main.ready);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    CHECK(LayersNear(EmissiveConstant(out), 0.3f));
    REQUIRE(out.dependencies.Size() == 1);
    CHECK(out.dependencies[0].asset == LayerLow);
}

// 혼합 사슬: 혼합 함수는 (아래, 위)를 받고 relevance가 정렬 순보다 앞선다. 혼합 함수가 없는 위
// 레이어는 불투명이라 결과가 그 레이어다(UE).
TEST(MaterialLayers_BlendChain) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture low(env, LayerLow, "Low");
    BuildConstantLayer(low, 0.2f);
    FunctionFixture high(env, LayerHigh, "High");
    BuildConstantLayer(high, 0.8f);
    FunctionFixture top(env, BlendTop, "KeepTop");
    BuildBlend(top, true);
    FunctionFixture bottom(env, BlendBottom, "KeepBottom");
    BuildBlend(bottom, false);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(low.snapshot));
    REQUIRE(sources.AddFunction(high.snapshot));
    REQUIRE(sources.AddFunction(top.snapshot));
    REQUIRE(sources.AddFunction(bottom.snapshot));
    const struct {
        Guid blend;
        float expected;
    } cases[] = {{BlendTop, 0.8f}, {BlendBottom, 0.2f}, {Guid{}, 0.8f}};
    for (const auto& item : cases) {
        GraphFixture main(env);
        const LayerSpec specs[] = {{LayerLow, Guid{}}, {LayerHigh, item.blend}};
        EmitBaseColor(main, AddAttributeLayers(main, specs));
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        REQUIRE(out.succeeded);
        CHECK(LayersNear(EmissiveConstant(out), item.expected));
    }
}

// 활성 레이어가 없으면 Input을 그대로 낸다(보이지 않는 레이어는 활성이 아니다).
TEST(MaterialLayers_PassThrough) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture low(env, LayerLow, "Low");
    BuildConstantLayer(low, 0.3f);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(low.snapshot));
    GraphFixture main(env);
    const LayerSpec specs[] = {{LayerLow, Guid{}, false}};
    const Guid layers = AddAttributeLayers(main, specs);
    const Guid make = main.Add("Material.MakeAttributes");
    main.Connect(AddConstant(main, 0.5f), "Result", make, "BaseColor");
    main.Connect(make, "Result", layers, "Input");
    EmitBaseColor(main, layers);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    CHECK(LayersNear(EmissiveConstant(out), 0.5f));

    GraphFixture empty(env);
    EmitBaseColor(empty, AddAttributeLayers(empty, {}));
    MaterialTranslation emptyOut(env.allocator);
    REQUIRE(TranslateWithSources(empty, &sources, emptyOut));
    CHECK(emptyOut.succeeded);
}

// AttributeLayers 검사 문구는 UE와 같고("LayerValidation:" 접두), 뒤에 "Failed to validate layer
// configuration."이 온다.
TEST(MaterialLayers_Validation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture general(env, LayerLow, "Plain");
    BuildConstantLayer(general, 0.3f);
    general.snapshot.usage = MaterialFunctionUsage::General;
    FunctionFixture high(env, LayerHigh, "High");
    BuildConstantLayer(high, 0.8f);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(general.snapshot));
    REQUIRE(sources.AddFunction(high.snapshot));
    {
        GraphFixture main(env);
        const LayerSpec specs[] = {{LayerLow, Guid{}}};
        const Guid layers = AddAttributeLayers(main, specs);
        EmitBaseColor(main, layers);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(!out.succeeded);
        CHECK(HasErrorAt(out, MaterialDiagnosticCode::LayerStackRule, layers));
        CHECK(
            HasMessageAt(out, layers, "LayerValidation:Layer 0, Plain, not set for layer usage."));
        CHECK(HasMessageAt(out, layers, "Failed to validate layer configuration."));
    }

    {
        // 첫 레이어가 보이지 않으면 혼합 자리 수가 활성 레이어 수와 같아 개수 오류다(UE와 같다).
        GraphFixture main(env);
        const LayerSpec specs[] = {
            {LayerHigh, Guid{}, false}, {LayerHigh, Guid{}}, {LayerHigh, Guid{}}};
        const Guid layers = AddAttributeLayers(main, specs);
        EmitBaseColor(main, layers);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasMessageAt(out, layers,
                           "LayerValidation:Invalid number of layers (2) or blends (2) assigned."));
    }

    {
        // 머티리얼의 레이어 노드는 하나다(쓰임과 무관 — 두 번째 노드 위치).
        GraphFixture main(env);
        const LayerSpec specs[] = {{LayerHigh, Guid{}}};
        const Guid first = AddAttributeLayers(main, specs);
        const Guid second = AddAttributeLayers(main, specs);
        EmitBaseColor(main, first);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::LayerStackRule, second));
        CHECK(HasMessageAt(out, second,
                           "Materials can contain only one Material Attribute Layers node."));
    }
}

// 함수 안의 레이어 노드는 쓸 수 없다(진단은 바깥 호출 노드 — 함수 경로·노드 접두).
TEST(MaterialLayers_InsideFunction) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, FunctionWithLayers, "F");
    const Guid inner = AddAttributeLayers(function.graph, {});
    const Guid output = AddFunctionOutput(function.graph, LayerOutputId);
    function.graph.Connect(inner, "Result", output, "A");
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    GraphFixture main(env);
    const Guid outputs[] = {LayerOutputId};
    const Guid call = AddCall(main, FunctionWithLayers, {}, outputs);
    const Guid breaker = main.Add("Material.BreakAttributes");
    main.Connect(call, PinKey(env, LayerOutputId), breaker, "MaterialAttributes");
    EmitEmissive(main, breaker, "BaseColor");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(!out.succeeded);
    CHECK(HasMessageAt(out, call,
                       "(Function F) (Node AttributeLayers) LayerValidation:Layer expressions "
                       "cannot be used within a material function."));
}

// 같은 레이어 함수를 두 레이어에 쓰면 공유가 아닌 파라미터는 레이어마다 따로다(scope Layer,
// layerId).
TEST(MaterialLayers_ParameterPerLayer) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture layer(env, LayerParameter, "Parameterized");
    layer.snapshot.usage = MaterialFunctionUsage::Layer;
    const Guid make = layer.graph.Add("Material.MakeAttributes");
    layer.graph.Connect(AddScalarParameter(layer.graph, LayerParameterId, "P", false), "Result",
                        make, "BaseColor");
    const Guid output = AddLayerOutput(layer.graph, LayerOutputId);
    layer.graph.Connect(make, "Result", output, "A");
    FunctionFixture blend(env, BlendTop, "KeepTop");
    BuildBlend(blend, true);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(layer.snapshot));
    REQUIRE(sources.AddFunction(blend.snapshot));
    GraphFixture main(env);
    const LayerSpec specs[] = {{LayerParameter, Guid{}}, {LayerParameter, BlendTop}};
    const Guid layers = AddAttributeLayers(main, specs);
    // 두 레이어 결과를 모두 쓰도록 아래 레이어 값도 혼합 입력으로 남는다(DCE는 위만 남긴다 — 정체만
    // 본다).
    EmitBaseColor(main, layers);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    REQUIRE(out.module.Parameters().Size() == 1);
    CHECK(!(out.module.Parameters()[0].id == LayerParameterId));

    // 위 레이어를 Bottom 혼합으로 바꾸면 아래 레이어의 파라미터가 남고 정체가 다르다.
    FunctionFixture keepBottom(env, BlendBottom, "KeepBottom");
    BuildBlend(keepBottom, false);
    REQUIRE(sources.AddFunction(keepBottom.snapshot));
    GraphFixture other(env);
    const LayerSpec bottomSpecs[] = {{LayerParameter, Guid{}}, {LayerParameter, BlendBottom}};
    EmitBaseColor(other, AddAttributeLayers(other, bottomSpecs));
    MaterialTranslation otherOut(env.allocator);
    REQUIRE(TranslateWithSources(other, &sources, otherOut));
    REQUIRE(otherOut.succeeded);
    REQUIRE(otherOut.module.Parameters().Size() == 1);
    CHECK(!(otherOut.module.Parameters()[0].id == out.module.Parameters()[0].id));
}

// 보통 호출로 써도 용도 Layer 함수의 입출력 규칙을 본다(UE ValidateFunctionUsage).
TEST(MaterialLayers_FunctionUsage) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture function(env, LayerLow, "TwoInputs");
    function.snapshot.usage = MaterialFunctionUsage::Layer;
    const Guid first =
        AddFunctionInput(function.graph, LayerInputId, FunctionInputType::MaterialAttributes);
    AddFunctionInput(function.graph, LayerInputSecondId, FunctionInputType::MaterialAttributes);
    const Guid output = AddLayerOutput(function.graph, LayerOutputId);
    function.graph.Connect(first, "Result", output, "A");
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(function.snapshot));
    GraphFixture main(env);
    const Guid outputs[] = {LayerOutputId};
    const Guid call = AddCall(main, LayerLow, {}, outputs);
    const Guid breaker = main.Add("Material.BreakAttributes");
    main.Connect(call, PinKey(env, LayerOutputId), breaker, "MaterialAttributes");
    EmitEmissive(main, breaker, "BaseColor");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::LayerStackRule, call));
    CHECK(HasMessageAt(out, call,
                       "LayerValidation:Layer TwoInputs: can only have up to 1 Material Attributes "
                       "type output node. It has 2"));
}

// LayerStack: 혼합이 없으면 위 레이어의 MaterialAttributes 입력에 아래 레이어 결과를 잇는다.
TEST(MaterialLayers_LayerStack) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture low(env, LayerLow, "Low");
    BuildConstantLayer(low, 0.2f);
    FunctionFixture pass(env, LayerPass, "Pass");
    BuildPassLayer(pass);
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(low.snapshot));
    REQUIRE(sources.AddFunction(pass.snapshot));
    const Guid available[] = {LayerLow, LayerPass};
    GraphFixture main(env);
    const LayerSpec specs[] = {{LayerLow, Guid{}}, {LayerPass, Guid{}}};
    EmitBaseColor(main, AddLayerStack(main, specs, available, {}));
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateWithSources(main, &sources, out));
    REQUIRE(out.succeeded);
    CHECK(LayersNear(EmissiveConstant(out), 0.2f));
}

// LayerStack 검사: 목록 밖 레이어, 연결되지 않은 layerInputs, 용도가 다른 쓸 수 있는 혼합.
TEST(MaterialLayers_LayerStackValidation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    FunctionFixture low(env, LayerLow, "Low");
    BuildConstantLayer(low, 0.2f);
    FunctionFixture plain(env, BlendTop, "Plain");
    BuildBlend(plain, true);
    plain.snapshot.usage = MaterialFunctionUsage::General;
    MemorySources sources(env.allocator);
    REQUIRE(sources.AddFunction(low.snapshot));
    REQUIRE(sources.AddFunction(plain.snapshot));
    const LayerSpec specs[] = {{LayerLow, Guid{}}};
    {
        GraphFixture main(env);
        const Guid stack = AddLayerStack(main, specs, {}, {});
        EmitBaseColor(main, stack);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasMessageAt(out, stack,
                           "LayerValidation:Layer 0, Low, is not set as an allowed layer in the "
                           "base layer stack node."));
        CHECK(HasMessageAt(out, stack, "Failed to validate layer configuration."));
    }

    {
        GraphFixture main(env);
        const Guid available[] = {LayerLow};
        const StringView inputs[] = {"Tint"};
        const Guid stack = AddLayerStack(main, specs, available, {}, inputs);
        EmitBaseColor(main, stack);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasMessageAt(out, stack,
                           "LayerValidation:LayerStack \"Tint\" input pin must be connected."));
    }

    {
        GraphFixture main(env);
        const Guid available[] = {LayerLow};
        const Guid blends[] = {BlendTop};
        const Guid stack = AddLayerStack(main, specs, available, blends);
        EmitBaseColor(main, stack);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateWithSources(main, &sources, out));
        CHECK(HasMessageAt(out, stack,
                           "LayerValidation:Function Plain: function usage is not set for use as "
                           "a blend."));
        CHECK(HasMessageAt(out, stack,
                           "LayerValidation:LayerStack Available Layers/Blends contain invalid "
                           "functions."));
    }
}

} // namespace material_layers_tests_file
