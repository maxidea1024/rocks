#include <MaterialCompiler/MaterialSourceWriter.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/StaticBoolProperties.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/TimeProperties.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.generated.h>
#include <MaterialSchema/Nodes/Coordinates/TextureCoordinateProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>

#include "MaterialGraphFixture.h"

// MS2b4a 시험이다(Docs/Design/MaterialSystem 6.9절 보간기): VertexInterpolator는 입력을 정점
// 문맥에서 번역해 픽셀로 보간하고(정점 전용 값이 허용되고 그 기능은 Vertex 진입점 사용), 보간
// 배정은 성분을 이어 붙이며, 정점 문맥 사용·입력 없음·Bool 입력·예산 초과는 오류다. CustomizedUv는
// 픽셀이 쓰는 UV 채널 수만큼 정점 결과를 낸다(연결은 traits.customizedUvCount 안에서만).
using namespace rocks;
using namespace rocks::material;
namespace material_interpolator_tests_file {

using namespace material_graph_fixture;

namespace {

bool HasText(StringView text, StringView part) noexcept {
    for (usize at = 0; at + part.ByteCount() <= text.ByteCount(); ++at) {
        if (StringView(text.Data() + at, part.ByteCount()) == part) {
            return true;
        }
    }

    return false;
}

Guid AddMask(GraphFixture& fixture, bool r, bool g, bool b) noexcept {
    ComponentMaskProperties properties;
    properties.r = r;
    properties.g = g;
    properties.b = b;
    return fixture.Add("Material.ComponentMask", fixture.env.Save(properties));
}

bool Generate(GraphFixture& fixture, const MaterialTraitsDesc& traits, MaterialTranslation& out,
              MaterialGeneratedSource& source) noexcept {
    return Translate(fixture, traits, out).IsOk() && out.succeeded &&
           WriteMaterialSource(fixture.env.allocator, out.module, traits, TestAssetId, MainGraphId,
                               source)
               .IsOk() &&
           source.succeeded;
}

} // namespace

// 정점 전용 값(PreSkinnedPosition)을 보간해 표면에서 읽는다. 그 공급 기능은 Vertex 진입점 사용이다.
TEST(MaterialInterpolator_VertexValueInPixel) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid skinned = fixture.Add("Material.PreSkinnedPosition");
    const Guid interpolator = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(skinned, "Result", interpolator, "Input");
    const Guid mask = AddMask(fixture, true, false, false);
    fixture.Connect(interpolator, "PS", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    CHECK(out.module.EntryFeatures(IrEntry::Surface) == NoMaterialContextFeatures);
    CHECK(out.module.EntryFeatures(IrEntry::Vertex) == ToMask(MaterialContextFeature::Skin));
    CHECK(out.module.InterpolantComponents() == 3);
    const StringView text = source.text.View();
    CHECK(HasText(text, "#define MATERIAL_NUM_INTERPOLANT_VECTORS 1\n"));
    CHECK(HasText(text, "r.interpolants[0].z = m"));
    CHECK(HasText(text, "float3(c.interpolants[0].x, c.interpolants[0].y, c.interpolants[0].z)"));
    CHECK(HasText(text, "#define MATERIAL_VERTEX_USES_SKIN 1\n"));
}

// 두 보간기는 성분을 이어 붙인다(float3 다음 float2는 벡터 0의 w와 벡터 1의 x).
TEST(MaterialInterpolator_PackingAcrossVectors) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid normal = fixture.Add("Material.VertexNormal");
    const Guid first = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(normal, "Result", first, "Input");
    const Guid uv =
        fixture.Add("Material.TextureCoordinate", env.Save(TextureCoordinateProperties{}));
    const Guid second = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(uv, "Result", second, "Input");
    const Guid firstMask = AddMask(fixture, true, false, false);
    const Guid secondMask = AddMask(fixture, false, true, false);
    fixture.Connect(first, "PS", firstMask, "Input");
    fixture.Connect(second, "PS", secondMask, "Input");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(firstMask, "Result", add, "A");
    fixture.Connect(secondMask, "Result", add, "B");
    fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    CHECK(out.module.InterpolantComponents() == 5);
    CHECK(HasText(source.text.View(), "#define MATERIAL_NUM_INTERPOLANT_VECTORS 2\n"));
    CHECK(HasText(source.text.View(), "float2(c.interpolants[0].w, c.interpolants[1].x)"));
}

