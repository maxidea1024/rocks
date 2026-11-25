#include "EngineShaderProbe.h"

#include <Base/NumericLimits.h>

// A02 난수·잡음·저불일치·표본 사상·SH·광선 교차의 CPU 기준과 통계
// 시험이다(Docs/Design/EngineShaders 7.3절, 8.1절 (S)·(P)). 통계 판정은 고정 seed·고정 유의
// 수준(0.001)이라 결과가 결정적이다(8.4절 — 시간·경쟁에 기대지 않는다). 큰 표본은 1024 × 1024 2D
// 배치다.
namespace sandbox::engine_shaders {

namespace {

constexpr uint32 BigSide = 1024;
constexpr uint32 Width = 256;
constexpr uint32 Height = 256;
constexpr double Pi = 3.14159265358979323846;
constexpr double TwoPi = 2.0 * Pi;
// 히스토그램 칸 수(한 축)다.
constexpr uint32 HistogramSide = 32;
// 표본 사상 비교의 삼각 함수 전파 상한(sin·cos 절대 2^-11 × 세 번)이다.
constexpr double SamplingTolerance = 3.0 * SpecTrigAbsolute;
// float 계수·근 계산의 상대 오차(8 ulp)다(근 허용 오차 전파의 ε).
constexpr double RootEpsilon = 8.0 * 1.1920928955078125e-7;
// 광선 교차의 애매한 경계(이 안의 사례는 float·double 판정이 갈릴 수 있어 뺀다)다.
constexpr double RayBoundaryMargin = 0.05;

double Wrap01(double x) noexcept {
    return x - floor(x);
}

// [0,1)² 히스토그램의 χ²다(칸마다 기대 확률이 같다).
double ChiSquareUniform(const Array<uint32>& counts, uint64 samples) noexcept {
    const double expected = static_cast<double>(samples) / counts.Size();
    double sum = 0.0;
    for (const uint32 count : counts) {
        const double d = count - expected;
        sum += d * d / expected;
    }

    return sum;
}

void ReportChiSquare(ProbeContext& context, const char* label, const Array<uint32>& counts,
                     uint64 samples, uint32 outside) noexcept {
    const double chi = ChiSquareUniform(counts, samples);
    const double critical = ChiSquareCritical(static_cast<double>(counts.Size() - 1));
    char detail[160];
    snprintf(detail, sizeof(detail),
             "%llu samples in %zu bins, chi-square %.1f (critical %.1f at p 0.001), outside %u",
             static_cast<unsigned long long>(samples), counts.Size(), chi, critical, outside);
    ReportProbe(context, label, chi <= critical && outside == 0, detail);
}

void RunRandomProbes(ProbeContext& context) noexcept {
    // UintToUnitFloat(정확, 최대 1 − 2^-24)·RadicalInverseBase2(정확)·Halton3(나눗셈 1번).
    ProbeRequest unit = MakeProbeRequest(ProbeUnitFloat, 0, Width, Height);
    Tolerance unitTolerance = UniformTolerance(0.0, 0.0);
    unitTolerance.relative[3] = Arithmetic(1);
    CheckFloatProbe(context, "RandomNumbers.UintToUnitFloat", unit, unitTolerance,
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const uint32 bits = c == 0 ? rocks::NumericLimits<uint32>::Max()
                                                   : (c == 1 ? 0u : ProbeBits(c, 0, 0));
                        e.value[0] = (bits >> 8) * ldexp(1.0, -24);
                        e.value[1] = e.value[0];
                        uint32 reversed = 0;
                        for (uint32 bit = 0; bit < 32; ++bit) {
                            reversed |= ((c >> bit) & 1u) << (31 - bit);
                        }

                        e.value[2] = (reversed >> 8) * ldexp(1.0, -24);
                        uint32 numerator = 0;
                        uint32 denominator = 1;
                        for (uint32 index = c; index > 0; index /= 3) {
                            numerator = numerator * 3 + index % 3;
                            denominator *= 3;
                        }

                        e.value[3] = static_cast<double>(numerator) / denominator;
                    });

