#include <CurveAssets/ColorCurveEvaluate.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <Platform/Memory.h>

#include "MaterialGraphFixture.h"
#include "MaterialIrEvaluator.h"

// MS2f4 시험이다(Docs/Design/MaterialSystem 6.13절 MS2f4): Input이 없으면 CurveAssets
// EvaluateAdjustedColorCurve와 비트까지 같은 상수, 연결되면 UE 키 구간 합(키 0·1·2개와 float4
// 묶음·나머지 — 조정을 쓰지 않는다), 커브가 없으면 UE 문구, 번역 의존 ColorCurve 기록을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_color_ramp_tests_file {

using namespace material_graph_fixture;
using namespace material_ir_evaluator;

namespace {

constexpr Guid RampCurveId{0x52414d50ull, 1};
// 구간 합 기대값의 허용 오차다(같은 연산 순서 — Dot 합 순서만 다를 수 있다).
constexpr float RampTolerance = 1e-6f;

// 색 커브 하나를 공급하는 provider다.
class RampSources final : public MaterialSourceProvider {
public:
    explicit RampSources(const MaterialColorCurveSnapshot& snapshot) noexcept
        : snapshot_(snapshot) {
    }

    const MaterialGraphSnapshot* FindFunction(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialGraphSnapshot* FindMaterial(Guid) const noexcept override {
        return nullptr;
    }

    const MaterialColorCurveSnapshot* FindColorCurve(Guid asset) const noexcept override {
        return asset == RampCurveId ? &snapshot_ : nullptr;
    }

private:
    const MaterialColorCurveSnapshot& snapshot_;
};

// 시험 커브 네 채널이다.
struct RampCurves {
    explicit RampCurves(Allocator& allocator) noexcept
        : r(allocator), g(allocator), b(allocator), a(allocator) {
    }

    MaterialColorCurveSnapshot Snapshot(const ColorCurveAdjustments& adjustments) const noexcept {
        return MaterialColorCurveSnapshot{&r, &g, &b, &a, adjustments};
    }

    TangentCurve r;
    TangentCurve g;
    TangentCurve b;
    TangentCurve a;
};

bool AddKeys(TangentCurve& curve, Span<const float> times, Span<const float> values) noexcept {
    for (usize at = 0; at < times.Size(); ++at) {
        if (!curve.TryAddKey(times[at], values[at])) {
            return false;
        }
    }

    return true;
}

Guid AddRamp(GraphFixture& fixture, Guid curve) noexcept {
    ColorRampProperties properties;
    properties.colorCurve.assetId = AssetId{curve};
    return fixture.Add("Material.ColorRamp", fixture.env.Save(properties));
}

bool TranslateRamp(GraphFixture& fixture, const MaterialSourceProvider* sources,
                   MaterialTranslation& out) noexcept {
    MaterialTranslationInput input{fixture.document, fixture.env.catalog, fixture.env.names,
                                   Traits(),         TestAssetId,         {}};
    input.sources = sources;
    return fixture.ready && TranslateMaterial(fixture.env.allocator, input, out).IsOk();
}

// ramp Result의 R·G·B(emissive)와 A(AO 마스크)를 계산한다.
bool EvaluateRamp(GraphFixture& fixture, Guid ramp, const MaterialSourceProvider* sources,
                  MaterialTranslation& out, float (&color)[MaxIrComponents]) noexcept {
    const Guid unlit = fixture.Add("Material.SurfaceUnlit");
    fixture.Connect(ramp, "Result", unlit, "EmissiveColor");
    fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
    ComponentMaskProperties alpha;
    alpha.a = true;
    const Guid mask = fixture.Add("Material.ComponentMask", fixture.env.Save(alpha));
    fixture.Connect(ramp, "Result", mask, "Input");
    fixture.Connect(mask, "Result", OutputNodeId, "PixelDepthOffset");
    if (!TranslateRamp(fixture, sources, out) || !out.succeeded) {
        return false;
    }

    EvaluatedIr rgb;
    EvaluatedIr a;
    bool found = false;
    bool foundAlpha = false;
    for (const IrResult& item : out.module.Results()) {
        if (item.entry == IrEntry::Surface && item.slot == StringView("Emissive")) {
            found = EvaluateIr(out, item.value, rgb);
        } else if (item.entry == IrEntry::PixelDepthOffset) {
            foundAlpha = EvaluateIr(out, item.value, a);
        }
    }

    if (!found || !foundAlpha || rgb.count != 3 || a.count != 1) {
        return false;
    }

    color[0] = rgb.values[0];
    color[1] = rgb.values[1];
    color[2] = rgb.values[2];
    color[3] = a.values[0];
    return true;
}

uint32 BitsOf(float value) noexcept {
    uint32 bits = 0;
    platform::MemoryCopy(&bits, &value, sizeof(bits));
    return bits;
}

bool Near(float value, float expected) noexcept {
    const float difference = value - expected;
    return difference < RampTolerance && difference > -RampTolerance;
}

float Saturate(float value) noexcept {
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

bool HasMessage(const MaterialTranslation& out, StringView message) noexcept {
    for (const nodegraph::GraphDiagnostic& diagnostic : out.diagnostics.items) {
        if (diagnostic.message.View() == message) {
            return true;
        }
    }

    return false;
}

} // namespace

// Input이 없으면 핀 기본값(UE ConstInput 0.5)에서 조정 포함 커브 평가식의 상수다(CurveAssets와
// 비트까지 같다). 번역 의존에 커브가 남는다.
TEST(MaterialColorRamp_ConstantInputUsesCurveEvaluation) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    RampCurves curves(env.allocator);
    const float times[] = {0.0f, 0.3f, 1.0f};
    const float reds[] = {0.1f, 0.9f, 0.2f};
    const float greens[] = {0.0f, 0.4f, 1.0f};
    const float blues[] = {1.0f, 0.5f, 0.25f};
    REQUIRE(AddKeys(curves.r, times, reds));
    REQUIRE(AddKeys(curves.g, times, greens));
    REQUIRE(AddKeys(curves.b, times, blues));
    ColorCurveAdjustments adjustments;
    adjustments.hue = 20.0f;
    adjustments.saturation = 0.8f;
    adjustments.brightness = 1.2f;
    adjustments.minAlpha = 0.25f;
    const MaterialColorCurveSnapshot snapshot = curves.Snapshot(adjustments);
    const RampSources sources(snapshot);
    const LinearColor expected = EvaluateAdjustedColorCurve(curves.r, curves.g, curves.b, curves.a,
                                                            adjustments, 0.5f, false);

