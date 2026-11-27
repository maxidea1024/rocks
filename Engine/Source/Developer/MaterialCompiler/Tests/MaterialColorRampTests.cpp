#include <CurveAssets/ColorCurveEvaluate.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.generated.h>
#include <MaterialSchema/Nodes/Color/ColorRampProperties.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>

#include "MaterialGraphFixture.h"
#include "MaterialIrEvaluator.h"

// MS2f4 시험이다(Docs/Design/MaterialSystem 6.13절 MS2f4): Input이 없으면 CurveAssets
// EvaluateAdjustedColorCurveDeterministic(기준 부동소수점 환경)과 비트까지 같은 상수, 연결되면 UE
// 키 구간 합(키 0·1·2개와 float4 묶음·나머지 — 조정을 쓰지 않는다), 커브가 없으면 UE 문구, 번역
// 의존 ColorCurve 기록을 본다.
using namespace rocks;
using namespace rocks::material;
namespace material_color_ramp_tests_file {

using namespace material_graph_fixture;
using namespace material_ir_evaluator;

namespace {

constexpr Guid RampCurveId{0x52414d50ull, 1};
// 공급에 없는 커브 id다.
constexpr Guid UnknownCurveId{0x52414d50ull, 9};
// 키 구간 합의 float4 묶음 키 수다(UE ME:4949).
constexpr usize RampVectorKeys = 4;
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

// ramp Result의 R·G·B(emissive)와 A(PixelDepthOffset 마스크)를 계산한다.
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

// 쿠킹 경로와 같은 결정적 평가다(기준 부동소수점 환경 안 — CurveAssets CAD12).
LinearColor EvaluateDeterministic(const RampCurves& curves,
                                  const ColorCurveAdjustments& adjustments, float time) noexcept {
    struct Evaluation {
        const RampCurves* curves;
        const ColorCurveAdjustments* adjustments;
        float time;
        LinearColor color;
    };

    Evaluation evaluation{&curves, &adjustments, time, LinearColor()};
    platform::RunWithDeterministicFloatingPoint(
        [](void* job) noexcept {
            Evaluation& run = *static_cast<Evaluation*>(job);
            run.color = EvaluateAdjustedColorCurveDeterministic(run.curves->r, run.curves->g,
                                                                run.curves->b, run.curves->a,
                                                                *run.adjustments, run.time, false);
        },
        &evaluation);
    return evaluation.color;
}

} // namespace

// Input이 없으면 핀 기본값(UE ConstInput 0.5)에서 조정 포함 결정적 커브 평가식의 상수다(CurveAssets
// 결정적 판과 비트까지 같다). 번역 의존에 커브가 남는다.
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
    const LinearColor expected = EvaluateDeterministic(curves, adjustments, 0.5f);

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

    // 핀 사용자 값(다성분이면 첫 성분)이 시간이다.
    GraphFixture valued(env);
    const Guid valuedRamp = AddRamp(valued, RampCurveId);
    valued.SetValue(valuedRamp, "Input", PinValue(0.7f, 2));
    MaterialTranslation valuedOut(env.allocator);
    float valuedColor[MaxIrComponents] = {};
    REQUIRE(EvaluateRamp(valued, valuedRamp, &sources, valuedOut, valuedColor));
    const LinearColor at = EvaluateDeterministic(curves, adjustments, 0.7f);
    CHECK(BitsOf(valuedColor[0]) == BitsOf(at.r));
    CHECK(BitsOf(valuedColor[3]) == BitsOf(at.a));

    // 호출자 부동소수점 환경이 달라도(실시간 오디오 환경 — FTZ) 상수 byte가 같다(결정성 — 번역이
    // 기준 환경에서 평가한다).
    struct ShiftedRun {
        TranslatorEnv* env;
        const RampSources* sources;
        float color[MaxIrComponents];
        bool evaluated;
    };

    ShiftedRun shifted{&env, &sources, {}, false};
    platform::RunWithRealtimeAudioFloatingPoint(
        [](void* job) noexcept {
            ShiftedRun& run = *static_cast<ShiftedRun*>(job);
            GraphFixture graph(*run.env);
            const Guid node = AddRamp(graph, RampCurveId);
            MaterialTranslation out(run.env->allocator);
            run.evaluated = EvaluateRamp(graph, node, run.sources, out, run.color);
        },
        &shifted);
    REQUIRE(shifted.evaluated);
    for (usize component = 0; component < 4; ++component) {
        CHECK(BitsOf(shifted.color[component]) == BitsOf(color[component]));
    }
}

