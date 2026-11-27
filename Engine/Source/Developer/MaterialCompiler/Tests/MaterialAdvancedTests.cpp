#include <MaterialSchema/Nodes/Color/DesaturationProperties.generated.h>
#include <MaterialSchema/Nodes/Color/DesaturationProperties.h>
#include <MaterialSchema/Nodes/Math/RotateAboutAxisProperties.generated.h>
#include <MaterialSchema/Nodes/Math/RotateAboutAxisProperties.h>
#include <Platform/PlatformMath.h>

#include "MaterialGraphFixture.h"
#include "MaterialIrEvaluator.h"

// MS2f1 시험이다(Docs/Design/MaterialSystem 6.13절 MS2f1): 노드마다 상수 입력의 결과 값(UE 식을 C++
// float로 같은 순서로 계산한 기대값), 필수 입력 UE 문구, 연결 여부 분기(SphereMask Hardness·
// Desaturation Fraction·Fresnel Normal·HairColor 기본값), 형 규칙(DeriveNormalZ 스칼라·Hsv 통과·
// RotateAboutAxis Double)을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_advanced_tests_file {

using namespace material_graph_fixture;
using namespace material_ir_evaluator;

namespace {

using Math = platform::PlatformMath;

// 기대값과의 허용 오차다(IR 계산과 C++ 식의 연산 순서가 같아 거의 같다).
constexpr float AdvancedTolerance = 1e-5f;

Guid AddVector3(GraphFixture& fixture, float r, float g, float b) noexcept {
    Constant3Properties properties;
    properties.constant = LinearColor(r, g, b, 1.0f);
    return fixture.Add("Material.Constant3", fixture.env.Save(properties));
}

Guid AddVector2(GraphFixture& fixture, float r, float g) noexcept {
    Constant2Properties properties;
    properties.r = r;
    properties.g = g;
    return fixture.Add("Material.Constant2", fixture.env.Save(properties));
}

// node pin을 Unlit emissive로 내보내고 그 결과(float3)를 계산한다.
bool EmissiveOf(GraphFixture& fixture, Guid node, StringView pin, EvaluatedIr& result,
                MaterialTranslation& out) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(node, pin, unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
    if (!fixture.ready || !Translate(fixture, Traits(), out) || !out.succeeded) {
        return false;
    }

    for (const IrResult& item : out.module.Results()) {
        if (item.entry == IrEntry::Surface && item.slot == StringView("Emissive")) {
            return EvaluateIr(out, item.value, result);
        }
    }

    return false;
}

bool Near(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < AdvancedTolerance && difference > -AdvancedTolerance;
}

bool NearVector(const EvaluatedIr& value, float x, float y, float z) noexcept {
    return value.count == 3 && Near(value.values[0], x) && Near(value.values[1], y) &&
           Near(value.values[2], z);
}

bool HasText(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

} // namespace

// RotateAboutAxis: 결과는 위치 오프셋이고 각도는 RotationAngle × 2π / period다. 입력은 필수다.
TEST(MaterialAdvanced_RotateAboutAxis) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    GraphFixture fixture(env);
    RotateAboutAxisProperties properties;
    properties.period = 1.0f;
    const Guid rotate = fixture.Add("Material.RotateAboutAxis", env.Save(properties));
    fixture.Connect(AddVector3(fixture, 0.0f, 0.0f, 1.0f), "Result", rotate,
                    "NormalizedRotationAxis");
    fixture.Connect(AddConstant(fixture, 0.125f), "Result", rotate, "RotationAngle");
    fixture.Connect(AddVector3(fixture, 0.0f, 0.0f, 0.0f), "Result", rotate, "PivotPoint");
    fixture.Connect(AddVector3(fixture, 1.0f, 0.0f, 0.0f), "Result", rotate, "Position");
    EvaluatedIr result;
    MaterialTranslation out(env.allocator);
    REQUIRE(EmissiveOf(fixture, rotate, "Result", result, out));
    const float angle = 0.125f * (2.0f * static_cast<float>(3.14159265358979323846) / 1.0f);
    CHECK(NearVector(result, Math::Cos(angle) - 1.0f, Math::Sin(angle), 0.0f));

