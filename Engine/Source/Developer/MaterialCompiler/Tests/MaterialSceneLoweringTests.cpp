#include <MaterialSchema/MaterialContextValue.h>
#include <MaterialSchema/Nodes/Decals/DecalBufferTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Decals/DecalBufferTextureProperties.h>
#include <MaterialSchema/Nodes/Depth/SceneDepthProperties.generated.h>
#include <MaterialSchema/Nodes/Depth/SceneDepthProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Texture/SceneColorProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SceneColorProperties.h>
#include <MaterialSchema/Nodes/Texture/SceneTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/SceneTextureProperties.h>
#include <MaterialSchema/Nodes/Texture/UserSceneTextureProperties.generated.h>
#include <MaterialSchema/Nodes/Texture/UserSceneTextureProperties.h>
#include <MaterialSchema/Nodes/Water/SceneDepthWithoutWaterProperties.generated.h>
#include <MaterialSchema/Nodes/Water/SceneDepthWithoutWaterProperties.h>

#include "MaterialGraphFixture.h"

// MS2c4 시험이다(Docs/Design/MaterialSystem 6.10절 scene·깊이·버퍼): 화면 uv 세 갈래(UE
// GetScreenAlignedUV), scene depth·scene color·scene texture의 영역·블렌드 검사(UE 문구 — 노드
// 위치), Color 출력이 쓰일 때만 lookup, post process 입력의 pre-exposure, UserSceneTexture
// 이름·상한, DepthFade 식, SceneDepthWithoutWater·DBuffer·path tracing 버퍼 검사를 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_scene_lowering_tests_file {

using namespace material_graph_fixture;

