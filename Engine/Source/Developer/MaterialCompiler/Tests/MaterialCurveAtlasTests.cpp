#include <MaterialCompiler/MaterialCurveAtlasSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/CurveAtlasRowParameterProperties.h>
#include <MaterialSchema/Nodes/Texture/TextureNodeEnums.h>

#include "MaterialGraphFixture.h"
#include "MaterialIrEvaluator.h"

// MS2c5 시험이다(Docs/Design/MaterialSystem 6.10절 MS2c5 — UE CurveAtlasRowParameter): 행 번호
// scalar 파라미터(atlas의 첫 curve 행이 기본값)·custom primitive data 행, atlas 공유 Clamp
// LinearColor 샘플과 uv 식의 연산 구조, RGB 출력, 정점 문맥 mip 0, 번역 의존 CurveAtlas, UE 검사
// 순서·문구, 공급에 없는 atlas·사실 없는 atlas·2D 아닌 atlas를 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_curve_atlas_tests_file {

using namespace material_graph_fixture;
using namespace material_ir_evaluator;

namespace {

constexpr Guid AtlasId{0x41544c53ull, 1};
// 공급이 atlas로 주지 않는 id다(atlas가 아닌 에셋 — 텍스처 사실은 Cube).
constexpr Guid NotAtlasId{0x41544c53ull, 2};
// atlas snapshot은 있지만 텍스처 사실이 없는 id다.
constexpr Guid FactlessAtlasId{0x41544c53ull, 3};
constexpr Guid FirstCurveId{0x43555256ull, 1};
constexpr Guid SecondCurveId{0x43555256ull, 2};
// atlas에 없는 커브다.
constexpr Guid AbsentCurveId{0x43555256ull, 9};
constexpr Guid RowParameterId{0x524f5750ull, 1};
// 행: 0 첫 커브, 1 null, 2·3 둘째 커브(중복 — 첫 행이 답이다, UE GetCurveIndex).
const Guid AtlasRows[] = {FirstCurveId, Guid{}, SecondCurveId, SecondCurveId};
constexpr usize SecondCurveRow = 2;
// custom primitive data 행 번호 칸이다.
constexpr uint8 RowDataIndex = 5;
// 시간 입력 sin(값)의 값이다.
constexpr float TimeSeed = 0.3f;
// uv 식의 픽셀 중심 오프셋이다(UE Constant(0.5)).
constexpr float PixelCenter = 0.5f;

// atlas 둘(하나는 텍스처 사실 없음)을 공급하는 provider다.
class AtlasSources final : public MaterialSourceProvider {
public:
    const MaterialGraphSnapshot* FindFunction(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialGraphSnapshot* FindMaterial(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialCurveAtlasSnapshot* FindCurveAtlas(Guid asset) const noexcept override {
        return asset == AtlasId || asset == FactlessAtlasId ? &snapshot_ : nullptr;
    }

private:
    MaterialCurveAtlasSnapshot snapshot_{AtlasRows};
};

// atlas 텍스처 사실이다(UE 생성자 값 — sRGB 아님, HDR 압축).
MaterialTextureFacts AtlasFacts() noexcept {
    MaterialTextureFacts facts;
    facts.asset = AtlasId;
    facts.kind = IrTextureKind::Texture2D;
    facts.compression = TextureCompressionSettings::HDR;
    facts.srgb = false;
    return facts;
}

MaterialTextureFacts CubeFacts() noexcept {
    MaterialTextureFacts facts;
    facts.asset = NotAtlasId;
    facts.kind = IrTextureKind::TextureCube;
    return facts;
}

const MaterialTextureFacts AtlasTextures[] = {AtlasFacts(), CubeFacts()};

struct RowSettings {
    Guid curve;
    Guid atlas;
    bool customPrimitiveData = false;
};

// 노드를 더한다(출력은 잇지 않는다).
Guid AddRowNode(GraphFixture& fixture, const RowSettings& settings) noexcept {
    CurveAtlasRowParameterProperties properties;
    properties.parameterName = fixture.env.Intern("Row");
    properties.parameterId = RowParameterId;
    properties.curve.assetId = AssetId{settings.curve};
    properties.atlas.assetId = AssetId{settings.atlas};
    properties.useCustomPrimitiveData = settings.customPrimitiveData;
    properties.primitiveDataIndex = RowDataIndex;
    return fixture.Add("Material.CurveAtlasRowParameter", fixture.env.Save(properties));
}

// 노드를 더하고 R을 AO에 잇는다.
Guid AddRow(GraphFixture& fixture, const RowSettings& settings) noexcept {
    const Guid node = AddRowNode(fixture, settings);
    fixture.Connect(node, "R", OutputNodeId, "AmbientOcclusion");
    return node;
}

bool TranslateRow(GraphFixture& fixture, const MaterialSourceProvider& sources,
                  MaterialTranslation& out, const MaterialTraitsDesc& traits = Traits()) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   traits,           TestAssetId,         {}};
    input.textures = AtlasTextures;
    input.sources = &sources;
    return fixture.ready && TranslateMaterial(fixture.env.allocator, input, out).IsOk();
}

bool HasMessagePrefix(const MaterialTranslation& out, StringView prefix) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        const StringView message = diagnostic.message.View();
        if (message.ByteCount() >= prefix.ByteCount() &&
            StringView(message.Data(), prefix.ByteCount()) == prefix) {
            return true;
        }
    }

    return false;
}