    // 상태 있는 PCG 수열의 [0,1) 균일성(χ², 2^20 표본, 1024 칸).
    ProbeRequest stream = MakeProbeRequest(ProbeRandomStream, 0, BigSide, BigSide, 0x5eed);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "RandomNumbers.RandomPcgNextFloat", stream, gpu)) {
        Array<uint32> counts(context.allocator);
        if (counts.TryResize(HistogramSide * HistogramSide)) {
            for (uint32& count : counts) {
                count = 0;
            }

            uint32 outside = 0;
            for (uint32 c = 0; c < CaseCount(stream); ++c) {
                for (uint32 k = 0; k < 4; ++k) {
                    const float v = gpu[c * 4u + k];
                    if (!(v >= 0.0f && v < 1.0f)) {
                        ++outside;
                        continue;
                    }

                    if (k == 0) {
                        ++counts[static_cast<uint32>(v * counts.Size())];
                    }
                }
            }

            ReportChiSquare(context, "RandomNumbers.Uniformity", counts, CaseCount(stream),
                            outside);
        }
    }

    // interleaved gradient noise(Jimenez 2014): 원 위 거리(0과 1은 같다)로 대조, 64 프레임 주기는
    // bit 단위.
    ProbeRequest noise = MakeProbeRequest(ProbeScreenNoise, 0, 512, 512);
    noise.parameters[0] = 13.0f;
    Array<float> noiseValues(context.allocator);
    if (RunFloatProbe(context, "ScreenNoise.InterleavedGradientNoise", noise, noiseValues)) {
        double worst = 0.0;
        uint32 periodErrors = 0;
        const double frames[3] = {0.0, 13.0, 14.0};
        for (uint32 c = 0; c < CaseCount(noise); ++c) {
            const double x = c % noise.width;
            const double y = c / noise.width;
            const float* v = noiseValues.Data() + c * 4u;
            if (FloatToBits(v[1]) != FloatToBits(v[2])) {
                ++periodErrors;
            }

            const uint32 channels[3] = {0, 1, 3};
            for (uint32 k = 0; k < 3; ++k) {
                const double offset = static_cast<double>(5.588238f) * frames[k];
                const double inner = Wrap01((x + offset) * static_cast<double>(0.06711056f) +
                                            (y + offset) * static_cast<double>(0.00583715f));
                // 안쪽 frac이 0·1 경계면 바깥 값이 52.98배 폭으로 갈린다(IGN 정의의 불연속) — 뺀다.
                constexpr double InnerWrapMargin = 1.0e-4;
                if (inner < InnerWrapMargin || inner > 1.0 - InnerWrapMargin) {
                    continue;
                }
                const double expected = Wrap01(static_cast<double>(52.9829189f) * inner);
                double d = fabs(v[channels[k]] - expected);
                d = fmin(d, 1.0 - d);
                worst = fmax(worst, d);
            }
        }

        // dot(≈ 90) 결과의 ulp(≈ 1e-5)가 52.98배로 커진다(사양 산술 상한 전파).
        constexpr double NoiseTolerance = 2.0e-3;
        char detail[160];
        snprintf(detail, sizeof(detail),
                 "%u pixels, max circular error %.3g (tolerance %.0e), period-64 bit errors %u",
                 CaseCount(noise), worst, NoiseTolerance, periodErrors);
        ReportProbe(context, "ScreenNoise.InterleavedGradientNoiseCheck",
                    worst <= NoiseTolerance && periodErrors == 0, detail);
    }
}

// 2D 별 불일치(star discrepancy) — 원점 상자 [0,u)×[0,v)의 열린·닫힌 셈 둘 다(정확, O(N³))다.
double StarDiscrepancy(const double* x, const double* y, uint32 n) noexcept {
    double worst = 0.0;
    for (uint32 i = 0; i <= n; ++i) {
        const double u = i < n ? x[i] : 1.0;
        for (uint32 j = 0; j <= n; ++j) {
            const double v = j < n ? y[j] : 1.0;
            uint32 open = 0;
            uint32 closed = 0;
            for (uint32 k = 0; k < n; ++k) {
                open += (x[k] < u && y[k] < v) ? 1u : 0u;
                closed += (x[k] <= u && y[k] <= v) ? 1u : 0u;
            }

            const double area = u * v;
            worst = fmax(worst, fmax(area - static_cast<double>(open) / n,
                                     static_cast<double>(closed) / n - area));
        }
    }

    return worst;
}