// Input이 연결되면 UE 키 구간 합이다(접히지 않는 입력 sin(c)): 키 0개 0(알파도 — 상수 경로의 1과
// 다르다), 1개 그 값, 2개 자르지 않는 lerp, 그 밖은 float4 묶음 floor((N − 1) / 4)개(Dot)와 나머지
// 스칼라 구간(Saturate)이다. 색 조정은 쓰지 않는다. 값은 첫 키 아래·안·위에서 본다.
TEST(MaterialColorRamp_DynamicInputUsesKeySegments) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    using Math = platform::PlatformMath;
    const usize counts[] = {0, 1, 2, 3, 4, 5, 8, 9};
    // sin(c)가 첫 키(0) 아래·안·마지막 키 위가 되는 c다.
    const float seeds[] = {-0.3f, 0.45f, 1.4f};
    for (usize count : counts) {
        RampCurves curves(env.allocator);
        float times[9] = {};
        float values[9] = {};
        for (usize at = 0; at < count; ++at) {
            times[at] = static_cast<float>(at) * 0.11f;
            values[at] = static_cast<float>((at * 7) % 5) * 0.2f + 0.05f;
        }

        REQUIRE(
            AddKeys(curves.r, Span<const float>(times, count), Span<const float>(values, count)));
        ColorCurveAdjustments adjustments;
        adjustments.hue = 90.0f;
        adjustments.brightness = 2.0f;
        const MaterialColorCurveSnapshot snapshot = curves.Snapshot(adjustments);
        const RampSources sources(snapshot);
        for (float seed : seeds) {
            GraphFixture fixture(env);
            const Guid ramp = AddRamp(fixture, RampCurveId);
            fixture.Connect(AddVarying(fixture, seed), "Result", ramp, "Input");
            MaterialTranslation out(env.allocator);
            float color[MaxIrComponents] = {};
            REQUIRE(EvaluateRamp(fixture, ramp, &sources, out, color));

            const float x = Math::Sin(seed);
            float expected = 0.0f;
            if (count == 1) {
                expected = values[0];
            } else if (count == 2) {
                expected =
                    values[0] + ((x - times[0]) / (times[1] - times[0])) * (values[1] - values[0]);
            } else if (count > 2) {
                expected = values[0];
                for (usize at = 0; at + 1 < count; ++at) {
                    expected += (values[at + 1] - values[at]) *
                                Saturate((x - times[at]) / (times[at + 1] - times[at]));
                }
            }

            CHECK(Near(color[0], expected));
            CHECK(color[1] == 0.0f);
            CHECK(color[3] == 0.0f);

            usize dots = 0;
            usize saturates = 0;
            usize lerps = 0;
            for (const IrValue& value : out.module.Values()) {
                dots += value.op == IrOp::Dot ? 1 : 0;
                saturates += value.op == IrOp::Saturate ? 1 : 0;
                lerps += value.op == IrOp::Lerp ? 1 : 0;
            }

            const usize segments = count > 2 ? count - 1 : 0;
            const usize vectors = segments / RampVectorKeys;
            CHECK(dots == vectors);
            CHECK(saturates == vectors + (segments - vectors * RampVectorKeys));
            CHECK(lerps == (count == 2 ? 1u : 0u));
        }
    }
}

// 키 둘의 값이 같으면 UE Lerp가 끝 값을 그대로 돌려준다(HT:10378 — lerp 연산이 남지 않는다).
TEST(MaterialColorRamp_EqualPairSkipsLerp) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    RampCurves curves(env.allocator);
    const float times[] = {0.2f, 0.6f};
    const float values[] = {0.25f, 0.25f};
    REQUIRE(AddKeys(curves.r, times, values));
    const MaterialColorCurveSnapshot snapshot = curves.Snapshot(ColorCurveAdjustments{});
    const RampSources sources(snapshot);
    GraphFixture fixture(env);
    const Guid ramp = AddRamp(fixture, RampCurveId);
    fixture.Connect(AddVarying(fixture, 0.3f), "Result", ramp, "Input");
    MaterialTranslation out(env.allocator);
    float color[MaxIrComponents] = {};
    REQUIRE(EvaluateRamp(fixture, ramp, &sources, out, color));
    CHECK(color[0] == values[0]);
    for (const IrValue& value : out.module.Values()) {
        CHECK(value.op != IrOp::Lerp);
    }
}

// 커브 진단: 참조가 비었거나 공급이 없으면 UE 문구 "Missing ColorCurve", 공급에 없으면 "Color curve
// asset is not available", 채널이 빈 snapshot도 "Missing ColorCurve"다. 의존은 남지 않는다(빈
// 채널은 찾은 커브라 남는다).
TEST(MaterialColorRamp_MissingCurve) {
    TranslatorEnv env;
    REQUIRE(env.ready);
    RampCurves curves(env.allocator);
    const MaterialColorCurveSnapshot snapshot = curves.Snapshot(ColorCurveAdjustments{});
    const RampSources sources(snapshot);
    MaterialColorCurveSnapshot empty = snapshot;
    empty.g = nullptr;
    const RampSources emptySources(empty);
    const struct {
        Guid curve;
        const MaterialSourceProvider* provider;
        StringView message;
        usize dependencies;
    } cases[] = {{Guid{}, &sources, "Missing ColorCurve", 0},
                 {UnknownCurveId, &sources, "Color curve asset is not available", 0},
                 {RampCurveId, nullptr, "Missing ColorCurve", 0},
                 {RampCurveId, &emptySources, "Missing ColorCurve", 1}};
    for (const auto& item : cases) {
        GraphFixture fixture(env);
        const Guid ramp = AddRamp(fixture, item.curve);
        const Guid unlit = fixture.Add("Material.SurfaceUnlit");
        fixture.Connect(ramp, "Result", unlit, "EmissiveColor");
        fixture.Connect(unlit, "Result", OutputNodeId, "Surface");
        MaterialTranslation out(env.allocator);
        REQUIRE(TranslateRamp(fixture, item.provider, out));
        CHECK(HasOnlyErrorAt(out, MaterialDiagnosticCode::MissingInput, ramp));
        CHECK(HasMessage(out, item.message));
        CHECK(out.dependencies.Size() == item.dependencies);
    }
}

} // namespace material_color_ramp_tests_file