namespace {

uint64 ContextIndex(StringView name) noexcept {
    const Span<const MaterialContextValue> table = MaterialContextValues();
    for (usize index = 0; index < table.Size(); ++index) {
        if (table[index].name == name) {
            return index;
        }
    }

    return table.Size();
}

const IrValue* FindExtern(const MaterialTranslation& out, StringView name) noexcept {
    for (const IrValue& value : out.module.Values()) {
        if (value.op == IrOp::Extern && value.immediate[0] == ContextIndex(name)) {
            return &value;
        }
    }

    return nullptr;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

MaterialTraitsDesc Translucent(MaterialDomain domain = MaterialDomain::Surface) noexcept {
    return Traits(BlendMode::AlphaComposite, domain);
}

// node.pin의 첫 성분을 Opacity(opacity — AlphaComposite)나 AO(불투명)에 잇는다.
void ToOutput(GraphFixture& fixture, Guid node, StringView pin, bool opacity) noexcept {
    ComponentMaskProperties properties;
    properties.r = true;
    const Guid mask = fixture.Add("Material.ComponentMask", fixture.env.Save(properties));
    fixture.Connect(node, pin, mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, opacity ? "Opacity" : "AmbientOcclusion");
}

bool Run(GraphFixture& fixture, const MaterialTraitsDesc& traits, MaterialTranslation& out,
         const MaterialTranslationTarget& target = {}) noexcept {
    return fixture.ready && Translate(fixture, traits, out, target).IsOk();
}

Guid AddSceneDepth(GraphFixture& fixture,
                   SceneAttributeInputMode mode = SceneAttributeInputMode::Coordinates) noexcept {
    SceneDepthProperties properties;
    properties.inputMode = mode;
    return fixture.Add("Material.SceneDepth", fixture.env.Save(properties));
}

Guid AddSceneTexture(GraphFixture& fixture, SceneTextureId id) noexcept {
    SceneTextureProperties properties;
    properties.sceneTextureId = id;
    return fixture.Add("Material.SceneTexture", fixture.env.Save(properties));
}

} // namespace

// 화면 uv: 연결 없음은 ScreenAlignedUv, Coordinates 연결은 BufferUvFromViewportUv, OffsetFraction은
// 핀 값(ConstInput)의 ScreenUvFromOffsetFraction이다.
TEST(MaterialScene_ScreenUvModes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture plain(env);
    ToOutput(plain, AddSceneDepth(plain), "Result", true);
    MaterialTranslation standard(env.allocator);
    REQUIRE(Run(plain, Translucent(), standard));
    REQUIRE(standard.succeeded);
    const IrValue* depth = FindExtern(standard, "SceneDepthAt");
    REQUIRE(depth != nullptr);
    CHECK(ValueOf(standard, depth->operands[0]).immediate[0] == ContextIndex("ScreenAlignedUv"));

    GraphFixture coordinates(env);
    const Guid node = AddSceneDepth(coordinates);
    coordinates.Connect(coordinates.Add("Material.TextureCoordinate"), "Result", node, "Input");
    ToOutput(coordinates, node, "Result", true);
    MaterialTranslation viewport(env.allocator);
    REQUIRE(Run(coordinates, Translucent(), viewport));
    REQUIRE(viewport.succeeded);
    CHECK(FindExtern(viewport, "BufferUvFromViewportUv") != nullptr);

    GraphFixture offset(env);
    ToOutput(offset, AddSceneDepth(offset, SceneAttributeInputMode::OffsetFraction), "Result",
             true);
    MaterialTranslation fraction(env.allocator);
    REQUIRE(Run(offset, Translucent(), fraction));
    REQUIRE(fraction.succeeded);
    const IrValue* moved = FindExtern(fraction, "ScreenUvFromOffsetFraction");
    REQUIRE(moved != nullptr);
    CHECK(ValueOf(fraction, moved->operands[0]).op == IrOp::Constant);
}

// scene depth: 불투명 Surface는 UE 문구의 오류, 반투명 + Output Velocity도 오류다.
TEST(MaterialScene_SceneDepthChecks) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture opaque(env);
    const Guid node = AddSceneDepth(opaque);
    ToOutput(opaque, node, "Result", false);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Run(opaque, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, node));
    CHECK(HasMessage(rejected,
                     "Only transparent or postprocess materials can read from scene depth."));

    GraphFixture velocity(env);
    const Guid writer = AddSceneDepth(velocity);
    ToOutput(velocity, writer, "Result", true);
    MaterialTraitsDesc traits = Translucent();
    traits.outputTranslucentVelocity = true;
    MaterialTranslation conflict(env.allocator);
    REQUIRE(Run(velocity, traits, conflict));
    CHECK(HasMessage(conflict, "Translucenct material with 'Output Velocity' enabled will write to "
                               "depth buffer, therefore cannot read from depth buffer at the same "
                               "time."));
}

// DepthFade: opacity × saturate((scene − pixel) / max(fade, δ)), 모바일은 pixel depth를 자른다.
TEST(MaterialScene_DepthFade) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.DepthFade");
    fixture.Connect(node, "Result", OutputNodeId, "Opacity");
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(fixture, Translucent(), out));
    REQUIRE(out.succeeded);
    const IrResult* opacity = FindResult(out, "Opacity");
    REQUIRE(opacity != nullptr);
    const IrValue& product = ValueOf(out, opacity->value);
    CHECK(product.op == IrOp::Multiply || product.op == IrOp::Saturate);
    CHECK(FindExtern(out, "PixelDepth") != nullptr);
    CHECK(FindExtern(out, "SceneDepthAt") != nullptr);
    bool clamped = false;
    for (const IrValue& value : out.module.Values()) {
        clamped = clamped || value.op == IrOp::Min;
    }

    CHECK(!clamped);
    MaterialTranslationTarget mobile;
    mobile.isMobile = true;
    mobile.isPc = false;
    MaterialTranslation small(env.allocator);
    REQUIRE(Run(fixture, Translucent(), small, mobile));
    REQUIRE(small.succeeded);
    bool mobileClamp = false;
    for (const IrValue& value : small.module.Values()) {
        mobileClamp = mobileClamp || value.op == IrOp::Min;
    }

    CHECK(mobileClamp);
}