void RunLowDiscrepancyProbes(ProbeContext& context) noexcept {
    constexpr uint32 Points = 256;
    constexpr uint32 SobolSeed = 1234567u;
    ProbeRequest request = MakeProbeRequest(ProbeLowDiscrepancy, 0, 16, 16, SobolSeed);
    request.parameters[0] = static_cast<float>(Points);
    Tolerance tolerance = UniformTolerance(0.0, 0.0);
    tolerance.relative[0] = Arithmetic(1);
    Array<float> hammersley(context.allocator);
    Array<float> sobol(context.allocator);
    bool ran = RunFloatProbe(context, "LowDiscrepancy.HammersleyR2", request, hammersley);
    request.selector = 1;
    ran = ran && RunFloatProbe(context, "LowDiscrepancy.SobolOwen2D", request, sobol);
    if (!ran) {
        return;
    }

    double hx[Points];
    double hy[Points];
    uint32 valueErrors = 0;
    uint32 strata = 0;
    for (uint32 c = 0; c < Points; ++c) {
        hx[c] = hammersley[c * 4u];
        hy[c] = hammersley[c * 4u + 1];
        if (fabs(hx[c] - static_cast<double>(c) / Points) > Arithmetic(1) ||
            sobol[c * 4u + 2] != hammersley[c * 4u + 1]) {
            ++valueErrors;
        }

        const double r2x = ((0x80000000u + c * 3242174889u) >> 8) * ldexp(1.0, -24);
        const double r2y = ((0x80000000u + c * 2447445414u) >> 8) * ldexp(1.0, -24);
        if (hammersley[c * 4u + 2] != r2x || hammersley[c * 4u + 3] != r2y) {
            ++valueErrors;
        }
    }

    // Owen scramble한 Sobol 첫 2^8 점은 (0, 8, 2)-망이다: 모든 2^a × 2^(8−a) 기본 구간에 점이
    // 정확히 하나.
    for (uint32 a = 0; a <= 8; ++a) {
        uint32 occupied[Points] = {};
        for (uint32 c = 0; c < Points; ++c) {
            const uint32 cx = static_cast<uint32>(sobol[c * 4u] * (1u << a));
            const uint32 cy = static_cast<uint32>(sobol[c * 4u + 1] * (1u << (8 - a)));
            ++occupied[cx * (1u << (8 - a)) + cy];
        }

        for (const uint32 count : occupied) {
            strata += count != 1 ? 1u : 0u;
        }
    }

    double sx[Points];
    double sy[Points];
    for (uint32 c = 0; c < Points; ++c) {
        sx[c] = sobol[c * 4u];
        sy[c] = sobol[c * 4u + 1];
    }

    const double hammersleyDiscrepancy = StarDiscrepancy(hx, hy, Points);
    const double sobolDiscrepancy = StarDiscrepancy(sx, sy, Points);
    // 2^m 점의 별 불일치 상한(log2 N + 2) / N(Hammersley·(0, m, 2)-망 — Niederreiter 1992 정리 4.10
    // 꼴)이다.
    const double bound = (log2(static_cast<double>(Points)) + 2.0) / Points;
    char detail[192];
    snprintf(detail, sizeof(detail),
             "%u points, value errors %u, Sobol empty or crowded strata %u, star discrepancy "
             "Hammersley %.4f Sobol "
             "%.4f (bound %.4f)",
             Points, valueErrors, strata, hammersleyDiscrepancy, sobolDiscrepancy, bound);
    ReportProbe(context, "LowDiscrepancy.Properties",
                valueErrors == 0 && strata == 0 && hammersleyDiscrepancy <= bound &&
                    sobolDiscrepancy <= bound,
                detail);
}

// 표본 사상의 double 기준(Shirley–Chiu 동심원, PBRT 13.6절 식)이다.
void SampleReference(uint32 selector, double u0, double u1, double cosThetaMax,
                     double out[3]) noexcept {
    if (selector == 0) {
        const double ax = 2.0 * u0 - 1.0;
        const double ay = 2.0 * u1 - 1.0;
        out[2] = 0.0;
        if (ax == 0.0 && ay == 0.0) {
            out[0] = out[1] = 0.0;
            return;
        }

        double r;
        double phi;
        if (fabs(ax) > fabs(ay)) {
            r = ax;
            phi = Pi * 0.25 * (ay / ax);
        } else {
            r = ay;
            phi = Pi * 0.5 - Pi * 0.25 * (ax / ay);
        }

        out[0] = r * cos(phi);
        out[1] = r * sin(phi);
        return;
    }

    double z;
    if (selector == 1) {
        z = 1.0 - 2.0 * u0;
    } else if (selector == 2) {
        z = u0;
    } else if (selector == 3) {
        double disk[3];
        SampleReference(0, u0, u1, 0.0, disk);
        out[0] = disk[0];
        out[1] = disk[1];
        out[2] = sqrt(fmax(1.0 - disk[0] * disk[0] - disk[1] * disk[1], 0.0));
        return;
    } else {
        z = 1.0 + (cosThetaMax - 1.0) * u0;
    }

    const double r = sqrt(fmax(1.0 - z * z, 0.0));
    out[0] = r * cos(TwoPi * u1);
    out[1] = r * sin(TwoPi * u1);
    out[2] = z;
}