const IrValue& ValueOf(const MaterialTranslation& out, IrValueId id) noexcept {
    return out.module.Values()[id];
}

const IrValue* FindOp(const MaterialTranslation& out, IrOp op) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == op) {
            return &value;
        }
    }

    return nullptr;
}

// 상수 값의 첫 성분이다(계산하지 못하면 NaN 대신 false).
bool FirstComponent(const MaterialTranslation& out, IrValueId id, float& value) noexcept {
    EvaluatedIr result;
    if (!EvaluateIr(out, id, result) || result.count == 0) {
        return false;
    }

    value = result.values[0];
    return true;
}

} // namespace

// 행 번호는 atlas의 첫 curve 행을 기본값으로 하는 scalar 파라미터이고, atlas를 공유 Clamp
// LinearColor로 uv = (Div(Add(Mul(t, Sub(W, 1)), 0.5), W), Div(Add(행, 0.5), H))에서 샘플한다(UE
// 연산 순서). atlas는 번역 의존 CurveAtlas다.
TEST(MaterialCurveAtlas_RowParameterSamplesAtlas) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const AtlasSources sources;
    GraphFixture fixture(env);
    const Guid node = AddRow(fixture, RowSettings{SecondCurveId, AtlasId});
    fixture.Connect(AddVarying(fixture, TimeSeed), "Result", node, "InputTime");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateRow(fixture, sources, out));
    REQUIRE(out.succeeded);

    REQUIRE(out.module.Parameters().Size() == 1);
    const IrParameter& row = out.module.Parameters()[0];
    CHECK(row.id == RowParameterId);
    CHECK(row.kind == IrParameterKind::Scalar);
    CHECK(row.defaults[0] == static_cast<double>(SecondCurveRow));

    const IrValue* sample = FindOp(out, IrOp::TextureSample);
    REQUIRE(sample != nullptr);
    CHECK(sample->immediate[0] == static_cast<uint64>(SamplerType::LinearColor));
    // 픽셀·Surface·2D지만 view mip bias가 없다(UE 마지막 인자 false — Bias가 아니라 Implicit).
    CHECK(sample->immediate[1] == static_cast<uint64>(IrTextureMipMode::Implicit));
    CHECK(sample->immediate[2] == static_cast<uint64>(IrSamplerSource::SharedClamp));

    const IrValue& uv = ValueOf(out, sample->operands[1]);
    REQUIRE(uv.op == IrOp::Append);
    const IrValue& u = ValueOf(out, uv.operands[0]);
    const IrValue& v = ValueOf(out, uv.operands[1]);
    REQUIRE(u.op == IrOp::Divide);
    REQUIRE(v.op == IrOp::Divide);
    const IrValue& centered = ValueOf(out, u.operands[0]);
    REQUIRE(centered.op == IrOp::Add);
    const IrValue& scaled = ValueOf(out, centered.operands[0]);
    REQUIRE(scaled.op == IrOp::Multiply);
    CHECK(ValueOf(out, scaled.operands[0]).op == IrOp::Sin);
    const IrValue& last = ValueOf(out, scaled.operands[1]);
    REQUIRE(last.op == IrOp::Subtract);
    CHECK(last.operands[0] == u.operands[1]);
    float offset = 0.0f;
    REQUIRE(FirstComponent(out, centered.operands[1], offset));
    CHECK(offset == PixelCenter);
    const IrValue& rowCenter = ValueOf(out, v.operands[0]);
    REQUIRE(rowCenter.op == IrOp::Add);
    CHECK(ValueOf(out, rowCenter.operands[0]).op == IrOp::Parameter);
    // W·H는 같은 TextureSize의 다른 성분이다.
    const IrValue& width = ValueOf(out, u.operands[1]);
    const IrValue& height = ValueOf(out, v.operands[1]);
    REQUIRE(width.op == IrOp::Swizzle);
    REQUIRE(height.op == IrOp::Swizzle);
    CHECK(width.operands[0] == height.operands[0]);
    CHECK(ValueOf(out, width.operands[0]).op == IrOp::TextureSize);
    CHECK(u.operands[1] != v.operands[1]);

    REQUIRE(out.module.Textures().Size() == 1);
    CHECK(out.module.Textures()[0].id == AtlasId);
    CHECK(out.module.Textures()[0].sampler == IrTextureSampler::SharedClamp);
    bool recorded = false;
    for (const MaterialTranslationDependency& dependency : out.dependencies) {
        recorded = recorded || (dependency.role == MaterialDependencyRole::CurveAtlas &&
                                dependency.asset == AtlasId);
    }

    CHECK(recorded);

    // InputTime이 없으면 t = 0이다(UE Constant(0)) — Multiply의 왼쪽이 상수 0이다.
    GraphFixture idle(env);
    (void)AddRow(idle, RowSettings{FirstCurveId, AtlasId});
    MaterialTranslation idleOut(env.allocator);
    REQUIRE(TranslateRow(idle, sources, idleOut));
    REQUIRE(idleOut.succeeded);
    REQUIRE(idleOut.module.Parameters().Size() == 1);
    CHECK(idleOut.module.Parameters()[0].defaults[0] == 0.0);
    const IrValue* multiply = FindOp(idleOut, IrOp::Multiply);
    REQUIRE(multiply != nullptr);
    float zero = 1.0f;
    REQUIRE(FirstComponent(idleOut, multiply->operands[0], zero));
    CHECK(zero == 0.0f);
}