// SceneColor: 반투명 Surface 픽셀만. 불투명은 FinalCompileValidation 문구, PostProcess는 영역 문구,
// 정점 문맥은 NonPixel 문구다.
TEST(MaterialScene_SceneColor) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    SceneColorProperties properties;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.SceneColor", env.Save(properties));
    fixture.Connect(node, "A", OutputNodeId, "Opacity");
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(fixture, Translucent(), out));
    REQUIRE(out.succeeded);
    CHECK(FindExtern(out, "SceneColorAt") != nullptr);

    GraphFixture opaque(env);
    const Guid solid = opaque.Add("Material.SceneColor", env.Save(properties));
    ToOutput(opaque, solid, "RGB", false);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Run(opaque, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, solid));
    CHECK(HasMessage(rejected, "Only translucent materials can use the scene color node."));

    GraphFixture post(env);
    const Guid late = post.Add("Material.SceneColor", env.Save(properties));
    post.Connect(late, "A", OutputNodeId, "Opacity");
    MaterialTranslation domain(env.allocator);
    REQUIRE(Run(post, Translucent(MaterialDomain::PostProcess), domain));
    CHECK(
        HasMessage(domain, "SceneColor lookups are only available when MaterialDomain = Surface."));
    CHECK(HasMessage(domain, "Only 'surface' material domain can use the scene color node."));

    GraphFixture vertex(env);
    const Guid early = vertex.Add("Material.SceneColor", env.Save(properties));
    vertex.Connect(early, "RGB", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation stage(env.allocator);
    REQUIRE(Run(vertex, Translucent(), stage));
    CHECK(HasOnlyErrorAt(stage, MaterialDiagnosticCode::StageViolation, early));
    CHECK(HasMessage(stage, "Invalid node used in vertex/hull/domain shader input!"));
}

// SceneTexture: 불투명 Surface의 Color lookup은 UE 문구, Size만 쓰면 검사 없음, 반투명의 post
// process 입력·DecalMask·decal 허용 id·Velocity 문구.
TEST(MaterialScene_SceneTextureChecks) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture opaque(env);
    const Guid color = AddSceneTexture(opaque, SceneTextureId::BaseColor);
    ToOutput(opaque, color, "Color", false);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Run(opaque, Traits(), rejected));
    CHECK(HasOnlyErrorAt(rejected, MaterialDiagnosticCode::UnsupportedCapability, color));
    CHECK(HasMessage(rejected, "SceneTexture expressions cannot be used in opaque materials except "
                               "if used with the Single Layer Water shading model."));

    GraphFixture sizeOnly(env);
    const Guid size = AddSceneTexture(sizeOnly, SceneTextureId::BaseColor);
    ToOutput(sizeOnly, size, "Size", false);
    MaterialTranslation measured(env.allocator);
    REQUIRE(Run(sizeOnly, Traits(), measured));
    CHECK(measured.succeeded);
    CHECK(FindExtern(measured, "SceneTextureViewSize") != nullptr);
    CHECK(FindExtern(measured, "SceneTextureLookup") == nullptr);

    GraphFixture inputs(env);
    const Guid input = AddSceneTexture(inputs, SceneTextureId::PostProcessInput0);
    ToOutput(inputs, input, "Color", true);
    MaterialTranslation translucent(env.allocator);
    REQUIRE(Run(inputs, Translucent(), translucent));
    CHECK(HasMessage(translucent,
                     "SceneTexture expressions cannot use post process inputs or scene "
                     "color in non post process domain materials"));

    GraphFixture mask(env);
    const Guid decalMask = AddSceneTexture(mask, SceneTextureId::DecalMask);
    ToOutput(mask, decalMask, "Color", true);
    MaterialTranslation moved(env.allocator);
    REQUIRE(Run(mask, Translucent(MaterialDomain::PostProcess), moved));
    CHECK(HasMessage(moved, "Decal Mask bit was moved from GBuffer to the Stencil Buffer for "
                            "performance optimisation so therefore no longer available."));

    GraphFixture decal(env);
    const Guid gbuffer = AddSceneTexture(decal, SceneTextureId::BaseColor);
    ToOutput(decal, gbuffer, "Color", true);
    MaterialTranslation decalRejected(env.allocator);
    REQUIRE(Run(decal, Translucent(MaterialDomain::DeferredDecal), decalRejected));
    CHECK(HasMessage(
        decalRejected,
        "Decals can only access SceneDepth, CustomDepth, CustomStencil, and WorldNormal."));

    GraphFixture motion(env);
    const Guid velocity = AddSceneTexture(motion, SceneTextureId::Velocity);
    ToOutput(motion, velocity, "Color", true);
    MaterialTranslation notPost(env.allocator);
    REQUIRE(Run(motion, Translucent(), notPost));
    CHECK(HasMessage(notPost,
                     "Velocity scene textures are only available in post process materials."));
}