void RunSamplingProbes(ProbeContext& context) noexcept {
    constexpr double ConeCos = 0.6;
    const char* names[5] = {"SampleWarping.ConcentricSquareToDisk", "SampleWarping.UniformSphere",
                            "SampleWarping.UniformHemisphere", "SampleWarping.CosineHemisphere",
                            "SampleWarping.UniformCone"};
    for (uint32 selector = 0; selector < 5; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeSampleWarping, selector, BigSide, BigSide);
        request.parameters[0] = static_cast<float>(ConeCos);
        Array<uint32> counts(context.allocator);
        if (!counts.TryResize(HistogramSide * HistogramSide)) {
            continue;
        }

        for (uint32& count : counts) {
            count = 0;
        }

        uint32 outside = 0;
        CheckFloatProbe(
            context, names[selector], request, UniformTolerance(SamplingTolerance, 0.0),
            [selector, &counts, &outside](uint32 c, const float* gpu, ProbeExpectation& e) {
                const double u0 = ProbeUnit(c, 0, 0);
                const double u1 = ProbeUnit(c, 1, 0);
                double reference[3];
                SampleReference(selector, u0, u1, static_cast<float>(ConeCos), reference);
                for (uint32 k = 0; k < 3; ++k) {
                    e.value[k] = reference[k];
                }

                e.mask = selector == 0 ? 0x3u : 0x7u;
                // 히스토그램 칸(등확률): 디스크 r², 구·반구·원뿔 z, 코사인 z² × φ.
                double a;
                if (selector == 0) {
                    a = static_cast<double>(gpu[0]) * gpu[0] + static_cast<double>(gpu[1]) * gpu[1];
                } else if (selector == 1) {
                    a = (1.0 - gpu[2]) * 0.5;
                } else if (selector == 2) {
                    a = gpu[2];
                } else if (selector == 3) {
                    a = static_cast<double>(gpu[2]) * gpu[2];
                } else {
                    a = (1.0 - gpu[2]) / (1.0 - ConeCos);
                }

                const double phi =
                    Wrap01(atan2(static_cast<double>(gpu[1]), static_cast<double>(gpu[0])) / TwoPi);
                // 경계(r = 1·z = ±1)는 cos² + sin²의 반올림으로 1 ulp 넘을 수 있다.
                constexpr double BoundarySlack = 1.0e-6;
                if (!(a >= -BoundarySlack && a <= 1.0 + BoundarySlack)) {
                    ++outside;
                    return;
                }

                a = a < 0.0 ? 0.0 : a;

                const uint32 ia =
                    a >= 1.0 ? HistogramSide - 1 : static_cast<uint32>(a * HistogramSide);
                const uint32 ip =
                    phi >= 1.0 ? HistogramSide - 1 : static_cast<uint32>(phi * HistogramSide);
                ++counts[ia * HistogramSide + ip];
            });
        char label[96];
        snprintf(label, sizeof(label), "%sDistribution", names[selector]);
        ReportChiSquare(context, label, counts, CaseCount(request), outside);
    }

    // pdf 값(상수·cos/π·원뿔)이다.
    ProbeRequest pdf = MakeProbeRequest(ProbeSampleWarping, 5, 64, 64);
    pdf.parameters[0] = static_cast<float>(ConeCos);
    CheckFloatProbe(context, "SampleWarping.Pdfs", pdf, UniformTolerance(0.0, Arithmetic(2)),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double mu = ProbeUnit(c, 0, 0) * 2.0f - 1.0f;
                        e.value[0] = 0.25 / Pi;
                        e.value[1] = 0.5 / Pi;
                        e.value[2] = fmax(mu, 0.0) / Pi;
                        e.value[3] = 0.5 / Pi / (1.0 - static_cast<float>(ConeCos));
                    });
}

// 실수 SH 기저(Sloan 2008)의 double 기준이다(순서는 셰이더와 같다).
void ShBasis(const Double3& d, double out[9]) noexcept {
    const double b0 = 0.28209479177387814;
    const double b1 = 0.48860251190291992;
    const double p = 1.09254843059207907;
    const double zonal = 0.31539156525252001;
    const double diff = 0.54627421529603954;
    out[0] = b0;
    out[1] = b1 * d.y;
    out[2] = b1 * d.z;
    out[3] = b1 * d.x;
    out[4] = p * d.x * d.y;
    out[5] = p * d.y * d.z;
    out[6] = zonal * (3.0 * d.z * d.z - 1.0);
    out[7] = p * d.x * d.z;
    out[8] = diff * (d.x * d.x - d.y * d.y);
}

