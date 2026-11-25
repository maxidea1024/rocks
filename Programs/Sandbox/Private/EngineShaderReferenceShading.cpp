#include "EngineShaderProbe.h"

// A04 셰이딩의 CPU double 기준과 물리 성질 시험이다(Docs/Design/EngineShaders 7.5절, 8.1절
// (N)·(P)·(S)). BRDF 항은 Walter 2007·Heitz 2014/2018·Schlick·Burley 2012/Lagarde 2014·Chan
// 2018·Estevez–Kulla 2017· Neubelt–Pettineo 2013 식을 double로 계산한다. 수치 적분 판정은 상대
// 1%(8.3절)다. 환경 BRDF 해석 근사는 split-sum 수치 적분과의 차이를 재서 기록한다(SL3 LUT 전
// — 7.5절).
namespace sandbox::engine_shaders {

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr uint32 Width = 128;
constexpr uint32 Height = 128;
// 수치 적분 판정(8.3절)이다.
constexpr double IntegralTolerance = 0.01;
// Chan 2018 확산의 백색 노 초과 상한이다: 거친 표면(roughness 1)·스치는 시선에서 retro-reflection
// 항이 에너지를 약 4.5% 더한다(측정 — 발표 식 그대로, UE처럼 엽을 1로 자르지 않는다). 엔진
// 셰이더 7.5절에 적은 넓힌 판정이다.
constexpr double ChanEnergyExcess = 0.05;
// 거듭제곱·지수의 첫 차수 오차 전파에 곱하는 여유다.
constexpr double PropagationMargin = 2.0;
constexpr double MinimumClothRoughness = 0.0078125;

float ProbeRoughness(uint32 c, uint32 stream) noexcept {
    const float scaled = ProbeUnit(c, stream, 0) * 0.98f;
    return scaled + 0.02f;
}

Double3 UpperDirection(uint32 c, uint32 stream) noexcept {
    const Double3 d = ProbeDirection(c, stream, 0);
    return Normalize(Double3{d.x, d.y, fabs(d.z) + 0.01});
}

double Ggx(double alpha, double mu) noexcept {
    const double a2 = alpha * alpha;
    const double d = mu * mu * (a2 - 1.0) + 1.0;
    return a2 / (Pi * d * d);
}

double GgxAniso(double ax, double ay, const Double3& h) noexcept {
    const double s = h.x * h.x / (ax * ax) + h.y * h.y / (ay * ay) + h.z * h.z;
    return 1.0 / (Pi * ax * ay * s * s);
}

double Charlie(double roughness, double mu) noexcept {
    const double inverse = 1.0 / fmax(roughness, MinimumClothRoughness);
    const double sin2 = fmax(1.0 - mu * mu, 0.0);
    return (2.0 + inverse) * pow(sin2, 0.5 * inverse) / (2.0 * Pi);
}

double InverseGgx(double alpha, double mu) noexcept {
    const double a2 = fmax(alpha * alpha, MinimumClothRoughness * MinimumClothRoughness);
    const double cos2 = mu * mu;
    const double sin2 = fmax(1.0 - cos2, 1.0e-7);
    return (1.0 + 4.0 * exp(-cos2 / (sin2 * a2)) / (sin2 * sin2)) / (Pi * (1.0 + 4.0 * a2));
}

double VisibilityJoint(double alpha, double nv, double nl) noexcept {
    const double a2 = alpha * alpha;
    return 0.5 / (nl * sqrt(nv * nv * (1.0 - a2) + a2) + nv * sqrt(nl * nl * (1.0 - a2) + a2));
}

double SmithG1(double alpha, double nv) noexcept {
    const double a2 = alpha * alpha;
    return 2.0 * nv / (nv + sqrt(a2 + (1.0 - a2) * nv * nv));
}

double SmithG1Aniso(const Double3& v, double ax, double ay) noexcept {
    const double lambda =
        0.5 * (-1.0 + sqrt(1.0 + (ax * ax * v.x * v.x + ay * ay * v.y * v.y) / (v.z * v.z)));
    return 1.0 / (1.0 + lambda);
}

double Pow5(double x) noexcept {
    return x * x * x * x * x;
}

double Burley(double roughness, double nv, double nl, double lh) noexcept {
    const double fd90 = 0.5 * roughness + 2.0 * lh * lh * roughness;
    const double energy = 1.0 + (1.0 / 1.51 - 1.0) * roughness;
    return (1.0 + (fd90 - 1.0) * Pow5(1.0 - nl)) * (1.0 + (fd90 - 1.0) * Pow5(1.0 - nv)) * energy /
           Pi;
}

double Chan(double roughness, double nv, double nl, double vh, double nh) noexcept {
    const double alpha2 = fmax(roughness * roughness * roughness * roughness, 1.0e-30);
    const double gloss = fmin(fmax(log2(fmax(2.0 / alpha2 - 1.0, 1.0)) / 18.0, 0.0), 1.0);
    const double rough = vh + Pow5(1.0 - vh);
    const double smooth = (1.0 - 0.75 * Pow5(1.0 - nv)) * (1.0 - 0.75 * Pow5(1.0 - nl));
    const double blend = fmin(fmax(2.2 * gloss - 0.5, 0.0), 1.0);
    const double retro = ((34.5 * gloss - 59.0) * gloss + 24.5) * vh *
                         exp2(-fmax(73.2 * gloss - 21.2, 8.9) * sqrt(nh));
    return (rough + (smooth - rough) * blend + retro) / Pi;
}

double FresnelDielectric(double eta, double c) noexcept {
    const double g2 = eta * eta - 1.0 + c * c;
    if (g2 < 0.0) {
        return 1.0;
    }

    const double g = sqrt(g2);
    const double a = (g - c) / (g + c);
    const double b = (c * (g + c) - 1.0) / (c * (g - c) + 1.0);
    return 0.5 * a * a * (1.0 + b * b);
}

// 반구 격자 칸(ShadingTestKernels 사상 — cos θ = 1 − s², dω = 2s ds dφ)의 방향과 입체각이다.
Double3 GridDirection(uint32 x, uint32 y, uint32 width, uint32 rows, double& solidAngle) noexcept {
    const double s = (y + 0.5) / rows;
    const double mu = 1.0 - s * s;
    const double sine = sqrt(fmax(1.0 - mu * mu, 0.0));
    const double phi = (x + 0.5) * 2.0 * Pi / width;
    solidAngle = 2.0 * s * (1.0 / rows) * (2.0 * Pi / width);
    return {sine * cos(phi), sine * sin(phi), mu};
}

void RunTermProbes(ProbeContext& context) noexcept {
    // 분포 D(GGX 등방·이방, Charlie, inverse GGX): GGX 정점 근처의 1 − μ²(1 − α²) 상쇄를 사례별
    // 허용 오차에 넣는다(사양 산술 상한의 첫 차수 전파).
    ProbeRequest distributions = MakeProbeRequest(ProbeDistributions, 0, Width, Height);
    CheckFloatProbe(
        context, "MicrofacetTerms.Distributions", distributions, UniformTolerance(0.0, 0.0),
        [](uint32 c, const float*, ProbeExpectation& e) {
            const double roughness = ProbeRoughness(c, 0);
            const double alpha = static_cast<float>(roughness * roughness);
            const double mu = ProbeUnit(c, 1, 0);
            const Double3 h = UpperDirection(c, 2);
            const double alphaY = ProbeRoughness(c, 5);
            const double a2 = alpha * alpha;
            const double d = mu * mu * (a2 - 1.0) + 1.0;
            e.value[0] = Ggx(alpha, mu);
            e.absolute[0] =
                e.value[0] * PropagationMargin * (2.0 * 2.0 * FloatUlp / d + Arithmetic(6));
            e.value[1] = GgxAniso(alpha, alphaY, h);
            e.absolute[1] = e.value[1] * PropagationMargin * Arithmetic(14) + 1.0e-6;
            const double sin2 = fmax(1.0 - mu * mu, 1.0e-7);
            const double inverse = 1.0 / fmax(roughness, MinimumClothRoughness);
            e.value[2] = Charlie(roughness, mu);
            e.absolute[2] =
                e.value[2] * PropagationMargin *
                    (0.5 * inverse * (2.0 * FloatUlp / sin2) + PowRelative(0.5 * inverse, sin2)) +
                1.0e-7;
            e.value[3] = InverseGgx(alpha, mu);
            const double argument =
                mu * mu / (sin2 * fmax(a2, MinimumClothRoughness * MinimumClothRoughness));
            e.absolute[3] = e.value[3] * PropagationMargin *
                                (fmin(argument, 100.0) * (2.0 * FloatUlp / sin2 + Arithmetic(4)) +
                                 (3.0 + 2.0 * fmin(argument, 100.0)) * FloatUlp +
                                 4.0 * FloatUlp / sin2 + Arithmetic(6)) +
                            1.0e-7;
        });

    ProbeRequest visibility = MakeProbeRequest(ProbeVisibility, 0, Width, Height);
    double worstApproximation = 0.0;
    CheckFloatProbe(context, "MicrofacetTerms.Visibility", visibility,
                    UniformTolerance(1.0e-7, Arithmetic(12) * 2.0),
                    [&worstApproximation](uint32 c, const float* gpu, ProbeExpectation& e) {
                        const double roughness = ProbeRoughness(c, 0);
                        const double alpha = static_cast<float>(roughness * roughness);
                        const double nv = ProbeUnit(c, 1, 0);
                        const double nl = ProbeUnit(c, 2, 0);
                        const Double3 v = UpperDirection(c, 3);
                        const double alphaY = ProbeRoughness(c, 6);
                        e.value[0] = VisibilityJoint(alpha, nv, nl);
                        e.value[1] = 0.5 / (2.0 * nl * nv + (nl + nv - 2.0 * nl * nv) * alpha);
                        e.value[2] = SmithG1(alpha, nv);
                        e.value[3] = SmithG1Aniso(v, alpha, alphaY);
                        if (nv > 0.01 && nl > 0.01) {
                            worstApproximation =
                                fmax(worstApproximation, fabs(gpu[1] - e.value[0]) / e.value[0]);
                        }

                        if (nv < 1.0e-6 || nl < 1.0e-6) {
                            e.mask = 0xCu;
                        }
                    });
    char detail[128];
    snprintf(detail, sizeof(detail),
             "max relative difference of the linear approximation to the exact visibility %.3f",
             worstApproximation);
    ReportProbe(context, "MicrofacetTerms.VisibilityApproximationMeasured", true, detail);
    visibility.selector = 1;
    CheckFloatProbe(
        context, "MicrofacetTerms.SmithLambdaGgx", visibility,
        UniformTolerance(1.0e-6, Arithmetic(10)), [](uint32 c, const float*, ProbeExpectation& e) {
            const double roughness = ProbeRoughness(c, 0);
            const double alpha = static_cast<float>(roughness * roughness);
            const double mu = static_cast<float>(ProbeUnit(c, 1, 0) + 0.001f);
            const double tan2 = (1.0 - mu * mu) / (mu * mu);
            e.value[0] = 0.5 * (-1.0 + sqrt(1.0 + alpha * alpha * tan2));
            e.absolute[0] = 1.0e-6 + e.value[0] * (Arithmetic(10) +
                                                   2.0 * FloatUlp / fmax(1.0 - mu * mu, 1.0e-7));
            e.mask = 0x1u;
        });

    ProbeRequest fresnel = MakeProbeRequest(ProbeFresnel, 0, Width, Height);
    CheckFloatProbe(context, "MicrofacetTerms.Fresnel", fresnel,
                    UniformTolerance(1.0e-6, Arithmetic(6)),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double roughness = ProbeRoughness(c, 0);
                        const double mu = ProbeUnit(c, 1, 0);
                        const double f0 = ProbeUnit(c, 2, 0);
                        const float etaScaled = ProbeUnit(c, 3, 0) * 2.0f;
                        const float eta = 1.0f + etaScaled;
                        const double etaUsed = (ProbeBits(c, 4, 0) & 1u) != 0 ? 1.0 / eta : eta;
                        const double f90 = ProbeUnit(c, 5, 0);
                        e.value[0] = f0 + (1.0 - f0) * Pow5(1.0 - mu);
                        const double half = static_cast<float>(f0 * 0.5);
                        e.value[1] = half + (f90 - half) * Pow5(1.0 - mu);
                        e.value[2] = FresnelDielectric(etaUsed, mu);
                        const double g2 = etaUsed * etaUsed - 1.0 + mu * mu;
                        e.absolute[2] = 4.0e-6 + (g2 > 0.0 ? 2.0e-7 / sqrt(g2) : 0.0);
                        if (fabs(g2) < 1.0e-5) {
                            e.mask = 0xBu;
                        }

                        const double r2 = static_cast<float>(static_cast<float>(roughness) *
                                                             static_cast<float>(roughness));
                        const double clamped = fmax(r2, static_cast<double>(0.02f));
                        e.value[3] = clamped * clamped;
                    });