    GraphFixture missing(env);
    const Guid broken = missing.Add("Material.RotateAboutAxis", env.Save(properties));
    missing.Connect(broken, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation missingOut(env.allocator);
    REQUIRE(Translate(missing, Traits(), missingOut));
    CHECK(HasOnlyErrorAt(missingOut, MaterialDiagnosticCode::MissingInput, broken));
    CHECK(HasText(missingOut, "Missing RotateAboutAxis input NormalizedRotationAxis"));

    // Double 위치는 LWC 경로다(결과는 float3 오프셋).
    GraphFixture wide(env);
    const Guid wideRotate = wide.Add("Material.RotateAboutAxis", env.Save(properties));
    ConstantDoubleProperties position;
    position.value = 1.0;
    wide.Connect(AddVector3(wide, 0.0f, 0.0f, 1.0f), "Result", wideRotate,
                 "NormalizedRotationAxis");
    wide.Connect(AddConstant(wide, 0.125f), "Result", wideRotate, "RotationAngle");
    wide.Connect(AddVector3(wide, 0.0f, 0.0f, 0.0f), "Result", wideRotate, "PivotPoint");
    wide.Connect(wide.Add("Material.ConstantDouble", env.Save(position)), "Result", wideRotate,
                 "Position");
    wide.Connect(wideRotate, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation wideOut(env.allocator);
    REQUIRE(Translate(wide, Traits(), wideOut));
    CHECK(wideOut.succeeded);
}

// SphereMask: Hardness가 연결되면 비율, 아니면 핀 값이 퍼센트다.
TEST(MaterialAdvanced_SphereMask) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    const auto mask = [](float distance, float radius, float inverseHardness) noexcept {
        const float value = (1.0f - distance * (1.0f / radius)) * inverseHardness;
        return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
    };

    {
        GraphFixture fixture(env);
        const Guid sphere = fixture.Add("Material.SphereMask");
        fixture.Connect(AddConstant(fixture, 0.3f), "Result", sphere, "A");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", sphere, "B");
        fixture.Connect(AddConstant(fixture, 0.8f), "Result", sphere, "Radius");
        fixture.Connect(AddConstant(fixture, 0.2f), "Result", sphere, "Hardness");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, sphere, "Result", result, out));
        const float expected = mask(0.3f - 0.1f, 0.8f, 1.0f / (1.0f - 0.2f));
        CHECK(NearVector(result, expected, expected, expected));
    }

    {
        GraphFixture fixture(env);
        const Guid sphere = fixture.Add("Material.SphereMask");
        fixture.Connect(AddConstant(fixture, 0.3f), "Result", sphere, "A");
        fixture.Connect(AddConstant(fixture, 0.1f), "Result", sphere, "B");
        fixture.Connect(AddConstant(fixture, 0.8f), "Result", sphere, "Radius");
        fixture.SetValue(sphere, "Hardness", PinValue(20.0f, 1));
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, sphere, "Result", result, out));
        const float expected = mask(0.3f - 0.1f, 0.8f, 1.0f / (1.0f - 20.0f * 0.01f));
        CHECK(NearVector(result, expected, expected, expected));
    }

    GraphFixture missing(env);
    const Guid broken = missing.Add("Material.SphereMask");
    missing.Connect(broken, "Result", OutputNodeId, "AmbientOcclusion");
    MaterialTranslation out(env.allocator);
    REQUIRE(Translate(missing, Traits(), out));
    CHECK(HasText(out, "Missing input A"));
}

// DeriveNormalZ: z = sqrt(saturate(1 − dot(xy, xy))), 스칼라 입력은 (x, z, 0)이다.
TEST(MaterialAdvanced_DeriveNormalZ) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid derive = fixture.Add("Material.DeriveNormalZ");
        fixture.Connect(AddVector2(fixture, 0.6f, 0.0f), "Result", derive, "InXY");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, derive, "Result", result, out));
        CHECK(NearVector(result, 0.6f, 0.0f, Math::Sqrt(1.0f - 0.36f)));
    }

    {
        GraphFixture fixture(env);
        const Guid derive = fixture.Add("Material.DeriveNormalZ");
        fixture.Connect(AddConstant(fixture, 0.6f), "Result", derive, "InXY");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, derive, "Result", result, out));
        CHECK(NearVector(result, 0.6f, Math::Sqrt(1.0f - 0.36f), 0.0f));
    }
}