// post process 입력 lookup은 blendable location이 tonemap 뒤가 아니면 1/pre-exposure를 곱한다.
TEST(MaterialScene_PreExposure) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid input = AddSceneTexture(fixture, SceneTextureId::PostProcessInput0);
    ToOutput(fixture, input, "Color", true);
    MaterialTraitsDesc before = Translucent(MaterialDomain::PostProcess);
    before.postProcess.location = BlendableLocation::SceneColorBeforeDepthOfField;
    MaterialTranslation scaled(env.allocator);
    REQUIRE(Run(fixture, before, scaled));
    REQUIRE(scaled.succeeded);
    CHECK(FindExtern(scaled, "OneOverPreExposure") != nullptr);
    CHECK(FindExtern(scaled, "SceneTextureLookup") != nullptr);

    MaterialTranslation after(env.allocator);
    REQUIRE(Run(fixture, Translucent(MaterialDomain::PostProcess), after));
    REQUIRE(after.succeeded);
    CHECK(FindExtern(after, "OneOverPreExposure") == nullptr);
}

// UserSceneTexture: 이름이 없으면 UE 문구, 서로 다른 이름 다섯까지(여섯째는 V9), 이름 목록과 id.
TEST(MaterialScene_UserSceneTexture) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture empty(env);
    UserSceneTextureProperties none;
    const Guid lonely = empty.Add("Material.UserSceneTexture", env.Save(none));
    ToOutput(empty, lonely, "Color", true);
    MaterialTranslation missing(env.allocator);
    REQUIRE(Run(empty, Translucent(MaterialDomain::PostProcess), missing));
    CHECK(HasMessage(missing,
                     "UserSceneTexture missing name -- value must be set to something other "
                     "than None"));

    GraphFixture fixture(env);
    const StringView names[] = {"Bloom", "Glow", "Mask", "Edge", "Blur", "Extra"};
    Guid previous{};
    Guid last{};
    for (usize index = 0; index < 6; ++index) {
        UserSceneTextureProperties properties;
        properties.userSceneTexture = env.Intern(names[index]);
        last = fixture.Add("Material.UserSceneTexture", env.Save(properties));
        if (index == 0) {
            previous = last;
            continue;
        }

        const Guid sum = fixture.Add("Material.Add");
        fixture.Connect(previous, index == 1 ? StringView("Color") : StringView("Result"), sum,
                        "A");
        fixture.Connect(last, "Color", sum, "B");
        previous = sum;
    }

    ToOutput(fixture, previous, "Result", true);
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(fixture, Translucent(MaterialDomain::PostProcess), out));
    CHECK(!out.succeeded);
    CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::BudgetExceeded, last));
    CHECK(HasMessage(out, "Too many unique UserSceneTexture inputs in the post process material -- "
                          "max allowed is 5"));
    CHECK(out.userSceneTextures.Size() == 5);
}