    // 상반성 f(l, v) = f(v, l)(GGX 반사·Burley·Chan) — 같은 GPU에서 인자만 바꾼 두 값.
    for (uint32 selector = 0; selector < 2; ++selector) {
        ProbeRequest reciprocity = MakeProbeRequest(ProbeReciprocity, selector, Width, Height);
        Array<float> gpu(context.allocator);
        if (!RunFloatProbe(context,
                           selector == 0 ? "ShadingKernels.ReciprocitySpecularBurley"
                                         : "ShadingKernels.ReciprocityChan",
                           reciprocity, gpu)) {
            continue;
        }

        double worst = 0.0;
        double worstReference = 0.0;
        for (uint32 c = 0; c < CaseCount(reciprocity); ++c) {
            const float* v = gpu.Data() + c * 4u;
            const uint32 pairs = selector == 0 ? 2 : 1;
            for (uint32 p = 0; p < pairs; ++p) {
                worst = fmax(worst, fabs(v[2 * p] - v[2 * p + 1]) / (fabs(v[2 * p]) + 1.0e-6));
            }

            if (selector == 0) {
                const double roughness = ProbeRoughness(c, 0);
                const double alpha = static_cast<float>(roughness * roughness);
                const Double3 l = UpperDirection(c, 2);
                const Double3 view = UpperDirection(c, 5);
                const Double3 h = Normalize(Add(l, view));
                const double reference = Ggx(alpha, h.z) * VisibilityJoint(alpha, view.z, l.z);
                const double a2 = alpha * alpha;
                const double d = h.z * h.z * (a2 - 1.0) + 1.0;
                worstReference =
                    fmax(worstReference,
                         fabs(v[0] - reference) / (reference * PropagationMargin *
                                                       (4.0 * FloatUlp / d + Arithmetic(20)) +
                                                   1.0e-6));
            }
        }

        constexpr double ReciprocityLimit = 1.0e-5;
        snprintf(detail, sizeof(detail),
                 "max relative |f(l,v) - f(v,l)| %.3g (limit %.0e), specular vs double %.2f of "
                 "tolerance",
                 worst, ReciprocityLimit, worstReference);

        ReportProbe(context,
                    selector == 0 ? "ShadingKernels.Reciprocity"
                                  : "ShadingKernels.ReciprocityChanCheck",
                    worst <= ReciprocityLimit && worstReference <= 1.0, detail);
    }