// Desaturation: Fraction이 없으면 스칼라 Grey, 있으면 lerp(Color, Grey, Fraction)다.
TEST(MaterialAdvanced_Desaturation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    DesaturationProperties properties;
    const float grey = 0.2f * properties.luminanceFactors.r + 0.4f * properties.luminanceFactors.g +
                       0.6f * properties.luminanceFactors.b;
    {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.Desaturation", env.Save(properties));
        fixture.Connect(AddVector3(fixture, 0.2f, 0.4f, 0.6f), "Result", node, "Input");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, grey, grey, grey));
    }

    {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.Desaturation", env.Save(properties));
        fixture.Connect(AddVector3(fixture, 0.2f, 0.4f, 0.6f), "Result", node, "Input");
        fixture.Connect(AddConstant(fixture, 0.5f), "Result", node, "Fraction");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, 0.2f + 0.5f * (grey - 0.2f), 0.4f + 0.5f * (grey - 0.4f),
                         0.6f + 0.5f * (grey - 0.6f)));
    }
}

// HsvToRgb·RgbToHsv: UE 식 그대로, float3·float4가 아니면 입력을 그대로 낸다.
TEST(MaterialAdvanced_HsvConversions) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.HsvToRgb");
        fixture.Connect(AddVector3(fixture, 0.5f, 1.0f, 1.0f), "Result", node, "Input");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, 0.0f, 1.0f, 1.0f));
    }

    {
        // UE RgbToHsv 식을 C++로 같은 순서로 계산한다.
        const float r = 0.2f;
        const float g = 0.6f;
        const float b = 0.4f;
        const float p[4] = {g < b ? b : g, g < b ? g : b, g < b ? -1.0f : 0.0f,
                            g < b ? 2.0f / 3.0f : -1.0f / 3.0f};
        const float q[4] = {r < p[0] ? p[0] : r, p[1], r < p[0] ? p[3] : p[2], r < p[0] ? r : p[0]};
        const float chroma = q[0] - (q[3] < q[1] ? q[3] : q[1]);
        const float hueRaw = (q[3] - q[1]) / (6.0f * chroma + 1e-10f) + q[2];
        const float hue = hueRaw < 0.0f ? -hueRaw : hueRaw;
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.RgbToHsv");
        fixture.Connect(AddVector3(fixture, r, g, b), "Result", node, "Input");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, hue, chroma / (q[0] + 1e-10f), q[0]));
    }

    {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.HsvToRgb");
        fixture.Connect(AddConstant(fixture, 0.3f), "Result", node, "Input");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, 0.3f, 0.3f, 0.3f));
    }
}

