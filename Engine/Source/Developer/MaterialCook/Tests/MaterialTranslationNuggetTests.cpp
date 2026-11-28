#include "../../MaterialCompiler/Tests/MaterialGraphFixture.h"
#include "../../MaterialCompiler/Tests/MaterialMathGraph.h"

#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialCompiler/MaterialShaderTraitsBuilder.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialCook/MaterialCookCatalog.h>
#include <MaterialCook/MaterialCookSources.h>
#include <MaterialCook/MaterialTranslationKey.h>
#include <MaterialCook/MaterialTranslationNugget.h>

// 번역 너겟 시험이다(Docs/Design/MaterialSystem 12.8절 MS3c "번역 너겟", MS-T1·MS-T5 번역 부분):
// 같은 그래프는 같은 키(노드 저장 순서와 무관), 그래프·기본 속성·static 값·번역 대상을 바꾸면 다른
// 키, 파라미터 기본값만 바꾸면 같은 키다. 값은 왕복이 같은 byte이고 손상(머리·잘린 part)은
// CorruptData, 셰이더 집합 id는 같은 값에서 같고 묶음 id는 0이 아니다.
using namespace rocks;
using namespace rocks::material;
namespace material_translation_nugget_tests_file {

using namespace material_graph_fixture;

namespace {

// 시험 static 파라미터 id다.
constexpr Guid StaticParameterId{0x53544154ull, 1};
// 기본값만 바꿔 보는 파라미터 기본값 둘이다.
constexpr float FirstDefault = 0.25f;
constexpr float SecondDefault = 0.75f;

struct KeyEnv {
    explicit KeyEnv(TranslatorEnv& env) noexcept
        : catalog(env.allocator, env.names), sources(env.allocator),
          ready(catalog.Initialize().IsOk()) {
    }