    ProbeRequest diffuse = MakeProbeRequest(ProbeDiffuse, 0, Width, Height);
    Tolerance diffuseTolerance;
    diffuseTolerance.relative[0] = Arithmetic(2);
    // Burley는 입력 방향(GPU 정규화)의 오차가 (1 − cos)⁵ 도함수로 커진다.
    diffuseTolerance.relative[1] = 4.0e-5;
    diffuseTolerance.relative[2] = 5.0e-5;
    diffuseTolerance.absolute[2] = 1.0e-7;
    diffuseTolerance.absolute[3] = Arithmetic(6);
    CheckFloatProbe(context, "DiffuseTerms.LambertBurleyChanWrap", diffuse, diffuseTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double roughness = ProbeRoughness(c, 0);
                        const double mu = ProbeUnit(c, 1, 0);
                        const Double3 l = UpperDirection(c, 2);
                        const Double3 v = UpperDirection(c, 5);
                        const Double3 h = Normalize(Add(l, v));
                        const double albedo = ProbeUnit(c, 8, 0);
                        const double nl = ProbeSigned(c, 9, 0, 0);
                        e.value[0] = albedo / Pi;
                        e.value[1] = albedo * Burley(roughness, v.z, l.z, Dot(l, h));
                        e.value[2] = albedo * Chan(roughness, v.z, l.z, Dot(v, h), h.z);
                        e.value[3] = fmin(fmax((nl + mu) / (1.0 + mu), 0.0), 1.0) / (1.0 + mu);
                    });
}

