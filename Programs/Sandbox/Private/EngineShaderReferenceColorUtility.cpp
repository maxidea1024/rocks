#include "EngineShaderComputeProbes.h"

// SL1 A03(LUT 좌표·SDR 출력 부호화·색 지도·구분 색)의 CPU double 기준 구현과 대조다(Docs/Design/
// EngineShaders 14.3·14.6절). 식은 셰이더 원문과 따로 다시 쓴 것이다(결정 SLD10): LUT 반 texel
// 보정·log shaper(LutLogMiddleGrey 0.18, [-10, +6.5] 정지), IEC 61966-2-1 sRGB, Turbo·viridis
// 다항(공개 계수 — 근사 자체의 오차는 색 지도 표의 끝점·중간점과 따로 잰다), PCG 해시와 HSV(Smith
// 1978).
namespace sandbox::engine_shaders {

namespace {

constexpr uint32 LutSize = 32;
constexpr double LutLogGrey = 0.18;
constexpr double LutLogMinStops = -10.0;
constexpr double LutLogRange = 16.5;
// 색 지도 사례 수(t = c · 1.5/1023 − 0.25)다.
constexpr uint32 ColorMapCases = 1024;
// 다항 평가 허용 오차다: 계수 절댓값 합(약 400)에 float 반올림 몇 번 — 측정값보다 넉넉하다.
constexpr double PolynomialAbsolute = 2.0e-4;
// 색 지도 표 값과 근사의 차이 상한이다(근사 자체의 오차 — 14.9절 측정값과 같이 적는다).
constexpr double ViridisTableTolerance = 0.02;
constexpr double TurboTableTolerance = 0.13;
constexpr double DistinctSaturation = 0.75;
constexpr double DistinctValue = 0.9;

double SrgbEncode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= static_cast<double>(0.0031308f) ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

double SrgbDecode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= static_cast<double>(0.04045f) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

double Saturate(double x) noexcept {
    return fmin(fmax(x, 0.0), 1.0);
}

// pow 사양 상한(exp2(y·log2 x) 전파 — 8.3절)이다.
double PowBound(double result, double base, double exponent) noexcept {
    return base <= 0.0 ? 0.0 : fabs(result) * PowRelative(exponent, base) + Arithmetic(3);
}

double LutCenter(double unit) noexcept {
    return unit * (LutSize - 1.0) / LutSize + 0.5 / LutSize;
}

double LutLog(double linear) noexcept {
    const double v = fmax(linear, LutLogGrey * ldexp(1.0, -10)) / LutLogGrey;
    return Saturate((log2(v) - LutLogMinStops) / LutLogRange);
}

void TurboSrgb(double t, double out[3]) noexcept {
    static constexpr double Red[6] = {0.13572138,   4.61539260,    -42.66032258,
                                      132.13108234, -152.94239396, 59.28637943};
    static constexpr double Green[6] = {0.09140261,   2.19418839, 4.84296658,
                                        -14.18503333, 4.27729857, 2.82956604};
    static constexpr double Blue[6] = {0.10667330,   12.64194608,  -60.58204836,
                                       110.36276771, -89.90310912, 27.34824973};
    const double x = Saturate(t);
    double power = 1.0;
    double sums[3] = {};
    for (uint32 k = 0; k < 6; ++k) {
        sums[0] += Red[k] * power;
        sums[1] += Green[k] * power;
        sums[2] += Blue[k] * power;
        power *= x;
    }

    for (uint32 channel = 0; channel < 3; ++channel) {
        out[channel] = Saturate(sums[channel]);
    }
}

void ViridisSrgb(double t, double out[3]) noexcept {
    static constexpr double C[7][3] = {
        {0.2777273272234177, 0.005407344544966578, 0.3340998053353061},
        {0.1050930431085774, 1.404613529898575, 1.384590162594685},
        {-0.3308618287255563, 0.214847559468213, 0.09509516302823659},
        {-4.634230498983486, -5.799100973351585, -19.33244095627987},
        {6.228269936347081, 14.17993336680509, 56.69055260068105},
        {4.776384997670288, -13.74514537774601, -65.35303263337234},
        {-5.435455855934631, 4.645852612178535, 26.3124352495832}};
    const double x = Saturate(t);
    for (uint32 channel = 0; channel < 3; ++channel) {
        double value = C[6][channel];
        for (int k = 5; k >= 0; --k) {
            value = C[k][channel] + x * value;
        }

        out[channel] = Saturate(value);
    }
}

uint32 PcgHash(uint32 value) noexcept {
    const uint32 state = value * 747796405u + 2891336453u;
    const uint32 word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// HSV → RGB(셰이더 HsvToRgb와 같은 정의 — 위상 k = frac(h + n/6)·6, n = 0·4·2)다.
void HsvToRgb(double h, double s, double v, double out[3]) noexcept {
    static constexpr double Offsets[3] = {0.0, 4.0, 2.0};
    for (uint32 channel = 0; channel < 3; ++channel) {
        const double phase = h + Offsets[channel] / 6.0;
        const double k = (phase - floor(phase)) * 6.0;
        const double ramp = Saturate(fabs(k - 3.0) - 1.0);
        out[channel] = v * (1.0 + (ramp - 1.0) * s);
    }
}

void RunLutProbes(ProbeContext& context) noexcept {
    // 단위 좌표 → 반 texel 보정(산술 셋).
    CheckFloatProbe(
        context, "LutCoordinates.TexelCenter", MakeProbeRequest(ProbeLutCoordinates, 0, 64, 64),
        UniformTolerance(Arithmetic(4), 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
            for (uint32 channel = 0; channel < 3; ++channel) {
                e.value[channel] = LutCenter(ProbeUnit(c, channel, 0));
            }

            e.value[3] = LutSize;
        });

    // 선형 색 → LUT 좌표(log2 사양 상한 전파 + 반 texel 산술).
    CheckFloatProbe(context, "LutCoordinates.LinearColor",
                    MakeProbeRequest(ProbeLutCoordinates, 1, 64, 64),
                    UniformTolerance(4.0 * SpecLog2Absolute / LutLogRange + Arithmetic(8), 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        for (uint32 channel = 0; channel < 3; ++channel) {
                            const uint32 scale = ProbeBits(c, 3 + channel, 0) % 20u;
                            const double scene = static_cast<double>(ProbeUnit(c, channel, 0)) *
                                                 ldexp(1.0, static_cast<int>(scale) - 12);
                            e.value[channel] = LutCenter(LutLog(scene));
                        }

                        e.mask = 0x7u;
                    });

    // 32³ texel 전부: 선형 색(exp2 사양 상한)과 중립 좌표(나눗셈 하나).
    CheckFloatProbe(
        context, "LutCoordinates.TexelColors", MakeProbeRequest(ProbeLutCoordinates, 2, 256, 128),
        UniformTolerance(0.0, 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
            const uint32 texel[3] = {c % LutSize, (c / LutSize) % LutSize, c / (LutSize * LutSize)};
            for (uint32 channel = 0; channel < 3; ++channel) {
                const double neutral = texel[channel] / (LutSize - 1.0);
                e.value[channel] = LutLogGrey * exp2(LutLogMinStops + LutLogRange * neutral);
                // exp2 사양 상한(|y| <= 10)과 지수 인자 산술 오차의 전파(ln 2 · 16.5배)다.
                e.absolute[channel] = e.value[channel] * 2.0 *
                                      ((3.0 + 2.0 * 10.0) * FloatUlp +
                                       0.6931471805599453 * LutLogRange * Arithmetic(4));
            }

            e.value[3] = texel[0] / (LutSize - 1.0);
            e.absolute[3] = Arithmetic(2);
        });

    // 2D 펼친 LUT 픽셀 → texel(정수, 정확).
    CheckFloatProbe(context, "LutCoordinates.Unwrapped2D",
                    MakeProbeRequest(ProbeLutCoordinates, 3, LutSize * LutSize, LutSize),
                    UniformTolerance(0.0, 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
                        const uint32 x = c % (LutSize * LutSize);
                        const uint32 y = c / (LutSize * LutSize);
                        e.value[0] = x % LutSize;
                        e.value[1] = y;
                        e.value[2] = x / LutSize;
                        e.mask = 0x7u;
                    });
}

void RunDisplayProbes(ProbeContext& context) noexcept {
    // 8 bit 부호 256개: 부호 그대로 돌아오는지와 SDR 밖 자르기.
    CheckFloatProbe(
        context, "DisplayEncoding.SdrSrgbCodes", MakeProbeRequest(ProbeDisplayEncoding, 0, 256, 1),
        UniformTolerance(0.0, 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
            const double x = SrgbDecode(c / 255.0);
            const double inputs[4] = {x, x * 1.5, x - 0.5, x};
            for (uint32 channel = 0; channel < 4; ++channel) {
                const double clamped = Saturate(inputs[channel]);
                e.value[channel] = SrgbEncode(clamped);
                // 입력 x 자체가 float 복호화 결과다(복호화 오차를 부호화가 다시 키운다).
                e.absolute[channel] = PowBound(e.value[channel], clamped, 1.0 / 2.4) + 2.0e-6;
            }
        });

    CheckFloatProbe(
        context, "DisplayEncoding.SdrSrgbRandom", MakeProbeRequest(ProbeDisplayEncoding, 1, 64, 64),
        UniformTolerance(0.0, 0.0), [](uint32 c, const float*, ProbeExpectation& e) {
            for (uint32 channel = 0; channel < 3; ++channel) {
                const double input = static_cast<double>(ProbeUnit(c, channel, 0)) * 2.0 - 0.25;
                const double clamped = Saturate(input);
                e.value[channel] = SrgbEncode(clamped);
                e.absolute[channel] =
                    PowBound(e.value[channel], clamped, 1.0 / 2.4) + Arithmetic(4);
            }

            e.mask = 0x7u;
        });

    // 8 bit 부호 왕복: 부호화 결과 × 255 반올림이 처음 부호인지.
    ProbeRequest request = MakeProbeRequest(ProbeDisplayEncoding, 0, 256, 1);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "DisplayEncoding.SdrSrgbCodeValues", request, gpu)) {
        uint32 errors = 0;
        for (uint32 c = 0; c < 256; ++c) {
            errors += rint(gpu[c * 4u + 3u] * 255.0) != c ? 1u : 0u;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "256 codes, round-trip code errors %u", errors);
        ReportProbe(context, "DisplayEncoding.SdrSrgb8RoundTrip", errors == 0, detail);
    }
}

void RunColorMapProbes(ProbeContext& context) noexcept {
    static const char* const names[4] = {"ColorMaps.TurboSrgb", "ColorMaps.ViridisSrgb",
                                         "ColorMaps.TurboLinear", "ColorMaps.ViridisLinear"};
    for (uint32 selector = 0; selector < 4; ++selector) {
        CheckFloatProbe(
            context, names[selector], MakeProbeRequest(ProbeColorMaps, selector, ColorMapCases, 1),
            UniformTolerance(0.0, 0.0), [selector](uint32 c, const float*, ProbeExpectation& e) {
                const double t = c * (1.5 / 1023.0) - 0.25;
                double srgb[3] = {};
                if (selector % 2 == 0) {
                    TurboSrgb(t, srgb);
                } else {
                    ViridisSrgb(t, srgb);
                }

                for (uint32 channel = 0; channel < 3; ++channel) {
                    const bool linear = selector >= 2;
                    e.value[channel] = linear ? SrgbDecode(srgb[channel]) : srgb[channel];
                    // 선형 판은 sRGB 복호화 기울기(최대 2.4 배)가 다항 오차를 키운다.
                    e.absolute[channel] =
                        linear
                            ? PolynomialAbsolute * 2.4 +
                                  PowBound(e.value[channel], (srgb[channel] + 0.055) / 1.055, 2.4)
                            : PolynomialAbsolute;
                }

                e.value[3] = t;
                e.absolute[3] = Arithmetic(2);
            });
    }

    // 근사 자체의 오차: 공개 색 지도 표의 끝점·중간점(sRGB 부호 값)과의 차이.
    struct Anchor {
        bool turbo;
        double t;
        double srgb[3];
        double tolerance;
    };

    static constexpr Anchor Anchors[] = {
        {false, 0.0, {0.267004, 0.004874, 0.329415}, ViridisTableTolerance},
        {false, 128.0 / 255.0, {0.127568, 0.566949, 0.550556}, ViridisTableTolerance},
        {false, 1.0, {0.993248, 0.906157, 0.143936}, ViridisTableTolerance},
        {true, 0.0, {0.18995, 0.07176, 0.23217}, TurboTableTolerance},
        {true, 1.0, {0.4796, 0.01583, 0.01055}, TurboTableTolerance}};
    double worstViridis = 0.0;
    double worstTurbo = 0.0;
    bool anchorsPassed = true;
    for (const Anchor& anchor : Anchors) {
        double approximation[3] = {};
        if (anchor.turbo) {
            TurboSrgb(anchor.t, approximation);
        } else {
            ViridisSrgb(anchor.t, approximation);
        }

        for (uint32 channel = 0; channel < 3; ++channel) {
            const double error = fabs(approximation[channel] - anchor.srgb[channel]);
            double& worst = anchor.turbo ? worstTurbo : worstViridis;
            worst = fmax(worst, error);
            anchorsPassed = anchorsPassed && error <= anchor.tolerance;
        }
    }

    char detail[160];
    snprintf(detail, sizeof(detail),
             "polynomial vs published table: viridis max %.4f (<= %.2f), turbo max %.4f (<= %.2f)",
             worstViridis, ViridisTableTolerance, worstTurbo, TurboTableTolerance);
    ReportProbe(context, "ColorMaps.TableAnchors", anchorsPassed, detail);

    CheckFloatProbe(context, "ColorMaps.DistinctIndexColor",
                    MakeProbeRequest(ProbeDistinctColor, 0, 64, 64), UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        const double hue = static_cast<double>(PcgHash(c) >> 8) * ldexp(1.0, -24);
                        double srgb[3] = {};
                        HsvToRgb(hue, DistinctSaturation, DistinctValue, srgb);
                        for (uint32 channel = 0; channel < 3; ++channel) {
                            e.value[channel] = SrgbDecode(srgb[channel]);
                            // 위상 frac·곱 몇 번의 float 오차가 복호화 기울기로 커진다.
                            e.absolute[channel] =
                                Arithmetic(8) * 2.4 +
                                PowBound(e.value[channel], (srgb[channel] + 0.055) / 1.055, 2.4);
                        }

                        e.mask = 0x7u;
                    });
}

} // namespace

void RunColorUtilityProbes(ProbeContext& context) noexcept {
    RunLutProbes(context);
    RunDisplayProbes(context);
    RunColorMapProbes(context);
}

} // namespace sandbox::engine_shaders
