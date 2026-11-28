#include <MaterialCompiler/MaterialShaderTraitsBuilder.h>
#include <MaterialSchema/MaterialShaderTraits.h>

#include "MaterialGraphFixture.h"
#include "MaterialMathGraph.h"

// 머티리얼 셰이더 특성 시험이다(Docs/Design/MaterialSystem 8.3절 "유도 규칙"·"정규 바이트" — MS3c):
// Math 그래프(WPO·Masked·PDO)의 유도 값, 반투명 velocity·그림자 규칙, 정규 바이트 왕복과 손상
// 판정을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_shader_traits_tests_file {

using namespace material_graph_fixture;

// Math 그래프는 WPO·PDO·OpacityMask를 쓰고 Masked라 그림자를 드리운다. 정점 문맥 기능이 있다.
TEST(MaterialShaderTraits_DerivedFromMathGraph) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    BuildMathGraph(fixture);
    const MaterialTraitsDesc desc = Traits(BlendMode::Masked);
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, desc, out));
    REQUIRE(out.succeeded);
    MaterialShaderTraits traits(env.allocator);
    REQUIRE(MakeMaterialShaderTraits(out, desc, MaterialQualityLevel::High, traits));
    CHECK(traits.blendMode == BlendMode::Masked);
    CHECK(traits.usesWpo);
    CHECK(traits.usesPdo);
    CHECK(traits.usesOpacityMask);
    CHECK(traits.castsShadow);
    // 불투명 계열은 WPO가 있으면 velocity를 쓴다.
    CHECK(traits.writesVelocity);
    // Math 그래프는 품질 칸을 읽지 않으므로 품질은 None이다(8.3절 — 수준 공유).
    CHECK(traits.quality == MaterialQualityLevel::None);
    CHECK(traits.shadingModels == out.shadingModels);

    // 기본 표면(출력만)은 WPO·PDO가 없다(결과가 없거나 상수 0).
    GraphFixture plain(env);
    MaterialTranslation plainOut(env.allocator);
    REQUIRE(Translate(plain, Traits(), plainOut));
    REQUIRE(plainOut.succeeded);
    MaterialShaderTraits plainTraits(env.allocator);
    REQUIRE(MakeMaterialShaderTraits(plainOut, Traits(), MaterialQualityLevel::None, plainTraits));
    CHECK(!plainTraits.usesWpo);
    CHECK(!plainTraits.usesPdo);
    CHECK(!plainTraits.usesOpacityMask);
    CHECK(!plainTraits.writesVelocity);

    // 반투명은 velocity가 명시 표지, 그림자는 castDynamicShadowAsMasked일 때만이다.
    MaterialTraitsDesc translucent = Traits(BlendMode::Translucent);
    translucent.translucency.pass = TranslucencyPass::AfterMotionBlur;
    GraphFixture glass(env);
    MaterialTranslation glassOut(env.allocator);
    REQUIRE(Translate(glass, translucent, glassOut));
    REQUIRE(glassOut.succeeded);
    MaterialShaderTraits glassTraits(env.allocator);
    REQUIRE(
        MakeMaterialShaderTraits(glassOut, translucent, MaterialQualityLevel::None, glassTraits));
    CHECK(!glassTraits.writesVelocity);
    CHECK(!glassTraits.castsShadow);
    CHECK(glassTraits.translucencyPass == TranslucencyPass::AfterMotionBlur);
    translucent.outputTranslucentVelocity = true;
    translucent.castDynamicShadowAsMasked = true;
    REQUIRE(
        MakeMaterialShaderTraits(glassOut, translucent, MaterialQualityLevel::None, glassTraits));
    CHECK(glassTraits.writesVelocity);
    CHECK(glassTraits.castsShadow);

    // 실패한 번역은 거부한다.
    MaterialTranslation failed(env.allocator);
    CHECK(MakeMaterialShaderTraits(failed, desc, MaterialQualityLevel::None, traits).Error() ==
          ErrorCode::InvalidArgument);
}

// 정규 바이트 왕복과 손상(버전·열거 범위·불 값·길이) 판정이다.
TEST(MaterialShaderTraits_CanonicalBytes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    MaterialShaderTraits traits(env.allocator);
    traits.domain = MaterialDomain::PostProcess;
    traits.blendMode = BlendMode::AlphaComposite;
    traits.twoSided = true;
    traits.shadingModels = 0x5;
    traits.translucencyPass = TranslucencyPass::BeforeDepthOfField;
    traits.usesPdo = true;
    traits.vertexFeatures = 0x3;
    traits.pixelFeatures = 0x10;
    traits.minimumTier = MaterialCapabilityTier::Sm6;
    traits.quality = MaterialQualityLevel::Epic;
    String bent(env.allocator);
    REQUIRE(bent.TryAssign("BentNormal"));
    REQUIRE(traits.customOutputs.TryPushBack(Move(bent)));

    Array<uint8> bytes(env.allocator);
    REQUIRE(WriteMaterialShaderTraits(traits, bytes));
    MaterialShaderTraits read(env.allocator);
    usize consumed = 0;
    REQUIRE(ReadMaterialShaderTraits(bytes.View(), read, consumed));
    CHECK(consumed == bytes.Size());
    Array<uint8> again(env.allocator);
    REQUIRE(WriteMaterialShaderTraits(read, again));
    CHECK(again.Size() == bytes.Size());
    for (usize at = 0; at < bytes.Size() && at < again.Size(); ++at) {
        CHECK(again[at] == bytes[at]);
    }

    REQUIRE(read.customOutputs.Size() == 1);
    CHECK(read.customOutputs[0].View() == StringView("BentNormal"));

    // 버전(0번 byte)·domain(4번)·불 값(9번)을 망가뜨리면 CorruptData, 잘린 바이트도 같다.
    const usize corruptAt[] = {0, 4, 9};
    for (usize at : corruptAt) {
        Array<uint8> broken(env.allocator);
        REQUIRE(broken.TryAppend(bytes.View()));
        broken[at] = 0xff;
        CHECK(ReadMaterialShaderTraits(broken.View(), read, consumed).Error() ==
              ErrorCode::CorruptData);
    }

    CHECK(
        ReadMaterialShaderTraits(Span<const uint8>(bytes.Data(), bytes.Size() - 1), read, consumed)
            .Error() == ErrorCode::CorruptData);
}

} // namespace material_shader_traits_tests_file