// BlackBody·HairColor·SrgbToWorkingColorSpace: UE 셰이더 함수와 같은 식이다.
TEST(MaterialAdvanced_ColorFunctions) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        const float t = 6500.0f;
        const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t * t) /
                        (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t * t);
        const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t * t) /
                        (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t * t);
        const float x = 3 * u / (2 * u - 8 * v + 4);
        const float y = 2 * v / (2 * u - 8 * v + 4);
        const float z = 1 - x - y;
        const float bigX = 1.0f / y * x;
        const float bigZ = 1.0f / y * z;
        const float scale = Math::Pow(0.0004f * t, 4.0f);
        const float rows[3][3] = {{3.2409699419f, -1.5373831776f, -0.4986107603f},
                                  {-0.9692436363f, 1.8759675015f, 0.0415550574f},
                                  {0.0556300797f, -0.2039769589f, 1.0569715142f}};
        float expected[3];
        for (int row = 0; row < 3; ++row) {
            const float linear = rows[row][0] * bigX + rows[row][1] * 1.0f + rows[row][2] * bigZ;
            expected[row] = (linear > 0.0f ? linear : 0.0f) * scale;
        }

        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.BlackBody");
        fixture.Connect(AddConstant(fixture, t), "Result", node, "Temp");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        REQUIRE(result.count == 3);
        for (int at = 0; at < 3; ++at) {
            const float relative =
                (result.values[at] - expected[at]) / (expected[at] == 0.0f ? 1.0f : expected[at]);
            CHECK(relative < 1e-4f && relative > -1e-4f);
        }

        GraphFixture missing(env);
        const Guid broken = missing.Add("Material.BlackBody");
        EvaluatedIr unused;
        MaterialTranslation missingOut(env.allocator);
        CHECK(!EmissiveOf(missing, broken, "Result", unused, missingOut));
        CHECK(HasText(missingOut, "Missing BlackBody Temp input"));
    }

    {
        // HairColor 기본 입력(Melanin 0.5·Redness 0·DyeColor 1)이다.
        const float b = 0.3f;
        const float b2 = b * b;
        const float b3 = b * b2;
        const float b4 = b2 * b2;
        const float b5 = b * b4;
        const float d =
            (5.969f - 0.215f * b + 2.532f * b2 - 10.73f * b3 + 5.574f * b4 + 0.245f * b5);
        const float melanin = -Math::Log(1.0f - 0.5f);
        const float eumelanin[3] = {0.506f, 0.841f, 1.653f};
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.HairColor");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Color", result, out));
        REQUIRE(result.count == 3);
        for (int at = 0; at < 3; ++at) {
            const float absorption = melanin * eumelanin[at];
            CHECK(Near(result.values[at], Math::Exp(-Math::Sqrt(absorption) * d)));
        }
    }

    {
        GraphFixture fixture(env);
        const Guid node = fixture.Add("Material.SrgbToWorkingColorSpace");
        fixture.Connect(AddVector3(fixture, 0.1f, 0.2f, 0.3f), "Result", node, "Input");
        EvaluatedIr result;
        MaterialTranslation out(env.allocator);
        REQUIRE(EmissiveOf(fixture, node, "Result", result, out));
        CHECK(NearVector(result, 0.1f, 0.2f, 0.3f));
    }
}

// Fresnel·BumpOffset: 문맥 값(PixelNormal·CameraVector·TangentToWorld)을 쓴다. Normal이 없으면
// PixelNormal이라 정점 문맥은 V4다. BumpOffset Height는 필수다.
TEST(MaterialAdvanced_ContextNodes) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    {
        GraphFixture fixture(env);
        const Guid fresnel = fixture.Add("Material.Fresnel");
        const Guid unlit = fixture.Add("Material.SurfaceUnlit");
        fixture.Connect(fresnel, "Result", unlit, "EmissiveColor");
        fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);
    }

    {
        GraphFixture fixture(env);
        const Guid fresnel = fixture.Add("Material.Fresnel");
        fixture.Connect(fresnel, "Result", OutputNodeId, "WorldPositionOffset");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(HasError(out, MaterialDiagnosticCode::StageViolation));
    }

    {
        GraphFixture fixture(env);
        const Guid bump = fixture.Add("Material.BumpOffset");
        fixture.Connect(AddConstant(fixture, 0.7f), "Result", bump, "Height");
        fixture.Connect(bump, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation out(env.allocator);
        REQUIRE(Translate(fixture, Traits(), out));
        CHECK(out.succeeded);

        GraphFixture missing(env);
        const Guid broken = missing.Add("Material.BumpOffset");
        missing.Connect(broken, "Result", OutputNodeId, "AmbientOcclusion");
        MaterialTranslation missingOut(env.allocator);
        REQUIRE(Translate(missing, Traits(), missingOut));
        CHECK(HasText(missingOut, "Missing Height input"));
    }
}