// SceneDepthWithoutWater: PostProcess는 된다. Surface는 Single Layer Water가 없어 UE 문구, 정점
// 문맥도 오류다.
TEST(MaterialScene_SceneDepthWithoutWater) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    SceneDepthWithoutWaterProperties properties;
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.SceneDepthWithoutWater", env.Save(properties));
    ToOutput(fixture, node, "Result", true);
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(fixture, Translucent(MaterialDomain::PostProcess), out));
    REQUIRE(out.succeeded);
    const IrValue* depth = FindExtern(out, "SceneDepthWithoutWaterAt");
    REQUIRE(depth != nullptr);
    CHECK(FloatOf(ValueOf(out, depth->operands[1]), 0) == 1000000.0f);

    MaterialTranslation surface(env.allocator);
    REQUIRE(Run(fixture, Translucent(), surface));
    CHECK(HasMessage(surface, "Can only read scene depth below water when material Shading Model "
                              "is Single Layer Water or when material Domain is PostProcess."));

    GraphFixture vertex(env);
    const Guid early = vertex.Add("Material.SceneDepthWithoutWater", env.Save(properties));
    vertex.Connect(early, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation stage(env.allocator);
    REQUIRE(Run(vertex, Translucent(), stage));
    CHECK(HasMessage(stage, "Cannot read scene depth without water from the vertex shader."));
}

// DecalBufferTexture는 불투명·Masked Surface만, PathTracingBufferTexture는 PostProcess만이다.
TEST(MaterialScene_BufferTextures) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    DecalBufferTextureProperties dbuffer;
    dbuffer.dBufferTextureId = DecalBufferTextureId::B;
    GraphFixture opaque(env);
    const Guid decal = opaque.Add("Material.DecalBufferTexture", env.Save(dbuffer));
    ToOutput(opaque, decal, "RGBA", false);
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(opaque, Traits(), out));
    REQUIRE(out.succeeded);
    const IrValue* lookup = FindExtern(out, "DBufferTextureLookup");
    REQUIRE(lookup != nullptr);
    CHECK(ValueOf(out, lookup->operands[1]).immediate[0] == 1);

    GraphFixture translucent(env);
    const Guid wrong = translucent.Add("Material.DecalBufferTexture", env.Save(dbuffer));
    ToOutput(translucent, wrong, "RGBA", true);
    MaterialTranslation rejected(env.allocator);
    REQUIRE(Run(translucent, Translucent(), rejected));
    CHECK(HasMessage(rejected,
                     "DBuffer scene textures are only available on opaque or masked surfaces."));

    GraphFixture path(env);
    const Guid buffer = path.Add("Material.PathTracingBufferTexture");
    ToOutput(path, buffer, "RGBA", false);
    MaterialTranslation surface(env.allocator);
    REQUIRE(Run(path, Traits(), surface));
    CHECK(HasMessage(surface,
                     "Path tracing buffer textures are only available on post process material."));

    GraphFixture post(env);
    const Guid ok = post.Add("Material.PathTracingBufferTexture");
    ToOutput(post, ok, "RGBA", true);
    MaterialTranslation postOut(env.allocator);
    REQUIRE(Run(post, Translucent(MaterialDomain::PostProcess), postOut));
    REQUIRE(postOut.succeeded);
    CHECK(FindExtern(postOut, "PathTracingBufferDefaultUv") != nullptr);
}

// 지연 진단: 결과에 닿지 않는 SceneTexture Color 간선은 불투명 Surface에서도 오류가 없다(UE는
// 쓰이는 출력만 컴파일한다).
TEST(MaterialScene_UnreachedColorIsNotChecked) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid node = AddSceneTexture(fixture, SceneTextureId::BaseColor);
    const Guid dangling = fixture.Add("Material.Multiply");
    fixture.Connect(node, "Color", dangling, "A");
    fixture.Connect(AddConstant(fixture, 0.5f), "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Run(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(CountErrors(out) == 0);
}

} // namespace material_scene_lowering_tests_file