// 오류: 정점 문맥에서 보간 출력을 쓰면 V4, 입력이 없으면 MissingInput, Bool 입력은 V1.
TEST(MaterialInterpolator_Errors) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid interpolator = fixture.Add("Material.VertexInterpolator");
        fixture.Connect(fixture.Add("Material.VertexNormal"), "Result", interpolator, "Input");
        fixture.Connect(interpolator, "PS", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::StageViolation, interpolator));
    }

    {
        GraphFixture fixture(env);
        const Guid interpolator = fixture.Add("Material.VertexInterpolator");
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(interpolator, "PS", mask, "Input");
        fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, interpolator));
    }

    {
        GraphFixture fixture(env);
        StaticBoolProperties properties;
        properties.value = true;
        const Guid flag = fixture.Add("Material.StaticBool", env.Save(properties));
        const Guid interpolator = fixture.Add("Material.VertexInterpolator");
        fixture.Connect(flag, "Result", interpolator, "Input");
        fixture.Connect(interpolator, "PS", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::TypeMismatch, interpolator));
    }
}

// 보간 성분이 16 벡터(64 성분)를 넘으면 V9다.
TEST(MaterialInterpolator_Budget) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    constexpr usize Interpolators = 17;
    Guid previous = Guid{};
    for (usize index = 0; index < Interpolators; ++index) {
        const Guid color = fixture.Add("Material.VertexColor");
        const Guid scale = fixture.Add("Material.Multiply");
        fixture.Connect(color, "Result", scale, "A");
        fixture.Connect(AddConstant(fixture, static_cast<float>(index + 2)), "Result", scale, "B");
        const Guid append = fixture.Add("Material.Append");
        fixture.Connect(scale, "Result", append, "A");
        fixture.Connect(AddConstant(fixture, 1.0f), "Result", append, "B");
        const Guid interpolator = fixture.Add("Material.VertexInterpolator");
        fixture.Connect(append, "Result", interpolator, "Input");
        const Guid mask = AddMask(fixture, true, false, false);
        fixture.Connect(interpolator, "PS", mask, "Input");
        if (index == 0) {
            previous = mask;
            continue;
        }

        const Guid sum = fixture.Add("Material.Add");
        fixture.Connect(previous, "Result", sum, "A");
        fixture.Connect(mask, "Result", sum, "B");
        previous = sum;
    }

    fixture.Connect(previous, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(!out.succeeded);
    CHECK(HasError(out, MaterialDiagnosticCode::BudgetExceeded));
    CHECK(out.module.Values().IsEmpty());
}

// CustomizedUv: 픽셀이 쓰는 채널 수만큼 정점 결과를 낸다. customizedUvCount 안의 연결은 그 값,
// 나머지는 정점 TexCoord 그대로다. 수 밖의 연결은 V5 경고다.
TEST(MaterialInterpolator_CustomizedUvs) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    TextureCoordinateProperties properties;
    properties.coordinateIndex = 1;
    const Guid uv = fixture.Add("Material.TextureCoordinate", env.Save(properties));
    const Guid mask = AddMask(fixture, true, false, false);
    fixture.Connect(uv, "Result", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "AmbientOcclusion");
    Constant2Properties offset;
    offset.r = 0.25f;
    offset.g = 0.5f;
    const Guid custom = fixture.Add("Material.Constant2", env.Save(offset));
    fixture.Connect(custom, "Result", OutputNodeId, "CustomizedUv0");
    fixture.Connect(custom, "Result", OutputNodeId, "CustomizedUv1");
    MaterialTraitsDesc traits = Traits();
    traits.customizedUvCount = 1;
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, traits, out, source));
    CHECK(CountDiagnostics(out, MaterialDiagnosticCode::InactiveOutput,
                           nodegraph::GraphDiagnosticSeverity::Warning) == 1);
    const StringView text = source.text.View();
    CHECK(HasText(text, "#define MATERIAL_NUM_CUSTOMIZED_UVS 2\n"));
    CHECK(HasText(text, "r.customizedUvs[0] = float2(0.25, 0.5);"));
    CHECK(HasText(text, "r.customizedUvs[1] = m"));
    CHECK(HasText(text, "MaterialContextTexCoord(c, 1u)"));
}