// Sobol: LUT 없이 UE 표(ComputeGPUSpatialSeed·SobolIndex)와 같은 값을 낸다. TemporalSobol은 픽셀
// 전용(SvPosition)이다.
TEST(MaterialAdvanced_Sobol) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    // UE FSobol::ComputeGPUSpatialSeed(Core Sobol.cpp:241–275)를 옮긴 기대값 계산이다.
    const auto spatial = [](uint32 x, uint32 y, int index) noexcept {
        const uint32 x0[] = {0xf68e, 0x8e56, 0x1135, 0x220a};
        const uint32 y0[] = {0x94c4, 0x4ac2, 0xfb57, 0x0454};
        const uint32 x1[] = {0x4414, 0x8828, 0xe69e, 0xae76};
        const uint32 y1[] = {0xa28a, 0x265e, 0xe69e, 0xae76};
        uint32 result = 0;
        for (uint32 bit = 0; bit < 4; ++bit) {
            result ^= (x & (1u << bit)) ? (index == 0 ? x0[bit] : x1[bit]) : 0u;
            result ^= (y & (1u << bit)) ? (index == 0 ? y0[bit] : y1[bit]) : 0u;
        }

        return result;
    };

    const uint32 numbers[10][2] = {
        {0x8680, 0x4c80}, {0xf240, 0x9240}, {0x8220, 0x0e20}, {0x4110, 0x1610}, {0xa608, 0x7608},
        {0x8a02, 0x280a}, {0xe204, 0x9e04}, {0xa400, 0x4682}, {0xe300, 0xa74d}, {0xb700, 0x9817}};
    const uint32 cellX = 19;
    const uint32 cellY = 5;
    const uint32 index = 7;
    const uint32 packed = spatial(cellX & 0xF, cellY & 0xF, 0) ^ spatial(cellX >> 4, cellY >> 4, 1);
    uint32 sobol[2] = {packed & 0xff00u, (packed << 8) & 0xff00u};
    for (uint32 bit = 0; bit < 10; ++bit) {
        if (index & (1u << bit)) {
            sobol[0] ^= numbers[bit][0];
            sobol[1] ^= numbers[bit][1];
        }
    }

    const uint32 seed[2] = {static_cast<uint32>(0.25f * 65536.0f) & 0xffffu,
                            static_cast<uint32>(0.5f * 65536.0f) & 0xffffu};
    GraphFixture fixture(env);
    const Guid node = fixture.Add("Material.Sobol");
    fixture.Connect(AddVector2(fixture, static_cast<float>(cellX), static_cast<float>(cellY)),
                    "Result", node, "Cell");
    fixture.Connect(AddConstant(fixture, static_cast<float>(index)), "Result", node, "Index");
    fixture.Connect(AddVector2(fixture, 0.25f, 0.5f), "Result", node, "Seed");
    const Guid append = fixture.Add("Material.Append");
    fixture.Connect(node, "Result", append, "A");
    fixture.Connect(AddConstant(fixture, 0.0f), "Result", append, "B");
    EvaluatedIr result;
    MaterialTranslation out(env.allocator);
    REQUIRE(EmissiveOf(fixture, append, "Result", result, out));
    CHECK(NearVector(
        result, static_cast<float>(cellX) + static_cast<float>(sobol[0] ^ seed[0]) / 65536.0f,
        static_cast<float>(cellY) + static_cast<float>(sobol[1] ^ seed[1]) / 65536.0f, 0.0f));

    GraphFixture vertex(env);
    const Guid temporal = vertex.Add("Material.TemporalSobol");
    const Guid widen = vertex.Add("Material.Append");
    vertex.Connect(temporal, "Result", widen, "A");
    vertex.Connect(AddConstant(vertex, 0.0f), "Result", widen, "B");
    vertex.Connect(widen, "Result", OutputNodeId, "WorldPositionOffset");
    MaterialTranslation vertexOut(env.allocator);
    REQUIRE(Translate(vertex, Traits(), vertexOut));
    CHECK(HasError(vertexOut, MaterialDiagnosticCode::StageViolation));

    GraphFixture pixel(env);
    const Guid pixelTemporal = pixel.Add("Material.TemporalSobol");
    const Guid pixelWiden = pixel.Add("Material.Append");
    pixel.Connect(pixelTemporal, "Result", pixelWiden, "A");
    pixel.Connect(AddConstant(pixel, 0.0f), "Result", pixelWiden, "B");
    const Guid unlit = pixel.Add("Material.SurfaceUnlit");
    pixel.Connect(pixelWiden, "Result", unlit, "EmissiveColor");
    pixel.Connect(unlit, "Result", OutputNodeId, "Surface");
    MaterialTranslation pixelOut(env.allocator);
    REQUIRE(Translate(pixel, Traits(), pixelOut));
    CHECK(pixelOut.succeeded);
}

} // namespace material_advanced_tests_file
