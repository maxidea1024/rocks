#include "../../MaterialCompiler/Tests/MaterialGraphFixture.h"
#include "../../MaterialCompiler/Tests/MaterialMathGraph.h"

#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialCompiler/MaterialShaderTraitsBuilder.h>
#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialCook/MaterialArtifactBuild.h>
#include <MaterialCook/MaterialCookCatalog.h>
#include <MaterialCook/MaterialCookSources.h>
#include <MaterialCook/MaterialTranslationKey.h>
#include <MaterialCook/MaterialTranslationNugget.h>
#include <MaterialSchema/MaterialArtifactValidation.h>
#include <MaterialSchema/MaterialAttributeTable.h>
#include <ShaderCore/ShaderLibrary.h>

// 번역 너겟 시험이다(Docs/Design/MaterialSystem 12.8절 MS3c "번역 너겟", MS-T1·MS-T5 번역 부분):
// 같은 그래프는 같은 키(노드 저장 순서와 무관), 그래프·기본 속성·static 값·번역 대상·custom 속성
// 표를 바꾸면 다른 키, 파라미터 기본값·static 값의 정규형 밖 칸만 바꾸면 같은 키다. static
// parameterId가 겹치거나 NamePool이 다르면 InvalidArgument다. 값은 왕복이 같은 byte이고 손상(머리·
// part id·잘린 part·의미 범위)은 CorruptData, 셰이더 집합 id는 같은 값에서 같고 IR 값 번호에
// 무관하며 layout 모양에 따라 바뀌고 묶음 id는 0이 아니다.
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