void RunIntegralProbes(ProbeContext& context) noexcept {
    // ∫D(h)(n·h)dω = 1(α 16 묶음, 반구 격자 64 × 512).
    constexpr uint32 GridWidth = 64;
    constexpr uint32 RowsPerAlpha = 512;
    constexpr uint32 Bands = 16;
    ProbeRequest grid = MakeProbeRequest(ProbeDistributionGrid, 0, GridWidth, RowsPerAlpha * Bands);
    grid.parameters[0] = static_cast<float>(RowsPerAlpha);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "MicrofacetTerms.NormalizationGrid", grid, gpu)) {
        double worst[4] = {};
        for (uint32 band = 0; band < Bands; ++band) {
            double sums[4] = {};
            for (uint32 row = 0; row < RowsPerAlpha; ++row) {
                for (uint32 x = 0; x < GridWidth; ++x) {
                    double solidAngle;
                    (void)GridDirection(x, row, GridWidth, RowsPerAlpha, solidAngle);
                    const float* v =
                        gpu.Data() + ((band * RowsPerAlpha + row) * GridWidth + x) * 4u;
                    for (uint32 k = 0; k < 4; ++k) {
                        sums[k] += v[k] * solidAngle;
                    }
                }
            }

            // α = 1/16 묶음의 Charlie·inverse GGX는 하한·격자 해상도 밖이라 α ≥ 2/16부터 본다.
            for (uint32 k = 0; k < 4; ++k) {
                if (band == 0 && (k == 1 || k == 2)) {
                    continue;
                }

                worst[k] = fmax(worst[k], fabs(sums[k] - 1.0));
            }
        }

        char detail[192];
        snprintf(detail, sizeof(detail),
                 "16 alphas, max |integral - 1|: GGX %.4f, Charlie %.4f, inverse GGX %.4f, "
                 "anisotropic GGX %.4f (limit %.2f)",
                 worst[0], worst[1], worst[2], worst[3], IntegralTolerance);
        ReportProbe(context, "MicrofacetTerms.Normalization",
                    worst[0] <= IntegralTolerance && worst[1] <= IntegralTolerance &&
                        worst[2] <= IntegralTolerance && worst[3] <= IntegralTolerance,
                    detail);
    }

    // 백색 노 시험(furnace): F = 1의 반사 ∫f cos ≤ 1(다중 산란 보정은 SL3), Lambert = 1,
    // Burley·Chan ≤ 1.
    constexpr uint32 FurnaceWidth = 256;
    constexpr uint32 FurnaceRows = 512;
    const double views[3] = {1.0, 0.5, 0.1};
    const double roughnesses[3] = {0.3, 0.6, 1.0};
    double maxSpecular = 0.0;
    double worstLambert = 0.0;
    double maxBurley = 0.0;
    double maxChan = 0.0;
    double worstSpecularReference = 0.0;
    bool ran = true;
    for (const double view : views) {
        for (const double roughness : roughnesses) {
            ProbeRequest furnace = MakeProbeRequest(ProbeFurnace, 0, FurnaceWidth, FurnaceRows);
            furnace.parameters[0] = static_cast<float>(view);
            furnace.parameters[1] = static_cast<float>(roughness);
            furnace.parameters[2] = static_cast<float>(FurnaceRows);
            char label[96];
            snprintf(label, sizeof(label), "ShadingKernels.FurnaceGrid[%.1f,%.1f]", view,
                     roughness);
            if (!RunFloatProbe(context, label, furnace, gpu)) {
                ran = false;
                continue;
            }

            const double alpha =
                static_cast<double>(static_cast<float>(roughness)) * static_cast<float>(roughness);
            const Double3 v{sqrt(fmax(1.0 - view * view, 0.0)), 0.0, view};
            double sums[4] = {};
            double reference = 0.0;
            for (uint32 row = 0; row < FurnaceRows; ++row) {
                for (uint32 x = 0; x < FurnaceWidth; ++x) {
                    double solidAngle;
                    const Double3 l = GridDirection(x, row, FurnaceWidth, FurnaceRows, solidAngle);
                    const float* value = gpu.Data() + (row * FurnaceWidth + x) * 4u;
                    for (uint32 k = 0; k < 4; ++k) {
                        sums[k] += value[k] * solidAngle;
                    }

                    const Double3 h = Normalize(Add(l, v));
                    reference +=
                        Ggx(alpha, h.z) * VisibilityJoint(alpha, v.z, l.z) * l.z * solidAngle;
                }
            }

            maxSpecular = fmax(maxSpecular, sums[0]);
            worstSpecularReference =
                fmax(worstSpecularReference, fabs(sums[0] - reference) / reference);
            maxBurley = fmax(maxBurley, sums[1]);
            maxChan = fmax(maxChan, sums[2]);
            worstLambert = fmax(worstLambert, fabs(sums[3] - 1.0));
        }
    }

    if (ran) {
        char detail[256];
        snprintf(detail, sizeof(detail),
                 "9 views x roughness: max specular albedo %.4f (<= 1 + %.2f), specular vs double "
                 "integral %.4f, Lambert "
                 "|E - 1| %.4f, max Burley %.4f, max Chan %.4f",
                 maxSpecular, IntegralTolerance, worstSpecularReference, worstLambert, maxBurley,
                 maxChan);
        ReportProbe(context, "ShadingKernels.WhiteFurnace",
                    maxSpecular <= 1.0 + IntegralTolerance &&
                        worstSpecularReference <= IntegralTolerance &&
                        worstLambert <= IntegralTolerance && maxBurley <= 1.0 + IntegralTolerance &&
                        maxChan <= 1.0 + ChanEnergyExcess,
                    detail);
    }
}

