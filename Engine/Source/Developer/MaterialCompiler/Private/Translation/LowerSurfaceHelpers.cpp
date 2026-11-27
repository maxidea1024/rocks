#include "LoweringContext.h"
#include "LoweringTable.h"
#include "SurfaceMath.h"

// 표면 도우미 노드 lowering이다(Docs/Design/MaterialSystem 6.11절 MS2d2c). 식은 UE 5.8
// HLSLMaterialTranslator.cpp:13847–13945와 그 셰이더 함수(Substrate.ush·ThinFilmBSDF.ush·
// ParticipatingMediaCommon.ush)를 IR로 펼친 것이다.
namespace rocks::material::detail {

namespace {

// UE SUBSTRATE_LAYER_DEFAULT_THICKNESS_CM, 단위 변환이다.
constexpr float HelperDefaultThicknessCm = 0.01f;
constexpr float HelperCentimeterToMeter = 0.01f;
constexpr float HelperMeterToCentimeter = 100.0f;
// TransmittanceToMeanFreePath의 하한(ParticipatingMediaCommon.ush:170–180)이다.
constexpr float HelperTransmittanceFloor = 1e-12f;
// 유전체 F0(UE DielectricSpecularToF0 — SC:122–125)다.
constexpr float HelperDielectricF0 = 0.08f;
// 노드 입력 기본값이다(UE MES:3497–3912).
constexpr float HelperDefaultTransmittance = 0.5f;
constexpr float HelperDefaultBaseColor = 0.18f;
constexpr float HelperDefaultSpecular = 0.5f;
constexpr float HelperDefaultBaseRoughness = 0.1f;
constexpr float HelperDefaultHaziness = 0.5f;
constexpr float HelperDefaultF0 = 0.04f;
constexpr float HelperDefaultFilmThickness = 1.0f;
constexpr float HelperDefaultFilmIor = 1.44f;
// thin film 상수다(UE ThinFilmBSDF.ush·Substrate.ush:3363–3401).
constexpr float HelperPi = 3.14159265f;
constexpr float HelperFilmThicknessScale = 10.0f;
constexpr float HelperFilmIorFade = 0.03f;
constexpr float HelperFilmReferenceLimit = 0.99f;
constexpr float HelperMicroOcclusionThreshold = 0.02f;
constexpr float HelperMicroOcclusionScale = 50.0f;
constexpr int32 HelperFilmOrders = 3;

// 입력 key가 연결되면 Float width로 맞춘 값, 아니면 fallback 상수다(UE CompileWithDefaultFloatN).
Result<IrValueId> HelperFloatOr(LoweringContext& context, StringView key, uint8 width,
                                float fallback) noexcept {
    auto connected = context.IsConnected(key);
    if (!connected) {
        return Result<IrValueId>::Failure(connected.Error());
    }

    if (connected.Value()) {
        auto input = context.Input(key);
        if (!input || input.Value() == NoIrValue) {
            return input;
        }

        return context.CoerceToWidth(input.Value(), width, key);
    }

    float values[MaxIrComponents] = {fallback, fallback, fallback, fallback};
    return context.FloatConstant(Span<const float>(values, width), key);
}

// 입력 묶음이다(하나라도 위쪽 오류면 false).
struct HelperInput {
    StringView key;
    uint8 width;
    float fallback;
    IrValueId* out;
};

Result<bool> HelperInputs(LoweringContext& context, Span<const HelperInput> inputs) noexcept {
    for (const HelperInput& input : inputs) {
        auto value = HelperFloatOr(context, input.key, input.width, input.fallback);
        if (!value) {
            return Result<bool>::Failure(value.Error());
        }

        if (value.Value() == NoIrValue) {
            return Result<bool>::Success(false);
        }

        *input.out = value.Value();
    }

    return Result<bool>::Success(true);
}

// 출력 둘을 쓴다.
Result<void> HelperOutputs(LoweringContext& context, SurfaceMath& m, StringView firstKey,
                           IrValueId first, StringView secondKey, IrValueId second) noexcept {
    if (auto status = m.Status(); !status) {
        return status;
    }

    if (auto written = context.Output(firstKey, first); !written) {
        return written;
    }

    return context.Output(secondKey, second);
}

// UE SubstrateTransmittanceToMFP(HLSLT:13847–13870): MFP(cm)와 두께 그대로.
Result<void> LowerTransmittanceToMeanFreePath(LoweringContext& context) noexcept {
    IrValueId transmittance = NoIrValue;
    IrValueId thickness = NoIrValue;
    const HelperInput inputs[] = {
        {"TransmittanceColor", 3, HelperDefaultTransmittance, &transmittance},
        {"Thickness", 1, HelperDefaultThicknessCm, &thickness},
    };
    auto read = HelperInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    // TransmittanceToMeanFreePath(T, d) = 1/max(ε, −log(clamp(T, ε, 1))/max(ε, d)), d는 m.
    const IrValueId floor = m.Scalar(HelperTransmittanceFloor);
    const IrValueId depth =
        m.Mul(m.Scalar(-1.0f), m.Log(m.Clamp(transmittance, floor, m.Scalar(1.0f))));
    const IrValueId meters = m.Mul(thickness, m.Scalar(HelperCentimeterToMeter));
    const IrValueId mfp =
        m.Mul(m.Div(m.Scalar(1.0f), m.Max(floor, m.Div(depth, m.Max(floor, meters)))),
              m.Scalar(HelperMeterToCentimeter));
    return HelperOutputs(context, m, "MFP", mfp, "ThicknessOut", thickness);
}

// UE SubstrateMetalnessToDiffuseAlbedoF0(HLSLT:13872–13894): ComputeDiffuseAlbedo·ComputeF0.
Result<void> LowerMetalnessToDiffuseAlbedoF0(LoweringContext& context) noexcept {
    IrValueId baseColor = NoIrValue;
    IrValueId metallic = NoIrValue;
    IrValueId specular = NoIrValue;
    const HelperInput inputs[] = {
        {"BaseColor", 3, HelperDefaultBaseColor, &baseColor},
        {"Metallic", 1, 0.0f, &metallic},
        {"Specular", 1, HelperDefaultSpecular, &specular},
    };
    auto read = HelperInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    const IrValueId metal = m.Saturate(metallic);
    const IrValueId albedo = m.Sub(baseColor, m.Mul(baseColor, metal));
    const IrValueId f0 =
        m.Lerp(m.Splat3(m.Mul(m.Scalar(HelperDielectricF0), specular)), baseColor, metal);
    return HelperOutputs(context, m, "DiffuseAlbedo", albedo, "F0", f0);
}

// UE SubstrateComputeHazeRoughnessAndWeight(S:1131–1179 — "A Composite BRDF Model for Hazy
// Gloss")의 넓은 거칠기와 섞는 무게다.
void HazeLobe(SurfaceMath& m, IrValueId roughness, IrValueId haziness, IrValueId& wide,
              IrValueId& weight) noexcept {
    const IrValueId s2 = m.Saturate(m.Mul(m.Scalar(2.0f), roughness));
    const IrValueId s4 = m.Saturate(m.Mul(m.Scalar(4.0f), roughness));
    const IrValueId extent = m.Lerp(m.Scalar(27.0f), m.Scalar(3.0f), s4);
    const IrValueId core = m.Lerp(m.Scalar(0.01f), m.Scalar(0.5f), s2);
    const IrValueId alphaN = m.Mul(roughness, roughness);
    const IrValueId alphaW = m.Mul(alphaN, m.Add(m.Scalar(1.0f), extent));
    wide = m.Sqrt(alphaW);
    const IrValueId onePlus = m.Add(m.Scalar(1.0f), extent);
    const IrValueId p = m.Div(m.Scalar(1.0f), m.Mul(onePlus, onePlus));
    const IrValueId b =
        m.Mul(m.Scalar(2.0f), m.Add(m.Mul(core, m.OneMinus(alphaW)), m.Mul(alphaW, p)));
    const IrValueId d =
        m.Sub(m.Mul(b, b), m.Mul(m.Mul(m.Scalar(4.0f), m.Sub(b, m.Scalar(1.0f))), core));
    const IrValueId u = m.Div(m.Sub(b, m.Sqrt(d)), m.Sub(m.Mul(m.Scalar(2.0f), b), m.Scalar(2.0f)));
    const IrValueId u2 = m.Mul(u, u);
    const IrValueId w2 = m.Mul(m.Scalar(2.0f), alphaW);
    const IrValueId uMinusU2 = m.Sub(u, u2);
    const IrValueId oneMinusU = m.OneMinus(u);
    const IrValueId kh = m.Div(m.Mul(m.Mul(uMinusU2, w2), haziness),
                               m.Add(m.Add(m.Mul(oneMinusU, oneMinusU), m.Mul(uMinusU2, w2)), u2));
    const IrValueId r = m.Add(core, m.Mul(m.OneMinus(p), kh));
    weight = m.Div(kh, r);
}

// UE SubstrateHazinessToSecondaryRoughness(HLSLT:13896–13916).
Result<void> LowerHazinessToSecondaryRoughness(LoweringContext& context) noexcept {
    IrValueId roughness = NoIrValue;
    IrValueId haziness = NoIrValue;
    const HelperInput inputs[] = {
        {"BaseRoughness", 1, HelperDefaultBaseRoughness, &roughness},
        {"Haziness", 1, HelperDefaultHaziness, &haziness},
    };
    auto read = HelperInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    SurfaceMath m(context);
    IrValueId wide = NoIrValue;
    IrValueId weight = NoIrValue;
    HazeLobe(m, m.Saturate(roughness), m.Saturate(haziness), wide, weight);
    return HelperOutputs(context, m, "Second Roughness", wide, "Second Roughness Weight", weight);
}

// UE ThinFilmEvalSensitivity(ThinFilmBSDF.ush) — 가우스 감도 함수(XYZ)다.
IrValueId FilmSensitivity(SurfaceMath& m, IrValueId opd, IrValueId shift) noexcept {
    // 계수는 UE 값 × sqrt(2π·분산) / 1.0685e-7을 미리 곱한 것이다(val·sqrt(2πvar)/정규화).
    constexpr float Scales[] = {0.846590002f, 1.00022189f, 1.00112249f};
    constexpr float Positions[] = {1.6810e+06f, 1.7953e+06f, 2.2084e+06f};
    constexpr float Variances[] = {4.3278e+09f, 9.3046e+09f, 6.6121e+09f};
    constexpr float ExtraScale = 0.153868307f;
    constexpr float ExtraPosition = 2.2399e+06f;
    constexpr float ExtraVariance = 4.5282e+09f;
    // 2π × 1e-6(opd의 µm → 위상)이다.
    constexpr float PhaseScale = 6.28318531e-06f;
    const IrValueId phase = m.Mul(m.Scalar(PhaseScale), opd);
    const IrValueId phaseSquared = m.Mul(phase, phase);
    const IrValueId positions = m.Vector3(Positions[0], Positions[1], Positions[2]);
    const IrValueId variances = m.Vector3(Variances[0], Variances[1], Variances[2]);
    const IrValueId xyz = m.Mul(m.Mul(m.Vector3(Scales[0], Scales[1], Scales[2]),
                                      m.Cos(m.Add(m.Mul(positions, phase), shift))),
                                m.Exp(m.Mul(m.Mul(m.Scalar(-1.0f), variances), phaseSquared)));
    const IrValueId extra = m.Mul(
        m.Mul(m.Scalar(ExtraScale), m.Cos(m.Add(m.Mul(m.Scalar(ExtraPosition), phase), shift))),
        m.Exp(m.Mul(m.Scalar(-ExtraVariance), phaseSquared)));
    return m.Add(xyz, m.Mul(m.Vector3(1.0f, 0.0f, 0.0f), extra));
}

// UE ThinFilm(HLSLT:13918–13945 → SubstrateGetThinFilmF0F90 → F_ThinFilm 비참조판).
Result<void> LowerThinFilm(LoweringContext& context) noexcept {
    IrValueId f0 = NoIrValue;
    IrValueId f90 = NoIrValue;
    IrValueId thickness = NoIrValue;
    IrValueId ior = NoIrValue;
    const HelperInput inputs[] = {
        {"F0", 3, HelperDefaultF0, &f0},
        {"F90", 3, 1.0f, &f90},
        {"Thickness", 1, HelperDefaultFilmThickness, &thickness},
        {"IOR", 1, HelperDefaultFilmIor, &ior},
    };
    auto read = HelperInputs(context, inputs);
    if (!read || !read.Value()) {
        return read ? Result<void>::Success() : Result<void>::Failure(read.Error());
    }

    // Normal은 UE CompileWithDefaultNormalWS다(연결이 없으면 정점 노멀).
    auto connected = context.IsConnected("Normal");
    if (!connected) {
        return Result<void>::Failure(connected.Error());
    }

    IrValueId normal = NoIrValue;
    if (connected.Value()) {
        auto input = HelperFloatOr(context, "Normal", 3, 0.0f);
        if (!input || input.Value() == NoIrValue) {
            return input ? Result<void>::Success() : Result<void>::Failure(input.Error());
        }

        normal = input.Value();
        if (context.Traits().tangentSpaceNormal) {
            const IrValueId arguments[] = {normal};
            auto world = context.ContextValue("TransformTangentVectorToWorld", arguments);
            if (!world) {
                return Result<void>::Failure(world.Error());
            }

            normal = world.Value();
        }
    } else {
        auto vertex = context.ContextValue("VertexNormal");
        if (!vertex) {
            return Result<void>::Failure(vertex.Error());
        }

        normal = vertex.Value();
    }

    auto camera = context.ContextValue("CameraVector");
    if (!camera) {
        return Result<void>::Failure(camera.Error());
    }

    SurfaceMath m(context);
    // NoV = dot(N, V)(UE는 saturate하지 않는다 — NoV ≤ 0이면 반사 0).
    const IrValueId noV = m.Dot(normal, camera.Value());
    const IrValueId depth = m.Mul(thickness, m.Scalar(HelperFilmThicknessScale));
    const IrValueId eta2 = m.Lerp(m.Scalar(1.0f), ior,
                                  m.Smoothstep(m.Scalar(0.0f), m.Scalar(HelperFilmIorFade), depth));
    const IrValueId inverseEta = m.Div(m.Scalar(1.0f), eta2);
    const IrValueId cos2 = m.Sqrt(
        m.Sub(m.Scalar(1.0f), m.Mul(m.Mul(inverseEta, inverseEta), m.OneMinus(m.Mul(noV, noV)))));
    // R12 = F_Schlick(DielectricIorToF0(IOR), VoH).x(VoH = NoV).
    const IrValueId f0Root = m.Div(m.Sub(ior, m.Scalar(1.0f)), m.Add(ior, m.Scalar(1.0f)));
    const IrValueId filmF0 = m.Mul(f0Root, f0Root);
    const IrValueId fresnel = m.Pow5(m.OneMinus(noV));
    const IrValueId r12 =
        m.Add(m.Mul(m.Saturate(m.Mul(m.Scalar(HelperMicroOcclusionScale), filmF0)), fresnel),
              m.Mul(m.OneMinus(fresnel), filmF0));
    const IrValueId t121 = m.OneMinus(r12);
    const IrValueId opd = m.Mul(depth, cos2);
    const IrValueId phi2 = m.Scalar(HelperPi);
    const IrValueId r123 = m.Mul(r12, f0);
    const IrValueId root123 = m.Sqrt(r123);
    const IrValueId rs = m.Div(m.Mul(m.Mul(t121, t121), f0), m.OneMinus(r123));
    IrValueId intensity = m.Mul(m.Add(r12, rs), FilmSensitivity(m, m.Scalar(0.0f), m.Scalar(0.0f)));
    IrValueId cm = m.Sub(rs, t121);
    for (int32 order = 1; order <= HelperFilmOrders; ++order) {
        cm = m.Mul(cm, root123);
        const IrValueId factor = m.Scalar(static_cast<float>(order));
        intensity =
            m.Add(intensity, m.Mul(m.Mul(cm, m.Scalar(2.0f)),
                                   FilmSensitivity(m, m.Mul(factor, opd), m.Mul(factor, phi2))));
    }

    // XYZ → RGB(ThinFilm_XYZ_TO_RGB 행 벡터 곱).
    const IrValueId rgb = m.Add(
        m.Add(m.Mul(m.Component(intensity, 0), m.Vector3(2.3706743f, -0.5138850f, 0.0052982f)),
              m.Mul(m.Component(intensity, 1), m.Vector3(-0.9000405f, 1.4253036f, -0.0146949f))),
        m.Mul(m.Component(intensity, 2), m.Vector3(-0.4706338f, 0.0885814f, 1.0093968f)));
    const IrValueId film =
        m.Select(m.Greater(noV, m.Scalar(0.0f)), m.Saturate(rgb), m.Vector3(0.0f));
    // SubstrateGetThinFilmF0F90(S:3363–3388).
    const IrValueId reference = m.Min(m.Scalar(HelperFilmReferenceLimit), m.Pow5(m.OneMinus(noV)));
    const IrValueId reflected = m.Max(film, reference);
    IrValueId thinF0 = m.Div(m.Sub(reflected, m.Mul(f90, reference)), m.OneMinus(reference));
    thinF0 = m.Max(m.Scalar(HelperMicroOcclusionThreshold), thinF0);
    thinF0 = m.Mul(thinF0, m.Saturate(m.Mul(m.Scalar(HelperMicroOcclusionScale), m.Max3(f0))));
    const IrValueId thinF90 = m.Max(thinF0, f90);
    return HelperOutputs(context, m, "Specular Color", thinF0, "Edge Specular Color", thinF90);
}

constexpr LoweringEntry SurfaceHelperEntries[] = {
    {"Material.TransmittanceToMeanFreePath", &LowerTransmittanceToMeanFreePath},
    {"Material.MetalnessToDiffuseAlbedoF0", &LowerMetalnessToDiffuseAlbedoF0},
    {"Material.HazinessToSecondaryRoughness", &LowerHazinessToSecondaryRoughness},
    {"Material.ThinFilm", &LowerThinFilm},
};

} // namespace

Span<const LoweringEntry> SurfaceHelperLowerings() noexcept {
    return SurfaceHelperEntries;
}

} // namespace rocks::material::detail