void RunSphericalHarmonicsProbes(ProbeContext& context) noexcept {
    // 기저 값(무작위 방향)과 덧셈 정리 Σ Y² = 9 / 4π.
    for (uint32 selector = 0; selector < 3; ++selector) {
        ProbeRequest request =
            MakeProbeRequest(ProbeSphericalHarmonicsBasis, selector, Width, Height);
        const char* names[3] = {"SphericalHarmonics.BasisBand01", "SphericalHarmonics.BasisBand2",
                                "SphericalHarmonics.ProjectEvaluateDot"};
        CheckFloatProbe(context, names[selector], request, UniformTolerance(4.0e-6, 0.0),
                        [selector](uint32 c, const float*, ProbeExpectation& e) {
                            double y[9];
                            ShBasis(ProbeDirection(c, 0, 0), y);
                            if (selector < 2) {
                                for (uint32 k = 0; k < 4; ++k) {
                                    e.value[k] = y[selector * 4 + k];
                                }

                                return;
                            }

                            e.value[0] = y[8];
                            e.value[1] = 2.0 * 9.0 / (4.0 * Pi);
                            e.value[2] = 4.0 / (4.0 * Pi);
                            e.value[3] = 1.5 * 4.0 / (4.0 * Pi);
                        });
    }

    // 직교성 ∫Y_i Y_j dω = δ_ij(GPU 기저를 θ·φ 격자 칸 중심에서 읽어 CPU가 적분).
    constexpr uint32 PhiCells = 512;
    constexpr uint32 ThetaCells = 256;
    Array<float> band01(context.allocator);
    Array<float> band2(context.allocator);
    Array<float> last(context.allocator);
    ProbeRequest grid = MakeProbeRequest(ProbeSphericalHarmonicsGrid, 0, PhiCells, ThetaCells);
    grid.parameters[1] = static_cast<float>(ThetaCells);
    bool ran = RunFloatProbe(context, "SphericalHarmonics.GridBand01", grid, band01);
    grid.selector = 1;
    ran = ran && RunFloatProbe(context, "SphericalHarmonics.GridBand2", grid, band2);
    grid.selector = 2;
    ran = ran && RunFloatProbe(context, "SphericalHarmonics.GridLast", grid, last);
    if (ran) {
        double gram[9][9] = {};
        for (uint32 row = 0; row < ThetaCells; ++row) {
            const double theta = (row + 0.5) * Pi / ThetaCells;
            const double weight = sin(theta) * (Pi / ThetaCells) * (TwoPi / PhiCells);
            for (uint32 column = 0; column < PhiCells; ++column) {
                const uint32 c = row * PhiCells + column;
                double y[9];
                for (uint32 k = 0; k < 4; ++k) {
                    y[k] = band01[c * 4u + k];
                    y[4 + k] = band2[c * 4u + k];
                }

                y[8] = last[c * 4u];
                for (uint32 i = 0; i < 9; ++i) {
                    for (uint32 j = 0; j < 9; ++j) {
                        gram[i][j] += y[i] * y[j] * weight;
                    }
                }
            }
        }

        double worst = 0.0;
        for (uint32 i = 0; i < 9; ++i) {
            for (uint32 j = 0; j < 9; ++j) {
                worst = fmax(worst, fabs(gram[i][j] - (i == j ? 1.0 : 0.0)));
            }
        }

        constexpr double OrthogonalityLimit = 1.0e-3;
        char detail[128];
        snprintf(detail, sizeof(detail), "%u x %u grid, max |G - I| %.3g (limit %.0e)", PhiCells,
                 ThetaCells, worst, OrthogonalityLimit);
        ReportProbe(context, "SphericalHarmonics.Orthogonality", worst <= OrthogonalityLimit,
                    detail);
    }

    // 조도: 상수 radiance 1 → E = π, 한 방향 델타 광원 → Σ Â_l Y(l) Y(n)(Ramamoorthi 2001),
    // 창·배율.
    const double lobe[9] = {Pi,       2.0 * Pi / 3.0, 2.0 * Pi / 3.0, 2.0 * Pi / 3.0, Pi / 4.0,
                            Pi / 4.0, Pi / 4.0,       Pi / 4.0,       Pi / 4.0};
    for (uint32 scene = 0; scene < 2; ++scene) {
        ProbeRequest request =
            MakeProbeRequest(ProbeSphericalHarmonicsIrradiance, 0, Width, Height);
        double coefficients[9] = {};
        const Double3 light = Normalize(Double3{0.3, -0.5, 0.8});
        if (scene == 0) {
            coefficients[0] = sqrt(4.0 * Pi);
        } else {
            ShBasis(light, coefficients);
        }

        float stored[9];
        for (uint32 k = 0; k < 9; ++k) {
            stored[k] = static_cast<float>(coefficients[k]);
        }

        for (uint32 k = 0; k < 8; ++k) {
            request.values[k] = stored[k];
        }

        request.values[8] = stored[8];
        constexpr float WindowWidth = 3.0f;
        request.parameters[0] = WindowWidth;
        double approximation = 0.0;
        CheckFloatProbe(
            context,
            scene == 0 ? "SphericalHarmonics.IrradianceConstant"
                       : "SphericalHarmonics.IrradianceDelta",
            request, UniformTolerance(4.0e-6, 0.0),
            [&stored, &lobe, light, scene, &approximation](uint32 c, const float*,
                                                           ProbeExpectation& e) {
                const Double3 n = ProbeDirection(c, 0, 0);
                double y[9];
                ShBasis(n, y);
                const double window[9] = {1.0, 0.75, 0.75, 0.75, 0.25, 0.25, 0.25, 0.25, 0.25};
                double e3 = 0.0;
                double e2 = 0.0;
                double windowed = 0.0;
                double plain = 0.0;
                for (uint32 k = 0; k < 9; ++k) {
                    e3 += lobe[k] * stored[k] * y[k];
                    windowed += window[k] * stored[k] * y[k];
                    plain += stored[k] * y[k];
                    if (k < 4) {
                        e2 += lobe[k] * stored[k] * y[k];
                    }
                }

                e.value[0] = e3;
                e.value[1] = e2;
                e.value[2] = windowed;
                e.value[3] = 0.5 * plain;
                if (scene == 0) {
                    e.value[0] = Pi;
                    e.absolute[0] = 4.0e-6;
                } else {
                    approximation = fmax(approximation, fabs(e3 - fmax(Dot(n, light), 0.0)));
                }
            });
        if (scene == 1) {
            // 3차 SH의 클램프 코사인 근사 오차(측정 기록 — Ramamoorthi 2001 3절의 근사 정도).
            constexpr double ClampedCosineLimit = 0.1;
            char detail[128];
            snprintf(detail, sizeof(detail), "max |E_SH(n) - max(0, n.l)| %.4f (limit %.2f)",
                     approximation, ClampedCosineLimit);
            ReportProbe(context, "SphericalHarmonics.ClampedCosineApproximation",
                        approximation <= ClampedCosineLimit, detail);
        }
    }
}