// 값을 해석해 고친 뒤 다시 써서 해석한 결과다(의미 검사 시험).
template <typename Edit>
ErrorCode ReadEdited(TranslatorEnv& env, Span<const NuggetValue> parts, Edit edit) noexcept {
    MaterialTranslationValue probe(env.allocator);
    if (!ReadMaterialTranslationNugget(parts, probe)) {
        return ErrorCode::InvalidArgument;
    }

    edit(probe);
    Array<NuggetValue> written(env.allocator);
    if (!WriteMaterialTranslationNugget(probe, written)) {
        return ErrorCode::InvalidArgument;
    }

    MaterialTranslationValue sink(env.allocator);
    auto read = ReadMaterialTranslationNugget(written.View(), sink);
    return read ? ErrorCode::None : read.Error();
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

    // static 정규형: Bool은 mask를, ComponentMask는 value를 보지 않는다. 같은 parameterId 둘은
    // 오류다.
    MaterialStaticParameterValue noisy = flag;
    noisy.mask[0] = true;
    auto normalized = KeyOf(env, keys, first.document, masked,
                            Span<const MaterialStaticParameterValue>(&noisy, 1));
    REQUIRE(normalized);
    CHECK(normalized.Value() == statics.Value());
    MaterialStaticParameterValue channels;
    channels.parameterId = StaticParameterId;
    channels.kind = MaterialStaticParameterKind::ComponentMask;
    channels.mask[1] = true;
    MaterialStaticParameterValue channelsNoisy = channels;
    channelsNoisy.value = true;
    auto maskKey = KeyOf(env, keys, first.document, masked,
                         Span<const MaterialStaticParameterValue>(&channels, 1));
    auto maskNoisy = KeyOf(env, keys, first.document, masked,
                           Span<const MaterialStaticParameterValue>(&channelsNoisy, 1));
    REQUIRE(maskKey);
    REQUIRE(maskNoisy);
    CHECK(maskKey.Value() == maskNoisy.Value());
    const MaterialStaticParameterValue twice[] = {flag, channels};
    CHECK(
        KeyOf(env, keys, first.document, masked, Span<const MaterialStaticParameterValue>(twice, 2))
            .Error() == ErrorCode::InvalidArgument);

    // custom 속성 표: 넘기면 내장 표("builtin")와 다른 키다.
    MaterialAttributeTable attributes(env.allocator);
    MaterialTranslationKeyDesc withTable;
    withTable.catalog = &keys.catalog;
    withTable.registry = &env.objects;
    withTable.access = env.objects.OwnerAccess();
    withTable.root = &first.document;
    withTable.traits = &masked;
    withTable.sources = &keys.sources;
    withTable.attributes = &attributes;
    auto tableKey = ComputeMaterialTranslationKey(withTable);
    REQUIRE(tableKey);
    CHECK(!(tableKey.Value() == key.Value()));

    // 카탈로그 NamePool이 Registry의 것과 다르면 InvalidArgument다(카탈로그를 쓰기 전에 거른다 —
    // 초기화하지 않은 카탈로그로 충분하다).
    NamePool other(env.allocator);
    MaterialCookCatalog foreign(env.allocator, other);
    MaterialTranslationKeyDesc mismatched = withTable;
    mismatched.catalog = &foreign;
    mismatched.attributes = nullptr;
    CHECK(ComputeMaterialTranslationKey(mismatched).Error() == ErrorCode::InvalidArgument);
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
    CHECK(shader::MaterialShaderBundleId(set.Value()) != 0);

    // 셰이더 집합 id: 상수 멤버의 IR 값 번호는 넣지 않고 float4 번호(모양)는 넣는다.
    {
        MaterialTranslationValue shaped(env.allocator);
        REQUIRE(ReadMaterialTranslationNugget(parts.View(), shaped));
        MaterialConstantMember member;
        member.value = 7;
        member.components = 1;
        shaped.floatVectors = shaped.floatVectors + 2;
        REQUIRE(shaped.members.TryPushBack(member));
        auto base = MaterialShaderSetId(shaped);
        shaped.members[shaped.members.Size() - 1].value = 99;
        auto renumbered = MaterialShaderSetId(shaped);
        shaped.members[shaped.members.Size() - 1].vector = 1;
        auto moved = MaterialShaderSetId(shaped);
        REQUIRE(base);
        REQUIRE(renumbered);
        REQUIRE(moved);
        CHECK(base.Value() == renumbered.Value());
        CHECK(!(moved.Value() == base.Value()));
    }

    // 의미 손상: 멤버 명령·float4 범위, 성분 넘침, 앞 아닌 피연산자, 텍스처 slot, 성분 수 5.
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              MaterialConstantMember member;
              member.step = static_cast<uint32>(probe.steps.Size());
              member.components = 1;
              probe.floatVectors = probe.floatVectors + 1;
              (void)probe.members.TryPushBack(member);
          }) == ErrorCode::CorruptData);
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue constant;
              (void)probe.steps.TryPushBack(constant);
              MaterialConstantMember member;
              member.step = static_cast<uint32>(probe.steps.Size() - 1);
              member.vector = probe.floatVectors;
              member.components = 1;
              (void)probe.members.TryPushBack(member);
          }) == ErrorCode::CorruptData);
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue constant;
              (void)probe.steps.TryPushBack(constant);
              MaterialConstantMember member;
              member.step = static_cast<uint32>(probe.steps.Size() - 1);
              member.firstComponent = 3;
              member.components = 2;
              probe.floatVectors = probe.floatVectors + 1;
              member.vector = probe.floatVectors - 1;
              (void)probe.members.TryPushBack(member);
          }) == ErrorCode::CorruptData);
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue forward;
              forward.operandCount = 1;
              forward.operands[0] = static_cast<IrValueId>(probe.steps.Size());
              (void)probe.steps.TryPushBack(forward);
          }) == ErrorCode::CorruptData);
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue texture;
              texture.op = IrOp::Texture;
              texture.immediate[3] = probe.textures.Size();
              (void)probe.steps.TryPushBack(texture);
          }) == ErrorCode::CorruptData);
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue wide;
              wide.type.components = MaxIrComponents + 1;
              (void)probe.steps.TryPushBack(wide);
          }) == ErrorCode::CorruptData);
    // 고치지 않으면 그대로 읽힌다(위 검사가 정상 값을 거부하지 않는다).
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue&) noexcept {}) ==
          ErrorCode::None);

    // part 0 id.
    parts[0].id = MaterialTranslationValueParts;
    CHECK(ReadMaterialTranslationNugget(parts.View(), read).Error() == ErrorCode::CorruptData);
    parts[0].id = 0;

    // 손상: 머리 버전, 잘린 part, part 수.
    parts[0].bytes[0] ^= 0xffu;
    CHECK(ReadMaterialTranslationNugget(parts.View(), read).Error() == ErrorCode::CorruptData);
    parts[0].bytes[0] ^= 0xffu;
    REQUIRE(parts[2].bytes.TryResize(parts[2].bytes.Size() - 1));
    CHECK(ReadMaterialTranslationNugget(parts.View(), read).Error() == ErrorCode::CorruptData);
    CHECK(ReadMaterialTranslationNugget(Span<const NuggetValue>(parts.Data(), 3), read).Error() ==
          ErrorCode::CorruptData);
}