// split-sum DFG(Karis 2013): GGX 중요도 표본(Hammersley 4096, double)으로 (scale, bias)를 적분한다.
void SplitSumReference(double nv, double roughness, double& scale, double& bias) noexcept {
    constexpr uint32 Samples = 4096;
    const double alpha = fmax(roughness, 0.02) * fmax(roughness, 0.02);
    const Double3 v{sqrt(fmax(1.0 - nv * nv, 0.0)), 0.0, nv};
    scale = 0.0;
    bias = 0.0;
    for (uint32 i = 0; i < Samples; ++i) {
        uint32 reversed = 0;
        for (uint32 bit = 0; bit < 32; ++bit) {
            reversed |= ((i >> bit) & 1u) << (31 - bit);
        }

        const double u0 = (i + 0.5) / Samples;
        const double u1 = reversed * ldexp(1.0, -32);
        const double phi = 2.0 * Pi * u1;
        const double cosTheta = sqrt((1.0 - u0) / (1.0 + (alpha * alpha - 1.0) * u0));
        const double sinTheta = sqrt(fmax(1.0 - cosTheta * cosTheta, 0.0));
        const Double3 h{sinTheta * cos(phi), sinTheta * sin(phi), cosTheta};
        const double vh = Dot(v, h);
        const Double3 l = Subtract(Scale(h, 2.0 * vh), v);
        if (l.z <= 0.0) {
            continue;
        }

        const double weight = VisibilityJoint(alpha, nv, l.z) * 4.0 * l.z * vh / h.z;
        const double fc = Pow5(1.0 - vh);
        scale += (1.0 - fc) * weight;
        bias += fc * weight;
    }

    scale /= Samples;
    bias /= Samples;
}