// 리뷰 반영: 정점 함수는 WPO·CustomizedUv·보간 값이 함께 쓰는 부분식을 한 번만 선언한다.
TEST(MaterialInterpolator_SharedVertexValuesDeclaredOnce) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid time = fixture.Add("Material.Time", env.Save(TimeProperties{}));
    fixture.Connect(time, "Result", OutputNodeId, "WorldPositionOffset");
    const Guid scale = fixture.Add("Material.Multiply");
    fixture.Connect(time, "Result", scale, "A");
    fixture.Connect(AddConstant(fixture, 2.0f), "Result", scale, "B");
    const Guid interpolator = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(scale, "Result", interpolator, "Input");
    const Guid uv =
        fixture.Add("Material.TextureCoordinate", env.Save(TextureCoordinateProperties{}));
    const Guid second = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(uv, "Result", second, "Input");
    const Guid mask = AddMask(fixture, true, false, false);
    fixture.Connect(second, "PS", mask, "Input");
    const Guid add = fixture.Add("Material.Add");
    fixture.Connect(interpolator, "PS", add, "A");
    fixture.Connect(mask, "Result", add, "B");
    // 픽셀도 TexCoord(0)을 직접 읽어 CustomizedUv0 통과 결과가 생긴다.
    const Guid direct = AddMask(fixture, false, true, false);
    fixture.Connect(uv, "Result", direct, "Input");
    const Guid total = fixture.Add("Material.Add");
    fixture.Connect(add, "Result", total, "A");
    fixture.Connect(direct, "Result", total, "B");
    fixture.Connect(total, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    MaterialGeneratedSource source(env.allocator);
    REQUIRE(Generate(fixture, Traits(), out, source));
    const StringView text = source.text.View();
    // EvaluateMaterialVertex 안에서 Time·TexCoord(0)은 한 번씩만 선언된다(CustomizedUv0 통과 값과
    // 보간기 입력이 같은 TexCoord(0)이다).
    const StringView vertexHead = "void EvaluateMaterialVertex(";
    usize start = 0;
    while (start + vertexHead.ByteCount() <= text.ByteCount() &&
           !(StringView(text.Data() + start, vertexHead.ByteCount()) == vertexHead)) {
        ++start;
    }

    usize end = start;
    while (end < text.ByteCount() && text.Data()[end] != '}') {
        ++end;
    }

    const StringView body(text.Data() + start, end - start);
    const auto count = [&](StringView part) noexcept {
        usize found = 0;
        for (usize at = 0; at + part.ByteCount() <= body.ByteCount(); ++at) {
            found += StringView(body.Data() + at, part.ByteCount()) == part ? 1 : 0;
        }

        return found;
    };

    CHECK(count("= MaterialContextTime(c);") == 1);
    CHECK(count("= MaterialContextTexCoord(c, 0u);") == 1);
    CHECK(HasText(body, "r.customizedUvs[0] = m"));
}

// 리뷰 반영: 결과에서 빠진 보간기(상수 alpha Lerp의 고르지 않은 쪽)의 정점 쪽은 검사하지 않는다(DCE
// 뒤).
TEST(MaterialInterpolator_DeadInterpolatorNotChecked) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    const Guid derivative = fixture.Add("Material.Ddx");
    fixture.Connect(AddVarying(fixture, 0.5f), "Result", derivative, "Value");
    const Guid interpolator = fixture.Add("Material.VertexInterpolator");
    fixture.Connect(derivative, "Result", interpolator, "Input");
    const Guid lerp = fixture.Add("Material.LinearInterpolate");
    fixture.Connect(AddConstant(fixture, 0.25f), "Result", lerp, "A");
    fixture.Connect(interpolator, "PS", lerp, "B");
    fixture.Connect(AddConstant(fixture, 0.0f), "Result", lerp, "Alpha");
    fixture.Connect(lerp, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(fixture, Traits(), out));
    CHECK(out.succeeded);
    CHECK(CountErrors(out) == 0);
}

// 보간 배정은 간선 저장 순서와 무관하다(모듈 번호 순 — 6.6절).
TEST(MaterialInterpolator_PackingIsDeterministic) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto build = [&](GraphFixture& fixture) noexcept {
        const Guid normal = fixture.Add("Material.VertexNormal");
        const Guid first = fixture.Add("Material.VertexInterpolator");
        fixture.Connect(normal, "Result", first, "Input");
        const Guid color = fixture.Add("Material.VertexColor");
        const Guid second = fixture.Add("Material.VertexInterpolator");
        fixture.Connect(color, "Result", second, "Input");
        const Guid firstMask = AddMask(fixture, true, false, false);
        const Guid secondMask = AddMask(fixture, false, true, false);
        fixture.Connect(first, "PS", firstMask, "Input");
        fixture.Connect(second, "PS", secondMask, "Input");
        const Guid add = fixture.Add("Material.Add");
        fixture.Connect(firstMask, "Result", add, "A");
        fixture.Connect(secondMask, "Result", add, "B");
        fixture.Connect(add, "Result", OutputNodeId, "AmbientOcclusion");
    };

    GraphFixture forward(env);
    build(forward);
    GraphFixture backward(env);
    build(backward);
    nodegraph::Graph& graph = backward.Main();
    for (usize low = 0, high = graph.edges.Size() - 1; low < high; ++low, --high) {
        const nodegraph::GraphEdge swap = graph.edges[low];
        graph.edges[low] = graph.edges[high];
        graph.edges[high] = swap;
    }

    MaterialTranslation a(env.allocator);
    MaterialTranslation b(env.allocator);
    MaterialGeneratedSource sourceA(env.allocator);
    MaterialGeneratedSource sourceB(env.allocator);
    REQUIRE(Generate(forward, Traits(), a, sourceA));
    REQUIRE(Generate(backward, Traits(), b, sourceB));
    CHECK(sourceA.text.View() == sourceB.text.View());
}

} // namespace material_interpolator_tests_file