// 값 v2(12.8절 MS3d2): 파라미터 출처·컬렉션 part 왕복, 출처 누락·scope 범위 밖은 손상, artifact
// 변환(명령 연산·파라미터 번호·칸·셰이더 집합 id·layout 서명), links 길이 불일치는 InvalidArgument.
TEST(MaterialCook_TranslationValueOriginsAndArtifact) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BuildParameterGraph(fixture, FirstDefault);
    const MaterialTraitsDesc traits = Traits();
    MaterialTranslation translation(env.allocator);
    REQUIRE(Translate(fixture, traits, translation));
    REQUIRE(translation.succeeded);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(WriteMaterialSource(env.allocator, translation.module, traits, TestAssetId, MainGraphId,
                                source));
    MaterialConstants constants(env.allocator);
    REQUIRE(BuildMaterialConstants(translation.module, constants));
    MaterialShaderTraits shaderTraits(env.allocator);
    REQUIRE(
        MakeMaterialShaderTraits(translation, traits, MaterialQualityLevel::None, shaderTraits));
    MaterialTranslationValue value(env.allocator);
    REQUIRE(MakeMaterialTranslationValue(env.names, translation, source, constants,
                                         Move(shaderTraits), TestAssetId, value));
    REQUIRE(value.parameters.Size() == 1);
    REQUIRE(value.origins.Size() == 1);
    CHECK(value.origins[0].identity == value.parameters[0].id);
    CHECK(value.origins[0].name.View() == StringView("Gloss"));
    CHECK(value.origins[0].scope == ParameterScopeKind::Global);

    Array<NuggetValue> parts(env.allocator);
    REQUIRE(WriteMaterialTranslationNugget(value, parts));
    REQUIRE(parts.Size() == MaterialTranslationValueParts);
    MaterialTranslationValue read(env.allocator);
    REQUIRE(ReadMaterialTranslationNugget(parts.View(), read));
    REQUIRE(read.origins.Size() == 1);
    CHECK(read.origins[0].parameterId == value.origins[0].parameterId);
    CHECK(read.origins[0].name.View() == StringView("Gloss"));

    // 출처 없는 파라미터와 범위 밖 scope는 손상이다.
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              probe.origins.Clear();
          }) == ErrorCode::CorruptData);
    Array<NuggetValue> badScope(env.allocator);
    {
        MaterialTranslationValue probe(env.allocator);
        REQUIRE(ReadMaterialTranslationNugget(parts.View(), probe));
        probe.origins[0].scope = static_cast<ParameterScopeKind>(0xff);
        REQUIRE(WriteMaterialTranslationNugget(probe, badScope));
    }

    MaterialTranslationValue sink(env.allocator);
    CHECK(ReadMaterialTranslationNugget(badScope.View(), sink).Error() == ErrorCode::CorruptData);

    // artifact: 파라미터 하나를 Material::parameters 3번에 잇는다(명령 [3]은 artifact 번호 0
    // 그대로).
    constexpr uint32 ParameterSlot = 3;
    const uint32 parameterLinks[] = {ParameterSlot};
    MaterialArtifactLinks links;
    links.parameters = parameterLinks;
    MaterialArtifact artifact(env.allocator);
    REQUIRE(MakeMaterialArtifact(read, env.names, links, artifact));
    REQUIRE(artifact.program.Size() == read.steps.Size());
    REQUIRE(artifact.constants.Size() == read.members.Size());
    CHECK(artifact.floatVectors == read.floatVectors);
    bool parameterSeen = false;
    for (const MaterialConstantInstruction& instruction : artifact.program) {
        if (instruction.op == MaterialConstantOp::Parameter) {
            parameterSeen = true;
            CHECK(instruction.immediates[MaterialConstantTableImmediate] == 0);
            CHECK(instruction.scalar == MaterialConstantScalar::Float);
        }
    }

    CHECK(parameterSeen);
    REQUIRE(artifact.parameters.Size() == 1);
    CHECK(artifact.parameters[0] == ParameterSlot);
    auto setId = MaterialShaderSetId(read);
    auto layout = MaterialLayoutSignature(read);
    REQUIRE(setId);
    REQUIRE(layout);
    CHECK(artifact.shaderSetId == setId.Value());
    CHECK(artifact.layoutSignature == layout.Value());
    CHECK(!artifact.traits.IsEmpty());

    // 구조 검증: 표 크기 안이면 통과, 파라미터 표가 작으면 CorruptData다.
    CHECK(ValidateMaterialArtifact(artifact, MaterialArtifactTables{ParameterSlot + 1, 0, 0}));
    CHECK(ValidateMaterialArtifact(artifact, MaterialArtifactTables{ParameterSlot, 0, 0}).Error() ==
          ErrorCode::CorruptData);

    // 프로그램 밖 연산(Select)은 해석이 손상으로 본다(artifact 변환까지 가지 않는다).
    CHECK(ReadEdited(env, parts.View(), [](MaterialTranslationValue& probe) noexcept {
              IrValue select;
              select.op = IrOp::Select;
              (void)probe.steps.TryPushBack(select);
          }) == ErrorCode::CorruptData);

    // links에 없음 표지가 있으면 InvalidArgument다.
    const uint32 missingLinks[] = {NoMaterialReference};
    MaterialArtifactLinks missing;
    missing.parameters = missingLinks;
    MaterialArtifact unlinked(env.allocator);
    CHECK(MakeMaterialArtifact(read, env.names, missing, unlinked).Error() ==
          ErrorCode::InvalidArgument);

    // links 길이가 값과 다르면 InvalidArgument다.
    MaterialArtifactLinks empty;
    MaterialArtifact rejected(env.allocator);
    CHECK(MakeMaterialArtifact(read, env.names, empty, rejected).Error() ==
          ErrorCode::InvalidArgument);
}