// Result는 샘플의 RGB(UE 출력 0 마스크 1,1,1,1,0)이고, 정점 문맥은 mip Level 0이다(UE Constant(0)).
TEST(MaterialCurveAtlas_ResultIsRgbAndVertexLevelZero) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const AtlasSources sources;
    GraphFixture fixture(env);
    const Guid node = AddRowNode(fixture, RowSettings{FirstCurveId, AtlasId});
    fixture.Connect(node, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateRow(fixture, sources, out));
    REQUIRE(out.succeeded);
    bool level = false;
    bool rgb = false;
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::TextureSample &&
            value.immediate[1] == static_cast<uint64>(IrTextureMipMode::Level)) {
            float mip = 1.0f;
            level = FirstComponent(out, value.operands[2], mip) && mip == 0.0f;
        }

        if (value.op == IrOp::Swizzle && value.type == IrFloat(3) &&
            ValueOf(out, value.operands[0]).op == IrOp::TextureSample) {
            rgb = true;
        }
    }

    CHECK(level);
    CHECK(rgb);
}

// custom primitive data면 행 번호가 그 칸이고 파라미터를 만들지 않는다(curve가 없거나 atlas에
// 없어도 된다). UI 영역이면 UE 문구다.
TEST(MaterialCurveAtlas_CustomPrimitiveDataRow) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const AtlasSources sources;
    GraphFixture fixture(env);
    RowSettings settings{Guid{}, AtlasId};
    settings.customPrimitiveData = true;
    (void)AddRow(fixture, settings);
    MaterialTranslation out(env.allocator);
    REQUIRE(TranslateRow(fixture, sources, out));
    REQUIRE(out.succeeded);
    CHECK(out.module.Parameters().IsEmpty());
    CHECK(FindOp(out, IrOp::TextureSample) != nullptr);

    GraphFixture absent(env);
    RowSettings absentSettings{AbsentCurveId, AtlasId};
    absentSettings.customPrimitiveData = true;
    (void)AddRow(absent, absentSettings);
    MaterialTranslation absentOut(env.allocator);
    REQUIRE(TranslateRow(absent, sources, absentOut));
    CHECK(absentOut.succeeded);

    GraphFixture ui(env);
    const Guid uiNode = AddRowNode(ui, settings);
    ui.Connect(uiNode, "R", OutputNodeId, "Opacity");
    MaterialTranslation uiOut(env.allocator);
    REQUIRE(TranslateRow(ui, sources, uiOut,
                         Traits(BlendMode::AlphaComposite, MaterialDomain::UserInterface)));
    CHECK(!uiOut.succeeded);
    CHECK(HasMessagePrefix(uiOut,
                           "Custom Primitive Data can't be used with the UI material domain."));
}

// UE 검사 순서·문구와 공급에 없는 atlas, atlas에 없는 커브, 2D 아닌 atlas, 사실 없는 atlas다(모두
// 노드 위치의 InvalidProperty).
TEST(MaterialCurveAtlas_Diagnostics) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const AtlasSources sources;
    struct Case {
        RowSettings settings;
        StringView message;
    };

    const Case cases[] = {
        {RowSettings{Guid{}, Guid{}}, "The curve and atlas are not currently set."},
        {RowSettings{Guid{}, Guid{}, true}, "The atlas are not currently set."},
        {RowSettings{Guid{}, AtlasId}, "The curve is not currently set."},
        {RowSettings{FirstCurveId, Guid{}}, "The atlas is not currently set."},
        {RowSettings{FirstCurveId, Guid{}, true}, "The atlas is not currently set."},
        {RowSettings{FirstCurveId, NotAtlasId}, "Could not resolve referenced curve atlas '"},
        {RowSettings{FirstCurveId, NotAtlasId, true}, "The atlas is not a 2D texture."},
        {RowSettings{FirstCurveId, FactlessAtlasId}, "Could not resolve referenced texture '"},
        {RowSettings{AbsentCurveId, AtlasId}, "The curve is not contained within the atlas."}};
    for (const Case& item : cases) {
        GraphFixture fixture(env);
        const Guid node = AddRow(fixture, item.settings);
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateRow(fixture, sources, out));
        CHECK(!out.succeeded);
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::InvalidProperty, node));
        CHECK(HasMessagePrefix(out, item.message));
    }
}

} // namespace material_curve_atlas_tests_file