void RunEnvironmentProbes(ProbeContext& context) noexcept {
    ProbeRequest environment = MakeProbeRequest(ProbeEnvironmentBrdf, 0, 32, 32);
    double worstIntegral = 0.0;
    double worstAt[4] = {};
    double worstNonGrazing = 0.0;
    CheckFloatProbe(
        context, "EnvironmentBrdf.Approx", environment, UniformTolerance(1.0e-6, 2.0e-5),
        [&worstIntegral, &worstAt, &worstNonGrazing](uint32 c, const float* gpu,
                                                     ProbeExpectation& e) {
            const double roughness = ProbeRoughness(c, 0);
            const double nv = ProbeUnit(c, 1, 0);
            const double r[4] = {roughness * -1.0 + 1.0, roughness * -0.0275 + 0.0425,
                                 roughness * -0.572 + 1.04, roughness * 0.022 - 0.04};
            const double a004 =
                fmin(r[0] * r[0], exp2(static_cast<double>(-9.28f) * nv)) * r[0] + r[1];
            e.value[0] = -1.04 * a004 + r[2];
            e.value[1] = 1.04 * a004 + r[3];
            const double f0 = ProbeUnit(c, 2, 0);
            const double f90 = ProbeUnit(c, 3, 0);
            e.value[2] = f0 * e.value[0] + f90 * e.value[1];
            e.value[3] = 0.04 * e.value[0] + f90 * e.value[1];
            if (nv > 0.02) {
                double scale;
                double bias;
                SplitSumReference(nv, roughness, scale, bias);
                const double difference = fmax(fabs(gpu[0] - scale), fabs(gpu[1] - bias));
                if (nv >= 0.25) {
                    worstNonGrazing = fmax(worstNonGrazing, difference);
                }

                if (difference > worstIntegral) {
                    worstIntegral = difference;
                    worstAt[0] = nv;
                    worstAt[1] = roughness;
                    worstAt[2] = scale;
                    worstAt[3] = bias;
                }
            }
        });
    // Karis 2014 근사는 높이 상관 Smith split-sum 수치 적분과 거친 표면·스치는 각에서 크게
    // 다르다(측정 0.36 — 비상관 Schlick–Smith 적분과도 0.15) — 측정해 기록하고 SL3 LUT가
    // 대신한다(엔진 셰이더 7.8절). 근사 식이 깨진 경우(0.5 넘는 어긋남)만 막는다.
    constexpr double GrossLimit = 0.5;
    char detail[256];
    snprintf(detail, sizeof(detail),
             "max |approx - split-sum integral| %.4f at NoV %.3f roughness %.3f (integral %.4f, "
             "%.4f), NoV >= 0.25 "
             "max %.4f (recorded; gross limit %.2f)",
             worstIntegral, worstAt[0], worstAt[1], worstAt[2], worstAt[3], worstNonGrazing,
             GrossLimit);
    ReportProbe(context, "EnvironmentBrdf.ApproxVsIntegral", worstIntegral <= GrossLimit, detail);
}