// 손으로 만든 값의 artifact 변환(12.8절 MS3d2): 파라미터 텍스처 slot(출처·종류·샘플러·번호)과
// TextureSize의 logicalSize, Double 파라미터의 tile·offset 칸, 구조 검증의 손상 판정, IR 연산
// 대응표의 완전성(상수 프로그램 연산이면 변환되고 아니면 InvalidArgument).
TEST(MaterialCook_ArtifactFromHandValue) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    constexpr Guid TextureParameterId{0x54505241ull, 1};
    constexpr uint32 MaterialTextureIndex = 2;
    constexpr uint32 MaterialParameterIndex = 4;
    constexpr uint32 DoubleParameterIndex = 5;
    constexpr uint8 SizeComponents = 2;

    MaterialTranslationValue value(env.allocator);
    IrTexture texture;
    texture.source = IrTextureSource::Parameter;
    texture.id = TextureParameterId;
    texture.kind = IrTextureKind::Texture2D;
    texture.sampler = IrTextureSampler::SharedWrap;
    REQUIRE(value.textures.TryPushBack(texture));
    IrParameter wide;
    wide.id = Guid{0x44424c45ull, 1};
    wide.kind = IrParameterKind::DoubleVector;
    wide.type = IrDouble(MaxIrComponents);
    REQUIRE(value.parameters.TryPushBack(wide));

    IrValue textureStep;
    textureStep.op = IrOp::Texture;
    textureStep.type.scalar = IrScalar::Texture;
    textureStep.type.components = 0;
    textureStep.type.texture = IrTextureKind::Texture2D;
    textureStep.immediate[MaterialConstantTableImmediate] = 0;
    IrValue size;
    size.op = IrOp::TextureSize;
    size.type = IrFloat(SizeComponents);
    size.operandCount = 1;
    size.operands[0] = 0;
    IrValue parameter;
    parameter.op = IrOp::Parameter;
    parameter.type = IrDouble(MaxIrComponents);
    parameter.immediate[MaterialConstantTableImmediate] = 0;
    REQUIRE(value.steps.TryPushBack(textureStep));
    REQUIRE(value.steps.TryPushBack(size));
    REQUIRE(value.steps.TryPushBack(parameter));

    MaterialConstantMember tile;
    tile.step = 2;
    tile.kind = MaterialConstantSlotKind::DoubleTile;
    tile.vector = 0;
    tile.components = MaxIrComponents;
    MaterialConstantMember offset = tile;
    offset.kind = MaterialConstantSlotKind::DoubleOffset;
    offset.vector = 1;
    MaterialConstantMember sizeMember;
    sizeMember.step = 1;
    sizeMember.vector = 2;
    sizeMember.components = SizeComponents;
    REQUIRE(value.members.TryPushBack(tile));
    REQUIRE(value.members.TryPushBack(offset));
    REQUIRE(value.members.TryPushBack(sizeMember));
    value.floatVectors = 3;

    const uint32 parameterLinks[] = {DoubleParameterIndex};
    const uint32 textureLinks[] = {MaterialTextureIndex};
    const uint32 textureParameterLinks[] = {MaterialParameterIndex};
    MaterialArtifactLinks links;
    links.parameters = parameterLinks;
    links.textures = textureLinks;
    links.textureParameters = textureParameterLinks;
    MaterialArtifact artifact(env.allocator);
    REQUIRE(MakeMaterialArtifact(value, env.names, links, artifact));
    REQUIRE(artifact.textures.Size() == 1);
    const MaterialTextureSlot& slot = artifact.textures[0];
    CHECK(slot.source == MaterialTextureSource::Parameter);
    CHECK(slot.kind == MaterialTextureKind::Texture2D);
    CHECK(slot.sampler == MaterialSamplerSource::SharedWrap);
    CHECK(slot.parameter == MaterialParameterIndex);
    CHECK(slot.texture == MaterialTextureIndex);
    CHECK(slot.logicalSize);
    REQUIRE(artifact.program.Size() == 3);
    CHECK(artifact.program[0].scalar == MaterialConstantScalar::Texture);
    CHECK(artifact.program[0].components == 0);
    CHECK(artifact.program[1].operands[0] == 0);
    CHECK(artifact.program[1].operands[1] == 0);
    CHECK(artifact.program[2].scalar == MaterialConstantScalar::Double);
    REQUIRE(artifact.constants.Size() == 3);
    CHECK(artifact.constants[0].kind == MaterialConstantSlotKind::DoubleTile);
    CHECK(artifact.constants[1].kind == MaterialConstantSlotKind::DoubleOffset);
    CHECK(artifact.constants[2].instruction == 1);

    const MaterialArtifactTables tables{DoubleParameterIndex + 1, MaterialTextureIndex + 1, 0};
    CHECK(ValidateMaterialArtifact(artifact, tables));
    // 손상: Double 명령의 칸이 Value, TextureSize 아닌 명령이 Texture를 읽음, 텍스처 번호가 표 밖.
    artifact.constants[0].kind = MaterialConstantSlotKind::Value;
    CHECK(ValidateMaterialArtifact(artifact, tables).Error() == ErrorCode::CorruptData);
    artifact.constants[0].kind = MaterialConstantSlotKind::DoubleTile;
    artifact.program[1].op = MaterialConstantOp::Abs;
    CHECK(ValidateMaterialArtifact(artifact, tables).Error() == ErrorCode::CorruptData);
    artifact.program[1].op = MaterialConstantOp::TextureSize;
    CHECK(ValidateMaterialArtifact(
              artifact, MaterialArtifactTables{DoubleParameterIndex + 1, MaterialTextureIndex, 0})
              .Error() == ErrorCode::CorruptData);

    // 파라미터 텍스처 slot인데 파라미터 번호가 없으면 InvalidArgument다.
    const uint32 noParameter[] = {NoMaterialReference};
    MaterialArtifactLinks broken = links;
    broken.textureParameters = noParameter;
    MaterialArtifact rejected(env.allocator);
    CHECK(MakeMaterialArtifact(value, env.names, broken, rejected).Error() ==
          ErrorCode::InvalidArgument);

    // 대응표 완전성: 명령 하나짜리 값으로 IR 연산마다 변환 성공 여부가 상수 프로그램 연산 집합과
    // 같다(Texture는 위에서 본다).
    for (uint32 code = 0; code < IrOpCount; ++code) {
        const IrOp op = static_cast<IrOp>(code);
        if (op == IrOp::Texture) {
            continue;
        }

        MaterialTranslationValue single(env.allocator);
        REQUIRE(single.parameters.TryPushBack(wide));
        IrValue step;
        step.op = op;
        step.type = IrFloat(1);
        REQUIRE(single.steps.TryPushBack(step));
        const uint32 singleParameter[] = {0};
        MaterialArtifactLinks singleLinks;
        singleLinks.parameters = singleParameter;
        MaterialArtifact converted(env.allocator);
        const bool expected = op == IrOp::Constant || IsMaterialConstantProgramOp(op);
        CHECK(MakeMaterialArtifact(single, env.names, singleLinks, converted).IsOk() == expected);
    }
}

} // namespace material_translation_nugget_tests_file