// --- 광선 교차 double 기준 ---

struct RayCase {
    Double3 origin;
    Double3 direction;
    Double3 center;
};

RayCase MakeRay(uint32 c) noexcept {
    RayCase ray;
    ray.origin = Scale(ProbeVector(c, 0, 0), 400.0);
    ray.direction = ProbeDirection(c, 3, 0);
    ray.center = Scale(ProbeVector(c, 6, 0), 100.0);
    return ray;
}

// 2차식 근이다(double). 근이 없으면 false다.
bool QuadraticRoots(double a, double b, double c, double& r0, double& r1,
                    double& discriminant) noexcept {
    discriminant = b * b - 4.0 * a * c;
    if (fabs(a) < 1.0e-30 || discriminant < 0.0) {
        return false;
    }

    const double s = sqrt(discriminant);
    r0 = (-b - s) / (2.0 * a);
    r1 = (-b + s) / (2.0 * a);
    if (r0 > r1) {
        const double t = r0;
        r0 = r1;
        r1 = t;
    }

    return true;
}

void RunRayProbes(ProbeContext& context) noexcept {
    const char* names[6] = {"RayIntersection.Plane",    "RayIntersection.Sphere",
                            "RayIntersection.Aabb",     "RayIntersection.Cone",
                            "RayIntersection.Cylinder", "RayIntersection.Triangle"};
    for (uint32 selector = 0; selector < 6; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeRayIntersection, selector, Width, Height);
        CheckFloatProbe(
            context, names[selector], request, UniformTolerance(2.0e-3, 2.0e-6),
            [selector](uint32 c, const float*, ProbeExpectation& e) {
                const RayCase ray = MakeRay(c);
                const Double3 o = ray.origin;
                const Double3 d = ray.direction;
                e.mask = 0x1u;
                e.value[0] = 0.0;
                switch (selector) {
                case 0: {
                    const Double3 n = ProbeDirection(c, 9, 0);
                    const double w = ProbeSigned(c, 12, 6, 0);
                    const double denominator = Dot(n, d);
                    if (fabs(denominator) < 1.0e-3) {
                        e.mask = 0;
                        return;
                    }

                    e.value[0] = 1.0;
                    e.value[1] = (w - Dot(n, o)) / denominator;
                    e.mask = 0x3u;
                    e.absolute[1] = 2.0e-3 * (1.0 + fabs(e.value[1]) * 1.0e-3) / fabs(denominator);
                    return;
                }
                case 1: {
                    const double radius = 50.0f + ProbeUnit(c, 9, 0) * 200.0f;
                    const Double3 f = Subtract(o, ray.center);
                    double r0;
                    double r1;
                    double discriminant;
                    const bool hit = QuadraticRoots(
                        1.0, 2.0 * Dot(f, d), Dot(f, f) - radius * radius, r0, r1, discriminant);
                    if (fabs(discriminant) < 1.0e-2 * radius * radius) {
                        e.mask = 0;
                        return;
                    }

                    e.value[0] = hit ? 1.0 : 0.0;
                    if (hit) {
                        e.value[1] = r0;
                        e.value[2] = r1;
                        e.mask = 0x7u;
                    }

                    return;
                }
                case 2: {
                    const double extent[3] = {ProbeUnit(c, 9, 0) * 200.0f + 10.0f,
                                              ProbeUnit(c, 10, 0) * 200.0f + 10.0f,
                                              ProbeUnit(c, 11, 0) * 200.0f + 10.0f};
                    const double oc[3] = {o.x, o.y, o.z};
                    const double dc[3] = {d.x, d.y, d.z};
                    const double cc[3] = {ray.center.x, ray.center.y, ray.center.z};
                    double enter = -1.0e300;
                    double exit = 1.0e300;
                    for (uint32 k = 0; k < 3; ++k) {
                        const double t0 = (cc[k] - extent[k] - oc[k]) / dc[k];
                        const double t1 = (cc[k] + extent[k] - oc[k]) / dc[k];
                        enter = fmax(enter, fmin(t0, t1));
                        exit = fmin(exit, fmax(t0, t1));
                    }

                    if (fabs(enter - exit) < RayBoundaryMargin || fabs(exit) < RayBoundaryMargin) {
                        e.mask = 0;
                        return;
                    }

                    const bool hit = enter <= exit && exit >= 0.0;
                    e.value[0] = hit ? 1.0 : 0.0;
                    e.value[1] = enter;
                    e.value[2] = exit;
                    e.mask = hit ? 0x7u : 0x1u;
                    return;
                }
                case 3:
                case 4: {
                    const Double3 axis = ProbeDirection(c, 9, 0);
                    const Double3 co = Subtract(o, ray.center);
                    const double dv = Dot(d, axis);
                    const double cv = Dot(co, axis);
                    constexpr double Height = 300.0;
                    double best = 1.0e300;
                    double bestTolerance = 0.0;
                    bool ambiguous = false;
                    double r0;
                    double r1;
                    double discriminant;
                    bool solved;
                    // 2차 계수 크기(float 계수 오차의 근 전파 |∂r/∂(A,B,C)| = (r², |r|, 1) / √D).
                    double magnitudeA;
                    double magnitudeB;
                    double magnitudeC;
                    const double coLength = sqrt(Dot(co, co));
                    if (selector == 3) {
                        const double cosine = 0.5f + ProbeUnit(c, 12, 0) * 0.45f;
                        const double c2 = cosine * cosine;
                        solved = QuadraticRoots(dv * dv - c2, 2.0 * (dv * cv - c2 * Dot(d, co)),
                                                cv * cv - c2 * Dot(co, co), r0, r1, discriminant);
                        magnitudeA = dv * dv + c2;
                        magnitudeB = 2.0 * (fabs(dv * cv) + c2 * coLength);
                        magnitudeC = cv * cv + c2 * coLength * coLength;
                    } else {
                        const double radius = 20.0f + ProbeUnit(c, 12, 0) * 150.0f;
                        const Double3 dPerp = Subtract(d, Scale(axis, dv));
                        const Double3 oPerp = Subtract(co, Scale(axis, cv));
                        solved = QuadraticRoots(Dot(dPerp, dPerp), 2.0 * Dot(dPerp, oPerp),
                                                Dot(oPerp, oPerp) - radius * radius, r0, r1,
                                                discriminant);
                        magnitudeA = 1.0;
                        magnitudeB = 2.0 * coLength;
                        magnitudeC = coLength * coLength + radius * radius;
                        for (uint32 cap = 0; cap < 2 && fabs(dv) > 1.0e-9; ++cap) {
                            const double t = ((cap == 0 ? 0.0 : Height) - cv) / dv;
                            const Double3 p = Add(oPerp, Scale(dPerp, t));
                            const double r2 = Dot(p, p);
                            if (fabs(r2 - radius * radius) < RayBoundaryMargin * radius * 4.0 ||
                                fabs(t) < RayBoundaryMargin) {
                                ambiguous = true;
                            }

                            if (t >= 0.0 && r2 <= radius * radius && t < best) {
                                best = t;
                                bestTolerance = RootEpsilon *
                                                    ((cap == 0 ? 0.0 : Height) + fabs(cv)) /
                                                    fabs(dv) +
                                                RootEpsilon * fabs(t);
                            }
                        }
                    }

                    if (solved) {
                        const double roots[2] = {r0, r1};
                        for (const double t : roots) {
                            const double h = cv + t * dv;
                            if (fabs(h) < RayBoundaryMargin ||
                                fabs(h - Height) < RayBoundaryMargin ||
                                fabs(t) < RayBoundaryMargin) {
                                ambiguous = true;
                            }
                        }

                        if (fabs(discriminant) <
                            1.0e-6 * (1.0 + fabs(discriminant) + dv * dv * cv * cv)) {
                            ambiguous = true;
                        }

                        for (const double t : roots) {
                            const double h = cv + t * dv;
                            if (t >= 0.0 && h >= 0.0 && h <= Height) {
                                if (t < best) {
                                    best = t;
                                    bestTolerance =
                                        RootEpsilon *
                                        (t * t * magnitudeA + fabs(t) * magnitudeB + magnitudeC) /
                                        sqrt(fmax(discriminant, 1.0e-30));
                                }

                                break;
                            }
                        }
                    }

                    if (ambiguous) {
                        e.mask = 0;
                        return;
                    }

                    const bool hit = best < 1.0e300;
                    e.value[0] = hit ? 1.0 : 0.0;
                    e.value[1] = best;
                    e.absolute[1] = 2.0e-3 + bestTolerance;
                    e.mask = hit ? 0x3u : 0x1u;
                    return;
                }
                default: {
                    const Double3 v0 = Add(ray.center, Scale(ProbeVector(c, 12, 0), 200.0));
                    const Double3 v1 = Add(ray.center, Scale(ProbeVector(c, 15, 0), 200.0));
                    const Double3 v2 = Add(ray.center, Scale(ProbeVector(c, 18, 0), 200.0));
                    // Möller–Trumbore(double)다.
                    const Double3 e1 = Subtract(v1, v0);
                    const Double3 e2 = Subtract(v2, v0);
                    const Double3 p = Cross(d, e2);
                    const double det = Dot(e1, p);
                    if (fabs(det) < 1.0e-6) {
                        e.mask = 0;
                        return;
                    }

                    const Double3 s = Subtract(o, v0);
                    const double b1 = Dot(s, p) / det;
                    const Double3 q = Cross(s, e1);
                    const double b2 = Dot(d, q) / det;
                    const double t = Dot(e2, q) / det;
                    const double b0 = 1.0 - b1 - b2;
                    if (fabs(b0) < 1.0e-4 || fabs(b1) < 1.0e-4 || fabs(b2) < 1.0e-4 ||
                        fabs(t) < RayBoundaryMargin) {
                        e.mask = 0;
                        return;
                    }

                    const bool hit = b0 > 0.0 && b1 > 0.0 && b2 > 0.0 && t >= 0.0;
                    e.value[0] = hit ? 1.0 : 0.0;
                    e.value[1] = t;
                    e.value[2] = b1;
                    e.value[3] = b2;
                    e.mask = hit ? 0xFu : 0x1u;
                    e.absolute[2] = e.absolute[3] = 2.0e-5 / fmin(1.0, fabs(det) / 1.0e3) + 1.0e-5;
                    return;
                }
                }
            });
    }

    // watertight: 공유 모서리 위 점을 겨눈 광선 2^16개(256 × 256)가 둘 중 하나 이상을 맞는다(누락
    // 0).
    ProbeRequest watertight = MakeProbeRequest(ProbeWatertight, 0, Width, Height);
    Array<uint32> masks(context.allocator);
    if (context.device.RunUint(watertight, masks)) {
        uint32 missed = 0;
        uint32 both = 0;
        for (const uint32 mask : masks) {
            missed += mask == 0 ? 1u : 0u;
            both += mask == 3 ? 1u : 0u;
        }

        char detail[128];
        snprintf(detail, sizeof(detail), "%u edge rays, missed %u, hit both %u",
                 CaseCount(watertight), missed, both);
        ReportProbe(context, "RayIntersection.Watertight", missed == 0, detail);
    }
}

} // namespace

void RunRandomSamplingProbes(ProbeContext& context) noexcept {
    RunRandomProbes(context);
    RunLowDiscrepancyProbes(context);
    RunSamplingProbes(context);
    RunSphericalHarmonicsProbes(context);
    RunRayProbes(context);
}

} // namespace sandbox::engine_shaders