void RunSurfaceProbes(ProbeContext& context) noexcept {
    // SurfaceAttributes 기본값(UE 5.8 머티리얼 속성 기본값 — SurfaceAttributes.rsh 주석).
    ProbeRequest defaults = MakeProbeRequest(ProbeSurfaceDefaults, 0, 6, 1);
    CheckFloatProbe(context, "SurfaceAttributes.Defaults", defaults, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double table[6][4] = {{0.0, 0.0, 0.0, 0.0},
                                                    {0.5, 0.5, 1.0, 1.0},
                                                    {0.0, 0.0, 1.0, 1.0},
                                                    {0.0, 0.0, 0.0, 1.0},
                                                    {1.0, static_cast<float>(0.1), 0.0, 0.0},
                                                    {1.0, 0.0, 0.0, 0.0}};
                        for (uint32 k = 0; k < 4; ++k) {
                            e.value[k] = table[c][k];
                        }
                    });

    for (uint32 selector = 0; selector < 3; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeShadingParameters, selector, Width, Height);
        const char* names[3] = {"SurfaceShading.DiffuseF90", "SurfaceShading.F0Alpha",
                                "SurfaceShading.RoughnessModel"};
        CheckFloatProbe(
            context, names[selector], request, UniformTolerance(1.0e-7, Arithmetic(4)),
            [selector](uint32 c, const float*, ProbeExpectation& e) {
                const double base[3] = {ProbeUnit(c, 2, 0), ProbeUnit(c, 3, 0), ProbeUnit(c, 4, 0)};
                const uint32 pick = ProbeBits(c, 5, 0);
                const double metallic =
                    (pick & 3u) == 0 ? 0.0 : ((pick & 3u) == 1 ? 1.0 : ProbeUnit(c, 6, 0));
                const double specular = ((pick >> 2) & 1u) == 0 ? 0.5 : ProbeUnit(c, 7, 0);
                const double roughness = ProbeUnit(c, 8, 0);
                const uint32 model = (pick >> 3) % 4u;
                const bool unlit = model == 0;
                const double dielectric = static_cast<float>(0.08f * static_cast<float>(specular));
                const double clamped = fmax(roughness, 0.02);
                switch (selector) {
                case 0:
                    for (uint32 k = 0; k < 3; ++k) {
                        e.value[k] = unlit ? 0.0 : base[k] * (1.0 - metallic);
                    }

                    e.value[3] = unlit ? 0.0 : 1.0;
                    break;
                case 1:
                    for (uint32 k = 0; k < 3; ++k) {
                        e.value[k] = unlit ? 0.0 : dielectric + (base[k] - dielectric) * metallic;
                    }

                    e.value[3] = clamped * clamped;
                    break;
                default:
                    e.value[0] = clamped;
                    e.value[1] = model;
                    e.value[2] = model <= 1 ? 1.0 : 0.0;
                    e.mask = 0x7u;
                    break;
                }
            });
    }
}

// VNDF 표본 h의 pdf(Heitz 2018 식 1)다.
double VisibleNormalPdf(const Double3& v, double ax, double ay, const Double3& h) noexcept {
    return SmithG1Aniso(v, ax, ay) * fmax(Dot(v, h), 0.0) * GgxAniso(ax, ay, h) / v.z;
}

// Heitz 2018 Listing 1(double)이다.
Double3 VisibleNormalSample(const Double3& v, double ax, double ay, double u0, double u1) noexcept {
    const Double3 vh = Normalize(Double3{ax * v.x, ay * v.y, v.z});
    const double lensq = vh.x * vh.x + vh.y * vh.y;
    const Double3 t1 =
        lensq > 0.0 ? Scale(Double3{-vh.y, vh.x, 0.0}, 1.0 / sqrt(lensq)) : Double3{1.0, 0.0, 0.0};
    const Double3 t2 = Cross(vh, t1);
    const double r = sqrt(u0);
    const double phi = 2.0 * Pi * u1;
    const double p1 = r * cos(phi);
    const double s = 0.5 * (1.0 + vh.z);
    const double p2 = (1.0 - s) * sqrt(fmax(1.0 - p1 * p1, 0.0)) + s * r * sin(phi);
    const Double3 nh =
        Add(Add(Scale(t1, p1), Scale(t2, p2)), Scale(vh, sqrt(fmax(1.0 - p1 * p1 - p2 * p2, 0.0))));
    return Normalize(Double3{ax * nh.x, ay * nh.y, fmax(0.0, nh.z)});
}