    MaterialCookCatalog catalog;
    MaterialCookSources sources;
    bool ready;
};

Result<Sha256Digest> KeyOf(TranslatorEnv& env, KeyEnv& keys, const nodegraph::GraphDocument& root,
                           const MaterialTraitsDesc& traits,
                           Span<const MaterialStaticParameterValue> statics = {},
                           MaterialTranslationTarget target = {}) noexcept {
    MaterialTranslationKeyDesc desc;
    desc.catalog = &keys.catalog;
    desc.registry = &env.objects;
    desc.access = env.objects.OwnerAccess();
    desc.root = &root;
    desc.traits = &traits;
    desc.statics = statics;
    desc.target = target;
    desc.sources = &keys.sources;
    return ComputeMaterialTranslationKey(desc);
}

// 파라미터 하나를 PDO에 잇는 그래프다(기본값만 다르게).
void BuildParameterGraph(GraphFixture& fixture, float value) noexcept {
    ScalarParameterProperties properties;
    properties.parameterName = fixture.env.Intern("Gloss");
    properties.parameterId = Guid{0x504152ull, 1};
    properties.defaultValue = value;
    const Guid parameter = fixture.Add("Material.ScalarParameter", fixture.env.Save(properties));
    fixture.Connect(parameter, "Result", OutputNodeId, "PixelDepthOffset");
}

} // namespace

// MS-T1·MS-T5 번역 키다.
TEST(MaterialCook_TranslationKeyInvalidation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    KeyEnv keys(env);
    REQUIRE(keys.ready);
    const MaterialTraitsDesc masked = Traits(BlendMode::Masked);

    GraphFixture first(env);
    BuildMathGraph(first);
    GraphFixture second(env);
    BuildMathGraph(second);
    auto key = KeyOf(env, keys, first.document, masked);
    auto same = KeyOf(env, keys, second.document, masked);
    REQUIRE(key);
    REQUIRE(same);
    CHECK(key.Value() == same.Value());

    // 노드 저장 순서만 섞어도 같다(MS-T1 (b)).
    Array<nodegraph::GraphNode>& nodes = second.document.graphs[0].nodes;
    for (usize index = 0; index < nodes.Size() / 2; ++index) {
        nodegraph::GraphNode swap = Move(nodes[index]);
        nodes[index] = Move(nodes[nodes.Size() - 1 - index]);
        nodes[nodes.Size() - 1 - index] = Move(swap);
    }

    auto shuffled = KeyOf(env, keys, second.document, masked);
    REQUIRE(shuffled);
    CHECK(shuffled.Value() == key.Value());

    // 기본 속성·번역 대상·static 값·그래프를 바꾸면 다르다.
    auto opaque = KeyOf(env, keys, first.document, Traits());
    MaterialTranslationTarget high;
    high.quality = MaterialQualityLevel::High;
    auto quality = KeyOf(env, keys, first.document, masked, {}, high);
    MaterialStaticParameterValue flag;
    flag.parameterId = StaticParameterId;
    flag.value = true;
    auto statics = KeyOf(env, keys, first.document, masked,
                         Span<const MaterialStaticParameterValue>(&flag, 1));
    GraphFixture larger(env);
    BuildMathGraph(larger);
    (void)larger.Add("Material.Sine");
    auto graph = KeyOf(env, keys, larger.document, masked);
    REQUIRE(opaque);
    REQUIRE(quality);
    REQUIRE(statics);
    REQUIRE(graph);
    CHECK(!(opaque.Value() == key.Value()));
    CHECK(!(quality.Value() == key.Value()));
    CHECK(!(statics.Value() == key.Value()));
    CHECK(!(graph.Value() == key.Value()));

    // 파라미터 기본값만 바꾸면 같다(semantic hash 밖 — 5.5절).
    GraphFixture low(env);
    BuildParameterGraph(low, FirstDefault);
    GraphFixture highDefault(env);
    BuildParameterGraph(highDefault, SecondDefault);
    auto lowKey = KeyOf(env, keys, low.document, Traits());
    auto highKey = KeyOf(env, keys, highDefault.document, Traits());
    REQUIRE(lowKey);
    REQUIRE(highKey);
    CHECK(lowKey.Value() == highKey.Value());
}

// 값 왕복·손상·셰이더 집합 id다.
TEST(MaterialCook_TranslationValueRoundTrip) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BuildMathGraph(fixture);
    const MaterialTraitsDesc masked = Traits(BlendMode::Masked);
    MaterialTranslation translation(env.allocator);
    REQUIRE(Translate(fixture, masked, translation));
    REQUIRE(translation.succeeded);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(WriteMaterialSource(env.allocator, translation.module, masked, TestAssetId, MainGraphId,
                                source));
    REQUIRE(source.succeeded);
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(translation.module, constants));
    MaterialShaderTraits traits(env.allocator);
    REQUIRE(MakeMaterialShaderTraits(translation, masked, MaterialQualityLevel::None, traits));

    MaterialTranslationValue value(env.allocator);
    REQUIRE(MakeMaterialTranslationValue(env.names, translation, source, constants, Move(traits),
                                         TestAssetId, value));
    CHECK(value.source.View() == source.text.View());
    CHECK(value.irHash == translation.module.Hash());

    Array<NuggetValue> parts(env.allocator);
    REQUIRE(WriteMaterialTranslationNugget(value, parts));
    REQUIRE(parts.Size() == MaterialTranslationValueParts);
    MaterialTranslationValue read(env.allocator);
    REQUIRE(ReadMaterialTranslationNugget(parts.View(), read));
    Array<NuggetValue> again(env.allocator);
    REQUIRE(WriteMaterialTranslationNugget(read, again));
    REQUIRE(again.Size() == parts.Size());
    for (usize index = 0; index < parts.Size(); ++index) {
        CHECK(again[index].bytes.Size() == parts[index].bytes.Size());
        bool equal = again[index].bytes.Size() == parts[index].bytes.Size();
        for (usize at = 0; equal && at < parts[index].bytes.Size(); ++at) {
            equal = again[index].bytes[at] == parts[index].bytes[at];
        }

        CHECK(equal);
    }

    auto set = MaterialShaderSetId(value);
    auto readSet = MaterialShaderSetId(read);
    REQUIRE(set);
    REQUIRE(readSet);
    CHECK(set.Value() == readSet.Value());
    CHECK(MaterialShaderBundleId(set.Value()) != 0);

    // 손상: 머리 버전, 잘린 part, part 수.
    parts[0].bytes[0] ^= 0xffu;
    CHECK(ReadMaterialTranslationNugget(parts.View(), read).Error() == ErrorCode::CorruptData);
    parts[0].bytes[0] ^= 0xffu;
    REQUIRE(parts[2].bytes.TryResize(parts[2].bytes.Size() - 1));
    CHECK(ReadMaterialTranslationNugget(parts.View(), read).Error() == ErrorCode::CorruptData);
    CHECK(ReadMaterialTranslationNugget(Span<const NuggetValue>(parts.Data(), 3), read).Error() ==
          ErrorCode::CorruptData);
}

} // namespace material_translation_nugget_tests_file