    GraphFixture fixture(env);
    const Guid ramp = AddRamp(fixture, RampCurveId);
    MaterialTranslation out(env.allocator);
    float color[MaxIrComponents] = {};
    REQUIRE(EvaluateRamp(fixture, ramp, &sources, out, color));
    CHECK(BitsOf(color[0]) == BitsOf(expected.r));
    CHECK(BitsOf(color[1]) == BitsOf(expected.g));
    CHECK(BitsOf(color[2]) == BitsOf(expected.b));
    // 알파 키가 없으면 커브 A는 1이고 조정 lerp(minAlpha, maxAlpha, 1)이다.
    CHECK(BitsOf(color[3]) == BitsOf(expected.a));
    REQUIRE(out.dependencies.Size() == 1);
    CHECK(out.dependencies[0].role == MaterialDependencyRole::ColorCurve);
    CHECK(out.dependencies[0].asset == RampCurveId);
}

// Input이 연결되면 UE 키 구간 합이다: 키 0개 0(알파도 — 상수 경로의 1과 다르다), 1개 그 값, 2개
// 자르지 않는 lerp, 6개는 float4 묶음 하나와 나머지 한 구간이다. 색 조정은 쓰지 않는다.
TEST(MaterialColorRamp_DynamicInputUsesKeySegments) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    RampCurves curves(env.allocator);
    const float single[] = {0.75f};
    const float oneTime[] = {0.2f};
    const float pairTimes[] = {0.0f, 0.5f};
    const float pairValues[] = {0.2f, 0.6f};
    const float manyTimes[] = {0.0f, 0.1f, 0.25f, 0.4f, 0.7f, 1.0f};
    const float manyValues[] = {0.0f, 0.5f, 0.2f, 0.9f, 0.3f, 1.0f};
    REQUIRE(AddKeys(curves.g, oneTime, single));
    REQUIRE(AddKeys(curves.b, pairTimes, pairValues));
    REQUIRE(AddKeys(curves.a, manyTimes, manyValues));
    ColorCurveAdjustments adjustments;
    adjustments.hue = 90.0f;
    adjustments.brightness = 2.0f;
    const MaterialColorCurveSnapshot snapshot = curves.Snapshot(adjustments);
    const RampSources sources(snapshot);

    const float inputs[] = {0.33f, 0.8f, 1.5f};
    for (float x : inputs) {
        GraphFixture fixture(env);
        const Guid ramp = AddRamp(fixture, RampCurveId);
        ConstantProperties input;
        input.r = x;
        fixture.Connect(fixture.Add("Material.Constant", env.Save(input)), "Result", ramp, "Input");
        MaterialTranslation out(env.allocator);
        float color[MaxIrComponents] = {};
        REQUIRE(EvaluateRamp(fixture, ramp, &sources, out, color));

        // UE: Lerp(v0, v1, (x − t0) / (t1 − t0))는 범위 밖도 늘린다.
        const float pair = pairValues[0] + ((x - pairTimes[0]) / (pairTimes[1] - pairTimes[0])) *
                                               (pairValues[1] - pairValues[0]);
        float many = manyValues[0];
        for (usize at = 0; at + 1 < 6; ++at) {
            many += (manyValues[at + 1] - manyValues[at]) *
                    Saturate((x - manyTimes[at]) / (manyTimes[at + 1] - manyTimes[at]));
        }

        CHECK(color[0] == 0.0f);
        CHECK(color[1] == single[0]);
        CHECK(Near(color[2], pair));
        CHECK(Near(color[3], many));
    }
}

// 커브가 없거나(참조 없음·공급에 없음) 공급이 없으면 UE 문구 "Missing ColorCurve"다.
TEST(MaterialColorRamp_MissingCurve) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    RampCurves curves(env.allocator);
    const MaterialColorCurveSnapshot snapshot = curves.Snapshot(ColorCurveAdjustments{});
    const RampSources sources(snapshot);
    const struct {
        Guid curve;
        const MaterialSourceProvider* provider;
    } cases[] = {{Guid{}, &sources}, {Guid{0x52414d50ull, 9}, &sources}, {RampCurveId, nullptr}};
    for (const auto& item : cases) {
        GraphFixture fixture(env);
        const Guid ramp = AddRamp(fixture, item.curve);
        const Guid unlit = fixture.Add("Material.SurfaceUnlit");
        fixture.Connect(ramp, "Result", unlit, "EmissiveColor");
        fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateRamp(fixture, item.provider, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, ramp));
        CHECK(HasMessage(out, "Missing ColorCurve"));
        CHECK(out.dependencies.IsEmpty());
    }
}

} // namespace material_color_ramp_tests_file