void RunVisibleNormalProbes(ProbeContext& context) noexcept {
    constexpr uint32 BigSide = 1024;
    constexpr uint32 Bins = 32;
    constexpr uint32 SubSamples = 8;
    constexpr double MinimumExpected = 20.0;
    const double thetas[3] = {0.2, 1.0, 1.45};
    const double alphas[3][2] = {{0.3, 0.3}, {0.5, 0.15}, {0.08, 0.08}};
    char label[96];
    for (uint32 index = 0; index < 3; ++index) {
        const Double3 v = Normalize(Double3{sin(thetas[index]), 0.0, cos(thetas[index])});
        const double ax = static_cast<float>(alphas[index][0]);
        const double ay = static_cast<float>(alphas[index][1]);
        ProbeRequest sample = MakeProbeRequest(ProbeVndfSample, 0, BigSide, BigSide);
        sample.values[0] = static_cast<float>(v.x);
        sample.values[1] = static_cast<float>(v.y);
        sample.values[2] = static_cast<float>(v.z);
        sample.parameters[0] = static_cast<float>(ax);
        sample.parameters[1] = static_cast<float>(ay);
        const Double3 vf{sample.values[0], sample.values[1], sample.values[2]};
        // 칸(cos θ = 1 − s², φ)의 기대 확률(칸 안 8 × 8 중점 적분)이다.
        double expected[Bins * Bins];
        for (uint32 by = 0; by < Bins; ++by) {
            for (uint32 bx = 0; bx < Bins; ++bx) {
                double probability = 0.0;
                for (uint32 sy = 0; sy < SubSamples; ++sy) {
                    for (uint32 sx = 0; sx < SubSamples; ++sx) {
                        double solidAngle;
                        const Double3 h =
                            GridDirection(bx * SubSamples + sx, by * SubSamples + sy,
                                          Bins * SubSamples, Bins * SubSamples, solidAngle);
                        probability += VisibleNormalPdf(vf, ax, ay, h) * solidAngle;
                    }
                }

                expected[by * Bins + bx] = probability;
            }
        }

        double counts[Bins * Bins] = {};
        snprintf(label, sizeof(label), "SampleWarping.SampleGgxVisibleNormal[%u]", index);
        CheckFloatProbe(
            context, label, sample, UniformTolerance(2.0 * SpecTrigAbsolute * 2.0, 0.0),
            [&](uint32 c, const float* gpu, ProbeExpectation& e) {
                const Double3 h =
                    VisibleNormalSample(vf, ax, ay, ProbeUnit(c, 0, 0), ProbeUnit(c, 1, 0));
                e.value[0] = h.x;
                e.value[1] = h.y;
                e.value[2] = h.z;
                e.mask = 0x7u;
                const double s = sqrt(fmax(1.0 - static_cast<double>(gpu[2]), 0.0));
                double phi =
                    atan2(static_cast<double>(gpu[1]), static_cast<double>(gpu[0])) / (2.0 * Pi);
                phi -= floor(phi);
                const uint32 by = s >= 1.0 ? Bins - 1 : static_cast<uint32>(s * Bins);
                const uint32 bx = phi >= 1.0 ? Bins - 1 : static_cast<uint32>(phi * Bins);
                counts[by * Bins + bx] += 1.0;
            });
        // 기대 표본이 적은 칸은 하나로 합친다(χ² 근사 조건).
        const double total = static_cast<double>(CaseCount(sample));
        double chi = 0.0;
        double restExpected = 0.0;
        double restCount = 0.0;
        double probabilitySum = 0.0;
        uint32 degrees = 0;
        for (uint32 bin = 0; bin < Bins * Bins; ++bin) {
            probabilitySum += expected[bin];
            const double e = expected[bin] * total;
            if (e < MinimumExpected) {
                restExpected += e;
                restCount += counts[bin];
                continue;
            }

            chi += (counts[bin] - e) * (counts[bin] - e) / e;
            ++degrees;
        }

        if (restExpected >= MinimumExpected) {
            chi += (restCount - restExpected) * (restCount - restExpected) / restExpected;
            ++degrees;
        }

        const double critical = ChiSquareCritical(static_cast<double>(degrees - 1));
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "2^20 samples, chi-square %.1f in %u bins (critical %.1f), pdf sum over bins %.4f",
                 chi, degrees, critical, probabilitySum);
        snprintf(label, sizeof(label), "SampleWarping.VisibleNormalDistribution[%u]", index);
        ReportProbe(context, label,
                    chi <= critical && fabs(probabilitySum - 1.0) <= IntegralTolerance, detail);

        // GPU pdf(D·G1는 MicrofacetTerms)의 반구 적분 = 1.
        constexpr uint32 PdfWidth = 256;
        constexpr uint32 PdfRows = 512;
        ProbeRequest pdf = sample;
        pdf.function = ProbeVndfPdfGrid;
        pdf.width = PdfWidth;
        pdf.height = PdfRows;
        pdf.parameters[2] = static_cast<float>(PdfRows);
        Array<float> gpu(context.allocator);
        snprintf(label, sizeof(label), "SampleWarping.GgxVisibleNormalPdfGrid[%u]", index);
        if (RunFloatProbe(context, label, pdf, gpu)) {
            double integral = 0.0;
            for (uint32 row = 0; row < PdfRows; ++row) {
                for (uint32 x = 0; x < PdfWidth; ++x) {
                    double solidAngle;
                    (void)GridDirection(x, row, PdfWidth, PdfRows, solidAngle);
                    integral += gpu[(row * PdfWidth + x) * 4u] * solidAngle;
                }
            }

            snprintf(detail, sizeof(detail),
                     "integral of the GPU pdf over the hemisphere %.4f (limit 1 +- %.2f)", integral,
                     IntegralTolerance);
            snprintf(label, sizeof(label), "SampleWarping.GgxVisibleNormalPdf[%u]", index);
            ReportProbe(context, label, fabs(integral - 1.0) <= IntegralTolerance, detail);
        }
    }
}

} // namespace

void RunShadingProbes(ProbeContext& context) noexcept {
    RunTermProbes(context);
    RunIntegralProbes(context);
    RunEnvironmentProbes(context);
    RunSurfaceProbes(context);
    RunVisibleNormalProbes(context);
}

} // namespace sandbox::engine_shaders
