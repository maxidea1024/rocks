#include "EngineShaderProbe.h"

// A03 색의 CPU double 기준 구현과 대조다(Docs/Design/EngineShaders 7.4절). 원색 행렬은 SMPTE RP 177
// 절차로 이 파일이 원색·백색 xy에서 직접 계산한다(셰이더 상수와 따로 — 결정 SLD10). 전달 함수는 IEC
// 61966-2-1· BT.709·BT.1886·ST 2084·BT.2100 식, 색온도는 Kang 2002·CIE 15:2004 식, filmic 곡선은
// FilmicToneCurve.rsh와 같은 식(UE Filmic 경로의 의미)을 double로 다시 쓴 것이다.
namespace sandbox::engine_shaders {

namespace {

constexpr double Pi = 3.14159265358979323846;
constexpr uint32 Width = 64;
constexpr uint32 Height = 64;

struct Matrix3 {
    double m[3][3] = {};
};

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) noexcept {
    Matrix3 r;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                r.m[i][j] += a.m[i][k] * b.m[k][j];
            }
        }
    }

    return r;
}

Matrix3 Inverse(const Matrix3& a) noexcept {
    const double (&m)[3][3] = a.m;
    const double c00 = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    const double c01 = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    const double c02 = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    const double det = m[0][0] * c00 + m[0][1] * c01 + m[0][2] * c02;
    Matrix3 r;
    r.m[0][0] = c00 / det;
    r.m[1][0] = c01 / det;
    r.m[2][0] = c02 / det;
    r.m[0][1] = (m[0][2] * m[2][1] - m[0][1] * m[2][2]) / det;
    r.m[1][1] = (m[0][0] * m[2][2] - m[0][2] * m[2][0]) / det;
    r.m[2][1] = (m[0][1] * m[2][0] - m[0][0] * m[2][1]) / det;
    r.m[0][2] = (m[0][1] * m[1][2] - m[0][2] * m[1][1]) / det;
    r.m[1][2] = (m[0][2] * m[1][0] - m[0][0] * m[1][2]) / det;
    r.m[2][2] = (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
    return r;
}

void Apply(const Matrix3& a, const double in[3], double out[3]) noexcept {
    for (int i = 0; i < 3; ++i) {
        out[i] = a.m[i][0] * in[0] + a.m[i][1] * in[1] + a.m[i][2] * in[2];
    }
}

void XyToXyz(double x, double y, double out[3]) noexcept {
    out[0] = x / y;
    out[1] = 1.0;
    out[2] = (1.0 - x - y) / y;
}

// SMPTE RP 177: 원색 xy 열에 백색이 (1,1,1)이 되는 배율을 곱한다.
Matrix3 RgbToXyz(const double primaries[3][2], const double white[2]) noexcept {
    Matrix3 p;
    for (int column = 0; column < 3; ++column) {
        double xyz[3];
        XyToXyz(primaries[column][0], primaries[column][1], xyz);
        for (int row = 0; row < 3; ++row) {
            p.m[row][column] = xyz[row];
        }
    }

    double w[3];
    XyToXyz(white[0], white[1], w);
    double s[3];
    Apply(Inverse(p), w, s);
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            p.m[row][column] *= s[column];
        }
    }

    return p;
}

constexpr double D65[2] = {0.3127, 0.3290};
constexpr double AcesWhite[2] = {0.32168, 0.33767};
constexpr double Rec709Primaries[3][2] = {{0.64, 0.33}, {0.30, 0.60}, {0.15, 0.06}};
constexpr double Rec2020Primaries[3][2] = {{0.708, 0.292}, {0.170, 0.797}, {0.131, 0.046}};
constexpr double P3Primaries[3][2] = {{0.680, 0.320}, {0.265, 0.690}, {0.150, 0.060}};
constexpr double AP0Primaries[3][2] = {{0.7347, 0.2653}, {0.0, 1.0}, {0.0001, -0.0770}};
constexpr double AP1Primaries[3][2] = {{0.713, 0.293}, {0.165, 0.830}, {0.128, 0.044}};
constexpr Matrix3 Bradford = {
    {{0.8951, 0.2664, -0.1614}, {-0.7502, 1.7135, 0.0367}, {0.0389, -0.0685, 1.0296}}};
constexpr Matrix3 Cat02 = {
    {{0.7328, 0.4296, -0.1624}, {-0.7036, 1.6975, 0.0061}, {0.0030, 0.0136, 0.9834}}};

Matrix3 Adaptation(const Matrix3& cone, const double source[2],
                   const double destination[2]) noexcept {
    double sw[3];
    double dw[3];
    XyToXyz(source[0], source[1], sw);
    XyToXyz(destination[0], destination[1], dw);
    double s[3];
    double d[3];
    Apply(cone, sw, s);
    Apply(cone, dw, d);
    Matrix3 scale;
    for (int k = 0; k < 3; ++k) {
        scale.m[k][k] = d[k] / s[k];
    }

    return Multiply(Inverse(cone), Multiply(scale, cone));
}

// 행렬 번호(ProbeMatrix*) 순서의 double 행렬이다.
Matrix3 ColorMatrix(uint32 index) noexcept {
    const Matrix3 rec709 = RgbToXyz(Rec709Primaries, D65);
    const Matrix3 rec2020 = RgbToXyz(Rec2020Primaries, D65);
    const Matrix3 p3 = RgbToXyz(P3Primaries, D65);
    const Matrix3 ap0 = RgbToXyz(AP0Primaries, AcesWhite);
    const Matrix3 ap1 = RgbToXyz(AP1Primaries, AcesWhite);
    switch (index) {
    case 0:
        return rec709;
    case 1:
        return Inverse(rec709);
    case 2:
        return rec2020;
    case 3:
        return Inverse(rec2020);
    case 4:
        return p3;
    case 5:
        return Inverse(p3);
    case 6:
        return ap0;
    case 7:
        return Inverse(ap0);
    case 8:
        return ap1;
    case 9:
        return Inverse(ap1);
    case 10:
        return Multiply(Inverse(ap1), ap0);
    case 11:
        return Multiply(Inverse(ap0), ap1);
    case 12:
        return Multiply(Inverse(rec2020), rec709);
    case 13:
        return Multiply(Inverse(rec709), rec2020);
    case 14:
        return Adaptation(Bradford, D65, AcesWhite);
    default:
        return Adaptation(Bradford, AcesWhite, D65);
    }
}

double Wrap6(double hue) noexcept {
    return hue < 0.0 ? hue + 6.0 : hue;
}

// pow 사양 상한(exp2(y·log2 x) 전파 — 8.3절)이다.
double PowSpec(double result, double base, double exponent) noexcept {
    if (base <= 0.0) {
        return 0.0;
    }

    const double t = fabs(exponent * log2(base));
    return fabs(result) *
           ((3.0 + 2.0 * t) * FloatUlp +
            fabs(exponent) * 0.6931471805599453 * SpecLog2Absolute * 4.0 + Arithmetic(3));
}

double SrgbEncode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= static_cast<double>(0.0031308f) ? v * 12.92 : 1.055 * pow(v, 1.0 / 2.4) - 0.055;
}

double SrgbDecode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= static_cast<double>(0.04045f) ? v / 12.92 : pow((v + 0.055) / 1.055, 2.4);
}

constexpr double PqM1 = 2610.0 / 16384.0;
constexpr double PqM2 = 2523.0 / 4096.0 * 128.0;
constexpr double PqC1 = 3424.0 / 4096.0;
constexpr double PqC2 = 2413.0 / 4096.0 * 32.0;
constexpr double PqC3 = 2392.0 / 4096.0 * 32.0;

double PqEncode(double nits) noexcept {
    const double y = pow(fmin(fmax(nits / 10000.0, 0.0), 1.0), PqM1);
    return pow((PqC1 + PqC2 * y) / (1.0 + PqC3 * y), PqM2);
}

double PqDecode(double signal) noexcept {
    const double e = pow(fmin(fmax(signal, 0.0), 1.0), 1.0 / PqM2);
    return 10000.0 * pow(fmax(e - PqC1, 0.0) / (PqC2 - PqC3 * e), 1.0 / PqM1);
}

constexpr double HlgA = 0.17883277;
constexpr double HlgB = 1.0 - 4.0 * HlgA;
const double HlgC = 0.5 - HlgA * log(4.0 * HlgA);

double HlgEncode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= static_cast<double>(1.0f / 12.0f) ? sqrt(3.0 * v)
                                                  : HlgA * log(12.0 * v - HlgB) + HlgC;
}

double HlgDecode(double x) noexcept {
    const double v = fmax(x, 0.0);
    return v <= 0.5 ? v * v / 3.0 : (exp((v - HlgC) / HlgA) + HlgB) / 12.0;
}

// Kang 2002 Planck 궤적·CIE 주광 궤적(1667–25000 K)이다.
void PlanckXy(double kelvin, double& x, double& y) noexcept {
    const double t = fmin(fmax(kelvin, 1667.0), 25000.0);
    const double i1 = 1.0 / t;
    const double i2 = i1 * i1;
    const double i3 = i2 * i1;
    x = t <= 4000.0 ? -0.2661239e9 * i3 - 0.2343589e6 * i2 + 0.8776956e3 * i1 + 0.179910
                    : -3.0258469e9 * i3 + 2.1070379e6 * i2 + 0.2226347e3 * i1 + 0.240390;
    if (t <= 2222.0) {
        y = -1.1063814 * x * x * x - 1.34811020 * x * x + 2.18555832 * x - 0.20219683;
    } else if (t <= 4000.0) {
        y = -0.9549476 * x * x * x - 1.37418593 * x * x + 2.09137015 * x - 0.16748867;
    } else {
        y = 3.0817580 * x * x * x - 5.87338670 * x * x + 3.75112997 * x - 0.37001483;
    }
}

void DaylightXy(double kelvin, double& x, double& y) noexcept {
    const double t = fmin(fmax(kelvin, 4000.0), 25000.0);
    const double i1 = 1.0 / t;
    const double i2 = i1 * i1;
    const double i3 = i2 * i1;
    x = t <= 7000.0 ? -4.6070e9 * i3 + 2.9678e6 * i2 + 0.09911e3 * i1 + 0.244063
                    : -2.0064e9 * i3 + 1.9018e6 * i2 + 0.24748e3 * i1 + 0.237040;
    y = -3.0 * x * x + 2.870 * x - 0.275;
}

void XyToUv(double x, double y, double& u, double& v) noexcept {
    const double d = -2.0 * x + 12.0 * y + 3.0;
    u = 4.0 * x / d;
    v = 6.0 * y / d;
}

void UvToXy(double u, double v, double& x, double& y) noexcept {
    const double d = 2.0 * u - 8.0 * v + 4.0;
    x = 3.0 * u / d;
    y = 2.0 * v / d;
}

// 색온도·tint 백색(ChromaticAdaptation.rsh WhiteBalanceWhiteXy와 같은 정의 — uv에서 Planck 궤적
// 수직, tint 1 = 0.05)이다.
void WhiteBalanceWhite(double kelvin, double tint, double& x, double& y) noexcept {
    double wx;
    double wy;
    if (kelvin >= 4000.0) {
        DaylightXy(kelvin, wx, wy);
    } else {
        PlanckXy(kelvin, wx, wy);
    }

    double px;
    double py;
    double u0;
    double v0;
    double u1;
    double v1;
    PlanckXy(kelvin - 10.0, px, py);
    XyToUv(px, py, u0, v0);
    PlanckXy(kelvin + 10.0, px, py);
    XyToUv(px, py, u1, v1);
    double au = u1 - u0;
    double av = v1 - v0;
    const double length = sqrt(au * au + av * av);
    au /= length;
    av /= length;
    const double sign = au < 0.0 ? -1.0 : 1.0;
    double u;
    double v;
    XyToUv(wx, wy, u, v);
    UvToXy(u - av * sign * tint * 0.05, v + au * sign * tint * 0.05, x, y);
}

void RunMatrixProbes(ProbeContext& context) noexcept {
    // 행렬 상수(GPU가 읽은 float 행) = RP 177 double(반올림 2^-24), 왕복 = I, 백색·원색 색도.
    ProbeRequest rows = MakeProbeRequest(ProbeColorMatrixRows, 0, ProbeMatrixCount * 3, 1);
    Array<float> gpu(context.allocator);
    if (RunFloatProbe(context, "ColorSpaces.MatrixRows", rows, gpu)) {
        double worstConstant = 0.0;
        double worstRoundTrip = 0.0;
        Matrix3 read[ProbeMatrixCount];
        for (uint32 index = 0; index < ProbeMatrixCount; ++index) {
            const Matrix3 reference = ColorMatrix(index);
            for (int row = 0; row < 3; ++row) {
                for (int column = 0; column < 3; ++column) {
                    read[index].m[row][column] = gpu[(index * 3 + row) * 4u + column];
                    worstConstant = fmax(
                        worstConstant, fabs(read[index].m[row][column] - reference.m[row][column]) /
                                           (fabs(reference.m[row][column]) + 1.0e-3));
                }
            }
        }

        for (uint32 index = 0; index + 1 < 10; index += 2) {
            const Matrix3 product = Multiply(read[index], read[index + 1]);
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    worstRoundTrip =
                        fmax(worstRoundTrip, fabs(product.m[i][j] - (i == j ? 1.0 : 0.0)));
                }
            }
        }

        // 백색 (1,1,1)의 xy = D65, 원색 열의 xy = BT.709 원색(Rec709ToXyz).
        double worstChromaticity = 0.0;
        const double ones[3] = {1.0, 1.0, 1.0};
        double white[3];
        Apply(read[0], ones, white);
        const double sum = white[0] + white[1] + white[2];
        worstChromaticity = fmax(fabs(white[0] / sum - D65[0]), fabs(white[1] / sum - D65[1]));
        for (int column = 0; column < 3; ++column) {
            const double s = read[0].m[0][column] + read[0].m[1][column] + read[0].m[2][column];
            worstChromaticity = fmax(worstChromaticity,
                                     fabs(read[0].m[0][column] / s - Rec709Primaries[column][0]));
            worstChromaticity = fmax(worstChromaticity,
                                     fabs(read[0].m[1][column] / s - Rec709Primaries[column][1]));
        }

        constexpr double ConstantLimit = 1.0e-7;
        constexpr double RoundTripLimit = 1.0e-6;
        char detail[192];
        snprintf(detail, sizeof(detail),
                 "%u matrices, max relative constant error %.3g (limit %.0e), max |M M^-1 - I| "
                 "%.3g (limit %.0e), "
                 "white and primary xy error %.3g",
                 ProbeMatrixCount, worstConstant, ConstantLimit, worstRoundTrip, RoundTripLimit,
                 worstChromaticity);
        ReportProbe(context, "ColorSpaces.MatrixConstants",
                    worstConstant <= ConstantLimit && worstRoundTrip <= RoundTripLimit &&
                        worstChromaticity <= 1.0e-6,
                    detail);
    }

    char label[96];
    for (uint32 index = 0; index < ProbeMatrixCount; ++index) {
        ProbeRequest request = MakeProbeRequest(ProbeColorMatrix, index, Width, Height);
        snprintf(label, sizeof(label), "ColorSpaces.Matrix%u", index);
        const Matrix3 reference = ColorMatrix(index);
        CheckFloatProbe(context, label, request, UniformTolerance(0.0, 0.0),
                        [&reference](uint32 c, const float*, ProbeExpectation& e) {
                            double in[3];
                            for (uint32 k = 0; k < 3; ++k) {
                                const float scaled = ProbeUnit(c, k, 0) * 2.0f;
                                in[k] = scaled - 1.0f;
                            }

                            double out[3];
                            Apply(reference, in, out);
                            for (int k = 0; k < 3; ++k) {
                                const double magnitude = fabs(reference.m[k][0] * in[0]) +
                                                         fabs(reference.m[k][1] * in[1]) +
                                                         fabs(reference.m[k][2] * in[2]);
                                e.value[k] = out[k];
                                e.absolute[k] = Arithmetic(8) * magnitude + 1.0e-7 * magnitude;
                            }

                            e.mask = 0x7u;
                        });
    }
}

void RunFunctionProbes(ProbeContext& context) noexcept {
    const Matrix3 rec709 = RgbToXyz(Rec709Primaries, D65);
    const char* names[8] = {"ColorSpaces.LuminanceXyY",     "ColorSpaces.XyYToXyz",
                            "ColorSpaces.RgbToYCoCg",       "ColorSpaces.YCoCgRoundTrip",
                            "ColorSpaces.RgbToHsv",         "ColorSpaces.HsvToRgb",
                            "ColorSpaces.LinearRgbToOklab", "ColorSpaces.OklabRoundTrip"};
    for (uint32 selector = 0; selector < 8; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeColorFunctions, selector, Width, Height);
        const double tolerance = selector >= 6 ? 5.0e-5 : 2.0e-6;
        CheckFloatProbe(
            context, names[selector], request, UniformTolerance(tolerance, 0.0),
            [selector, &rec709](uint32 c, const float* gpu, ProbeExpectation& e) {
                const double r = ProbeUnit(c, 0, 0);
                const double g = ProbeUnit(c, 1, 0);
                const double b = ProbeUnit(c, 2, 0);
                e.mask = 0x7u;
                switch (selector) {
                case 0: {
                    const double sum = r + g + b;
                    e.value[0] = rec709.m[1][0] * r + rec709.m[1][1] * g + rec709.m[1][2] * b;
                    e.value[1] = r / sum;
                    e.value[2] = g / sum;
                    e.value[3] = g;
                    e.mask = 0xFu;
                    break;
                }
                case 1: {
                    const float x = ProbeUnit(c, 0, 0) * 0.5f + 0.1f;
                    const float y = ProbeUnit(c, 1, 0) * 0.5f + 0.1f;
                    const double scale = b / y;
                    e.value[0] = x * scale;
                    e.value[1] = b;
                    e.value[2] = (1.0 - x - y) * scale;
                    e.absolute[0] = e.absolute[2] = 2.0e-6 * (1.0 + scale);
                    break;
                }
                case 2:
                    e.value[0] = 0.25 * r + 0.5 * g + 0.25 * b;
                    e.value[1] = 0.5 * r - 0.5 * b;
                    e.value[2] = -0.25 * r + 0.5 * g - 0.25 * b;
                    break;
                case 3:
                    e.value[0] = r;
                    e.value[1] = g;
                    e.value[2] = b;
                    break;
                case 4: {
                    const double maximum = fmax(r, fmax(g, b));
                    const double chroma = maximum - fmin(r, fmin(g, b));
                    double hue = 0.0;
                    if (chroma > 0.0) {
                        if (maximum == r) {
                            hue = Wrap6((g - b) / chroma);
                        } else if (maximum == g) {
                            hue = (b - r) / chroma + 2.0;
                        } else {
                            hue = (r - g) / chroma + 4.0;
                        }
                    }

                    hue /= 6.0;
                    // 색상각은 원이다(0과 1이 같다): GPU 값 쪽 바퀴로 맞춘다.
                    if (gpu[0] - hue > 0.5) {
                        hue += 1.0;
                    } else if (hue - gpu[0] > 0.5) {
                        hue -= 1.0;
                    }

                    e.value[0] = hue;
                    e.value[1] = maximum > 0.0 ? chroma / maximum : 0.0;
                    e.value[2] = maximum;
                    e.absolute[0] = chroma < 1.0e-3 ? 1.0 : 1.0e-5 / chroma;
                    break;
                }
                case 5: {
                    for (uint32 k = 0; k < 3; ++k) {
                        const double offset = (k == 0 ? 0.0 : (k == 1 ? 4.0 : 2.0)) / 6.0;
                        double f = r + offset;
                        f = (f - floor(f)) * 6.0;
                        const double ramp = fmin(fmax(fabs(f - 3.0) - 1.0, 0.0), 1.0);
                        e.value[k] = b * (1.0 + (ramp - 1.0) * g);
                    }

                    break;
                }
                default: {
                    const double in[3] = {2.0 * r, 2.0 * g, 2.0 * b};
                    const double l =
                        cbrt(0.4122214708 * in[0] + 0.5363325363 * in[1] + 0.0514459929 * in[2]);
                    const double m =
                        cbrt(0.2119034982 * in[0] + 0.6806995451 * in[1] + 0.1073969566 * in[2]);
                    const double s =
                        cbrt(0.0883024619 * in[0] + 0.2817188376 * in[1] + 0.6299787005 * in[2]);
                    if (selector == 6) {
                        e.value[0] = 0.2104542553 * l + 0.7936177850 * m - 0.0040720468 * s;
                        e.value[1] = 1.9779984951 * l - 2.4285922050 * m + 0.4505937099 * s;
                        e.value[2] = 0.0259040371 * l + 0.7827717662 * m - 0.8086757660 * s;
                    } else {
                        e.value[0] = in[0];
                        e.value[1] = in[1];
                        e.value[2] = in[2];
                    }

                    break;
                }
                }
            });
    }
}

void RunAdaptationProbes(ProbeContext& context) noexcept {
    // Bradford·CAT02 von Kries 순응(같은 백색이면 I).
    const double whites[4][2][2] = {{{0.3127, 0.3290}, {0.32168, 0.33767}},
                                    {{0.3127, 0.3290}, {0.3127, 0.3290}},
                                    {{0.44757, 0.40745}, {0.3127, 0.3290}},
                                    {{0.3457, 0.3585}, {0.3127, 0.3290}}};
    char label[96];
    for (uint32 pair = 0; pair < 4; ++pair) {
        ProbeRequest request = MakeProbeRequest(ProbeAdaptation, 0, 7, 1);
        for (uint32 k = 0; k < 2; ++k) {
            request.parameters[k] = static_cast<float>(whites[pair][0][k]);
            request.parameters[2 + k] = static_cast<float>(whites[pair][1][k]);
        }

        const double source[2] = {request.parameters[0], request.parameters[1]};
        const double destination[2] = {request.parameters[2], request.parameters[3]};
        const Matrix3 bradford = Adaptation(Bradford, source, destination);
        const Matrix3 cat02 = Adaptation(Cat02, source, destination);
        snprintf(label, sizeof(label), "ChromaticAdaptation.BradfordCat02[%u]", pair);
        CheckFloatProbe(context, label, request, UniformTolerance(4.0e-6, 0.0),
                        [&](uint32 c, const float*, ProbeExpectation& e) {
                            e.mask = 0x7u;
                            if (c == 6) {
                                XyToXyz(source[0], source[1], e.value);
                                return;
                            }

                            const Matrix3& m = c < 3 ? bradford : cat02;
                            for (int k = 0; k < 3; ++k) {
                                e.value[k] = m.m[c % 3][k];
                            }
                        });
    }

    // white balance 행렬(색온도·tint → D65, Bradford)과 uv 왕복.
    const double settings[3][2] = {{6504.0, 0.0}, {3200.0, 0.2}, {9000.0, -0.3}};
    const Matrix3 rec709 = RgbToXyz(Rec709Primaries, D65);
    for (uint32 index = 0; index < 3; ++index) {
        ProbeRequest request = MakeProbeRequest(ProbeWhiteBalance, 0, 4, 1);
        request.parameters[0] = static_cast<float>(settings[index][0]);
        request.parameters[1] = static_cast<float>(settings[index][1]);
        double wx;
        double wy;
        WhiteBalanceWhite(request.parameters[0], request.parameters[1], wx, wy);
        const double white[2] = {wx, wy};
        const Matrix3 matrix =
            Multiply(Inverse(rec709), Multiply(Adaptation(Bradford, white, D65), rec709));
        snprintf(label, sizeof(label), "ChromaticAdaptation.WhiteBalance[%u]", index);
        // 궤적 다항식(1/T³)·순응 비율·역행렬을 float로 거친다 — 상대 2e-4.
        CheckFloatProbe(context, label, request, UniformTolerance(2.0e-5, 2.0e-4),
                        [&](uint32 c, const float*, ProbeExpectation& e) {
                            if (c < 3) {
                                for (int k = 0; k < 3; ++k) {
                                    e.value[k] = matrix.m[c][k];
                                }

                                e.mask = 0x7u;
                                return;
                            }

                            const float u = ProbeUnit(c, 0, 0) * 0.4f + 0.1f;
                            const float v = ProbeUnit(c, 1, 0) * 0.4f + 0.1f;
                            e.value[0] = wx;
                            e.value[1] = wy;
                            e.value[2] = u;
                            e.value[3] = v;
                            e.absolute[2] = e.absolute[3] = 2.0e-6;
                        });
    }

    // 색온도 궤적(1667–25000 K)과 CIE 기준 백색(D65 6504 K·D50 5003 K 주광, A 2856 K Planck —
    // 0.0005).
    ProbeRequest locus = MakeProbeRequest(ProbeColorTemperature, 0, 1024, 1);
    locus.parameters[0] = 1667.0f;
    locus.parameters[1] = static_cast<float>((25000.0 - 1667.0) / 1023.0);
    CheckFloatProbe(context, "ChromaticAdaptation.ColorTemperatureLocus", locus,
                    UniformTolerance(3.0e-6, 0.0),
                    [&locus](uint32 c, const float*, ProbeExpectation& e) {
                        const float step = static_cast<float>(c) * locus.parameters[1];
                        const double kelvin = locus.parameters[0] + step;
                        PlanckXy(kelvin, e.value[0], e.value[1]);
                        if (kelvin >= 4000.0) {
                            DaylightXy(kelvin, e.value[2], e.value[3]);
                        } else {
                            e.value[2] = e.value[0];
                            e.value[3] = e.value[1];
                        }

                        // 4000 K 경계는 float 반올림으로 갈릴 수 있다.
                        if (fabs(kelvin - 4000.0) < 1.0) {
                            e.mask = 0x3u;
                        }
                    });
    struct Standard {
        double kelvin;
        double x;
        double y;
        bool daylight;
    };
    const Standard standards[3] = {{6504.0, 0.3127, 0.3290, true},
                                   {5003.0, 0.3457, 0.3585, true},
                                   {2856.0, 0.44757, 0.40745, false}};
    for (const Standard& standard : standards) {
        ProbeRequest single = MakeProbeRequest(ProbeColorTemperature, 0, 1, 1);
        single.parameters[0] = static_cast<float>(standard.kelvin);
        constexpr double StandardLimit = 0.0005;
        snprintf(label, sizeof(label), "ChromaticAdaptation.CieWhite%.0fK", standard.kelvin);
        CheckFloatProbe(context, label, single, UniformTolerance(StandardLimit, 0.0),
                        [&standard](uint32, const float*, ProbeExpectation& e) {
                            e.value[standard.daylight ? 2 : 0] = standard.x;
                            e.value[standard.daylight ? 3 : 1] = standard.y;
                            e.mask = standard.daylight ? 0xCu : 0x3u;
                        });
    }
}

void RunTransferProbes(ProbeContext& context) noexcept {
    const char* names[4] = {"TransferFunctions.SrgbRec709Rec1886", "TransferFunctions.PqHlg",
                            "TransferFunctions.LutLogGamma", "TransferFunctions.SrgbOverloads"};
    for (uint32 selector = 0; selector < 4; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeTransfer, selector, Width, Height);
        Array<float> gpu(context.allocator);
        CheckFloatProbe(
            context, names[selector], request, UniformTolerance(0.0, 0.0),
            [selector](uint32 c, const float*, ProbeExpectation& e) {
                const double x = ldexp(static_cast<double>(c), -12);
                switch (selector) {
                case 0:
                    e.value[0] = SrgbEncode(x);
                    e.value[1] = SrgbDecode(x);
                    e.value[2] =
                        x < static_cast<double>(0.018f) ? 4.5 * x : 1.099 * pow(x, 0.45) - 0.099;
                    e.value[3] = pow(x, 2.4);
                    e.absolute[0] = PowSpec(e.value[0], x, 1.0 / 2.4) + Arithmetic(3);
                    e.absolute[1] =
                        PowSpec(e.value[1], (x + 0.055) / 1.055, 2.4) + Arithmetic(2) * e.value[1];
                    e.absolute[2] = PowSpec(e.value[2], x, 0.45) + Arithmetic(3);
                    e.absolute[3] = PowSpec(e.value[3], x, 2.4);
                    break;
                case 1:
                    e.value[0] = PqEncode(x * 16384.0);
                    e.value[1] = PqDecode(x);
                    e.value[2] = HlgEncode(x);
                    e.value[3] = HlgDecode(x);
                    // PQ는 m2 = 78.8 거듭제곱이 안쪽 상대 오차를 78.8배로 키운다(사양 전파).
                    e.absolute[0] = 1.0e-4 * e.value[0] + 1.0e-6;
                    e.absolute[1] = 2.0e-4 * e.value[1] + 1.0e-3;
                    e.absolute[2] = 2.0e-6;
                    e.absolute[3] = 2.0e-6 * e.value[3] + 1.0e-7;
                    break;
                case 2: {
                    const double lut = x * 32.0;
                    const double lower = 0.18 * ldexp(1.0, -10);
                    const double v = fmax(lut, lower) / 0.18;
                    e.value[0] = fmin(fmax((log2(v) + 10.0) / 16.5, 0.0), 1.0);
                    e.value[1] = 0.18 * exp2(-10.0 + 16.5 * x);
                    e.value[2] = x > 0.0 ? pow(x, 1.0 / static_cast<float>(2.2)) : 0.0;
                    e.value[3] =
                        x > 0.0 ? pow(x, static_cast<double>(static_cast<float>(2.2))) : 0.0;
                    e.absolute[0] = 4.0 * SpecLog2Absolute / 16.5 + Arithmetic(3);
                    e.absolute[1] = e.value[1] * (3.0 + 2.0 * 6.5) * FloatUlp * 2.0;
                    e.absolute[2] = PowSpec(e.value[2], x, 1.0 / 2.2);
                    e.absolute[3] = PowSpec(e.value[3], x, 2.2);
                    break;
                }
                default:
                    e.value[0] = SrgbEncode(x * 0.5);
                    e.value[1] = SrgbEncode(x * 0.25);
                    e.value[2] = SrgbDecode(x * 0.5);
                    e.value[3] = SrgbDecode(x * 0.25);
                    e.absolute[0] = PowSpec(e.value[0], x * 0.5, 1.0 / 2.4) + Arithmetic(3);
                    e.absolute[1] = PowSpec(e.value[1], x * 0.25, 1.0 / 2.4) + Arithmetic(3);
                    e.absolute[2] = PowSpec(e.value[2], 1.0, 2.4) + 1.0e-6 * e.value[2] + 1.0e-9;
                    e.absolute[3] = PowSpec(e.value[3], 1.0, 2.4) + 1.0e-6 * e.value[3] + 1.0e-9;
                    break;
                }
            });
    }

    // 단조성(사례 순서 = 입력 증가): sRGB·Rec.709·PQ·HLG·LUT log 부호화.
    ProbeRequest monotone = MakeProbeRequest(ProbeTransfer, 0, Width, Height);
    Array<float> a(context.allocator);
    Array<float> b(context.allocator);
    bool monotoneRan = RunFloatProbe(context, "TransferFunctions.MonotonicA", monotone, a);
    monotone.selector = 1;
    monotoneRan =
        monotoneRan && RunFloatProbe(context, "TransferFunctions.MonotonicB", monotone, b);
    if (monotoneRan) {
        uint32 decreasing = 0;
        for (uint32 c = 1; c < CaseCount(monotone); ++c) {
            const uint32 channels[3] = {0, 1, 2};
            for (const uint32 k : channels) {
                decreasing += a[c * 4u + k] < a[(c - 1) * 4u + k] ? 1u : 0u;
            }

            decreasing += b[c * 4u] < b[(c - 1) * 4u] ? 1u : 0u;
            decreasing += b[c * 4u + 2] < b[(c - 1) * 4u + 2] ? 1u : 0u;
        }

        char detail[96];
        snprintf(detail, sizeof(detail), "%u steps, decreasing steps %u", CaseCount(monotone) - 1,
                 decreasing);
        ReportProbe(context, "TransferFunctions.Monotonic", decreasing == 0, detail);
    }

    // 8 bit sRGB 전 256 부호 왕복(부호 일치)과 PQ 표준 값(BT.2100·BT.2408 — 100 nit 0.5081, 203 nit
    // 0.5807, 1000 nit 0.7518, 10000 nit 1).
    ProbeRequest srgb = MakeProbeRequest(ProbeSrgbRoundTrip, 0, 256, 1);
    Array<float> trip(context.allocator);
    if (RunFloatProbe(context, "TransferFunctions.Srgb8RoundTripValues", srgb, trip)) {
        uint32 codeErrors = 0;
        double worst = 0.0;
        for (uint32 c = 0; c < 256; ++c) {
            const float* v = trip.Data() + c * 4u;
            codeErrors += rint(v[1] * 255.0) != c ? 1u : 0u;
            const double reference = SrgbDecode(c / 255.0);
            worst =
                fmax(worst, fabs(v[0] - reference) /
                                (PowSpec(reference, (c / 255.0 + 0.055) / 1.055, 2.4) + 1.0e-7));
        }

        char detail[128];
        snprintf(detail, sizeof(detail),
                 "256 codes, round-trip code errors %u, max decode error %.2f of tolerance",
                 codeErrors, worst);
        ReportProbe(context, "TransferFunctions.Srgb8RoundTrip", codeErrors == 0 && worst <= 1.0,
                    detail);
    }

    ProbeRequest pq = MakeProbeRequest(ProbePqStandard, 0, 8, 1);
    constexpr double PqNits[8] = {0.0, 0.1, 1.0, 100.0, 203.0, 1000.0, 4000.0, 10000.0};
    CheckFloatProbe(context, "TransferFunctions.PqStandardValues", pq, UniformTolerance(0.0, 0.0),
                    [](uint32 c, const float*, ProbeExpectation& e) {
                        e.value[0] = PqEncode(PqNits[c]);
                        e.value[1] = PqNits[c];
                        e.absolute[0] = 1.0e-4 * e.value[0] + 1.0e-6;
                        e.absolute[1] = 2.0e-4 * PqNits[c] + 1.0e-3;
                        e.mask = 0x3u;
                    });
    // 기준 식이 표준 표 값(소수 4자리)과 0.0005 안인가(GPU는 위 대조로 기준 식의 1e-4 안이다).
    constexpr double PqTableNits[5] = {0.0, 100.0, 203.0, 1000.0, 10000.0};
    constexpr double PqTableSignal[5] = {0.0, 0.5081, 0.5807, 0.7518, 1.0};
    double worstTable = 0.0;
    for (uint32 k = 0; k < 5; ++k) {
        worstTable = fmax(worstTable, fabs(PqEncode(PqTableNits[k]) - PqTableSignal[k]));
    }

    constexpr double PqTableLimit = 0.0005;
    char tableDetail[96];
    snprintf(tableDetail, sizeof(tableDetail),
             "max difference to the BT.2100 table %.5f (limit %.4f)", worstTable, PqTableLimit);
    ReportProbe(context, "TransferFunctions.PqStandardTable", worstTable <= PqTableLimit,
                tableDetail);
}

// --- filmic(FilmicToneCurve.rsh와 같은 식, double) ---

struct Film {
    double slope;
    double toe;
    double shoulder;
    double blackClip;
    double whiteClip;
    double blueCorrection;
    double expandGamut;
    double toneCurveAmount;
};

constexpr Matrix3 WideToXyz = {{{0.5441691, 0.2395926, 0.1666943},
                                {0.2394656, 0.7021530, 0.0583814},
                                {-0.0023439, 0.0361834, 1.0552183}}};
constexpr Matrix3 BlueCorrect = {{{0.9404372683, -0.0183068787, 0.0778696104},
                                  {0.0083786969, 0.8286599939, 0.1629613092},
                                  {0.0005471261, -0.0008833746, 1.0003362486}}};
constexpr Matrix3 BlueCorrectInverse = {{{1.06318, 0.0233956, -0.0865726},
                                         {-0.0106337, 1.20632, -0.19569},
                                         {-0.000590887, 0.00105248, 0.999538}}};

double Saturation(const double c[3]) noexcept {
    const double minimum = fmin(c[0], fmin(c[1], c[2]));
    const double maximum = fmax(c[0], fmax(c[1], c[2]));
    return (fmax(maximum, 1e-10) - fmax(minimum, 1e-10)) / fmax(maximum, 1e-2);
}

double Hue(const double c[3]) noexcept {
    double hue = 0.0;
    if (c[0] != c[1] || c[1] != c[2]) {
        hue = 180.0 / Pi * atan2(sqrt(3.0) * (c[1] - c[2]), 2.0 * c[0] - c[1] - c[2]);
    }

    if (hue < 0.0) {
        hue += 360.0;
    }

    return fmin(fmax(hue, 0.0), 360.0);
}

double Yc(const double c[3]) noexcept {
    const double chroma = sqrt(c[2] * (c[2] - c[1]) + c[1] * (c[1] - c[0]) + c[0] * (c[0] - c[2]));
    return (c[2] + c[1] + c[0] + 1.75 * chroma) / 3.0;
}

double Smoothstep(double e0, double e1, double x) noexcept {
    const double t = fmin(fmax((x - e0) / (e1 - e0), 0.0), 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double ToneChannel(double logColor, const Film& film, double toeMatch, double straightMatch,
                   double shoulderMatch) noexcept {
    const double toeScale = 1.0 + film.blackClip - film.toe;
    const double shoulderScale = 1.0 + film.whiteClip - film.shoulder;
    const double straight = film.slope * (logColor + straightMatch);
    double toe = -film.blackClip +
                 2.0 * toeScale / (1.0 + exp(-2.0 * film.slope / toeScale * (logColor - toeMatch)));
    double shoulder =
        1.0 + film.whiteClip -
        2.0 * shoulderScale /
            (1.0 + exp(2.0 * film.slope / shoulderScale * (logColor - shoulderMatch)));
    if (!(logColor < toeMatch)) {
        toe = straight;
    }

    if (!(logColor > shoulderMatch)) {
        shoulder = straight;
    }

    double t = fmin(fmax((logColor - toeMatch) / (shoulderMatch - toeMatch), 0.0), 1.0);
    if (shoulderMatch < toeMatch) {
        t = 1.0 - t;
    }

    t = (3.0 - 2.0 * t) * t * t;
    return toe + (shoulder - toe) * t;
}

void FilmToneMap(const double colorAP1[3], const Film& film, const Matrix3& ap1ToAP0,
                 const Matrix3& ap0ToAP1, const double ap1Y[3], double out[3]) noexcept {
    double ap0[3];
    Apply(ap1ToAP0, colorAP1, ap0);
    const double saturation = Saturation(ap0);
    const double ycIn = Yc(ap0);
    const double x = (saturation - 0.4) / 0.2;
    const double tt = fmax(1.0 - fabs(0.5 * x), 0.0);
    const double s = 0.5 * (1.0 + (x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0)) * (1.0 - tt * tt));
    const double glowGain = 0.05 * s;
    double glow;
    if (ycIn <= 2.0 / 3.0 * 0.08) {
        glow = glowGain;
    } else if (ycIn >= 2.0 * 0.08) {
        glow = 0.0;
    } else {
        glow = glowGain * (0.08 / ycIn - 0.5);
    }

    for (double& v : ap0) {
        v *= 1.0 + glow;
    }

    const double hue = Hue(ap0);
    double centered = hue;
    if (centered < -180.0) {
        centered += 360.0;
    } else if (centered > 180.0) {
        centered -= 360.0;
    }

    const double weight = Smoothstep(0.0, 1.0, 1.0 - fabs(2.0 * centered / 135.0));
    ap0[0] += weight * weight * saturation * (0.03 - ap0[0]) * (1.0 - 0.82);
    double working[3];
    Apply(ap0ToAP1, ap0, working);
    for (double& v : working) {
        v = fmax(v, 0.0);
    }

    const double luma = working[0] * ap1Y[0] + working[1] * ap1Y[1] + working[2] * ap1Y[2];
    for (double& v : working) {
        v = luma + (v - luma) * 0.96;
    }

    const double toeScale = 1.0 + film.blackClip - film.toe;
    double toeMatch;
    if (film.toe > 0.8) {
        toeMatch = (1.0 - film.toe - 0.18) / film.slope + log10(0.18);
    } else {
        const double bt = (0.18 + film.blackClip) / toeScale - 1.0;
        toeMatch = log10(0.18) - 0.5 * log((1.0 + bt) / (1.0 - bt)) * (toeScale / film.slope);
    }

    const double straightMatch = (1.0 - film.toe) / film.slope - toeMatch;
    const double shoulderMatch = film.shoulder / film.slope - straightMatch;
    double tone[3];
    for (int k = 0; k < 3; ++k) {
        tone[k] = ToneChannel(log10(fmax(working[k], 1e-10)), film, toeMatch, straightMatch,
                              shoulderMatch);
    }

    const double toneLuma = tone[0] * ap1Y[0] + tone[1] * ap1Y[1] + tone[2] * ap1Y[2];
    for (int k = 0; k < 3; ++k) {
        out[k] = fmax(toneLuma + (tone[k] - toneLuma) * 0.93, 0.0);
    }
}

void FilmicDisplay(const double linear[3], const Film& film, double out[3]) noexcept {
    const Matrix3 srgbToXyz = RgbToXyz(Rec709Primaries, D65);
    const Matrix3 ap0 = RgbToXyz(AP0Primaries, AcesWhite);
    const Matrix3 ap1 = RgbToXyz(AP1Primaries, AcesWhite);
    const Matrix3 d65ToD60 = Adaptation(Bradford, D65, AcesWhite);
    const Matrix3 d60ToD65 = Adaptation(Bradford, AcesWhite, D65);
    const Matrix3 srgbToAP1 = Multiply(Inverse(ap1), Multiply(d65ToD60, srgbToXyz));
    const Matrix3 ap1ToSrgb = Multiply(Inverse(srgbToXyz), Multiply(d60ToD65, ap1));
    const Matrix3 ap1ToAP0 = Multiply(Inverse(ap0), ap1);
    const Matrix3 ap0ToAP1 = Multiply(Inverse(ap1), ap0);
    const double ap1Y[3] = {ap1.m[1][0], ap1.m[1][1], ap1.m[1][2]};
    double colorAP1[3];
    Apply(srgbToAP1, linear, colorAP1);
    const double luma = colorAP1[0] * ap1Y[0] + colorAP1[1] * ap1Y[1] + colorAP1[2] * ap1Y[2];
    if (luma > 0.0) {
        double distance = 0.0;
        for (const double v : colorAP1) {
            distance += (v / luma - 1.0) * (v / luma - 1.0);
        }

        const double amount =
            (1.0 - exp2(-4.0 * distance)) * (1.0 - exp2(-4.0 * film.expandGamut * luma * luma));
        const Matrix3 expand = Multiply(Multiply(Inverse(ap1), WideToXyz), ap1ToSrgb);
        double expanded[3];
        Apply(expand, colorAP1, expanded);
        for (int k = 0; k < 3; ++k) {
            colorAP1[k] += (expanded[k] - colorAP1[k]) * amount;
        }
    }

    const Matrix3 blue = Multiply(ap0ToAP1, Multiply(BlueCorrect, ap1ToAP0));
    const Matrix3 blueInverse = Multiply(ap0ToAP1, Multiply(BlueCorrectInverse, ap1ToAP0));
    double corrected[3];
    Apply(blue, colorAP1, corrected);
    for (int k = 0; k < 3; ++k) {
        colorAP1[k] += (corrected[k] - colorAP1[k]) * film.blueCorrection;
    }

    double toned[3];
    FilmToneMap(colorAP1, film, ap1ToAP0, ap0ToAP1, ap1Y, toned);
    for (int k = 0; k < 3; ++k) {
        colorAP1[k] += (toned[k] - colorAP1[k]) * film.toneCurveAmount;
    }

    Apply(blueInverse, colorAP1, corrected);
    for (int k = 0; k < 3; ++k) {
        colorAP1[k] += (corrected[k] - colorAP1[k]) * film.blueCorrection;
    }

    Apply(ap1ToSrgb, colorAP1, out);
    for (int k = 0; k < 3; ++k) {
        out[k] = fmax(out[k], 0.0);
    }
}

void RunFilmicProbes(ProbeContext& context) noexcept {
    // UE 5.8 FPostProcessSettings 기본값, 다른 기울기, toe > 0.8 분기.
    const Film films[3] = {{0.88, 0.55, 0.26, 0.0, 0.04, 0.6, 1.0, 1.0},
                           {0.91, 0.53, 0.23, 0.0, 0.035, 0.0, 0.0, 1.0},
                           {1.0, 0.85, 0.3, 0.02, 0.05, 0.3, 0.5, 0.7}};
    char label[96];
    for (uint32 index = 0; index < 3; ++index) {
        ProbeRequest request = MakeProbeRequest(ProbeFilmic, 0, Width, Height);
        const double* fields = &films[index].slope;
        Film rounded;
        double* roundedFields = &rounded.slope;
        for (uint32 k = 0; k < 8; ++k) {
            request.values[k] = static_cast<float>(fields[k]);
            roundedFields[k] = request.values[k];
        }

        // exp·log10·atan2(색상각 4096 ulp) 전파 — 출력 [0, 1] 근처 절대 5e-4.
        constexpr double FilmicTolerance = 5.0e-4;
        snprintf(label, sizeof(label), "FilmicToneCurve.DisplayColor[%u]", index);
        CheckFloatProbe(context, label, request, UniformTolerance(FilmicTolerance, 1.0e-4),
                        [&rounded](uint32 c, const float*, ProbeExpectation& e) {
                            double scene[3];
                            for (uint32 k = 0; k < 3; ++k) {
                                const uint32 scale = ProbeBits(c, 3 + k, 0) % 9u;
                                scene[k] = static_cast<double>(ProbeUnit(c, k, 0)) *
                                           ldexp(1.0, static_cast<int>(scale) - 3);
                            }

                            FilmicDisplay(scene, rounded, e.value);
                            e.mask = 0x7u;
                        });

        // 회색 사다리 [0, 16)의 단조성·연속(이웃 차이 상한 — 구간 경계에서 값이 튀지 않는다).
        request.selector = 1;
        Array<float> ramp(context.allocator);
        snprintf(label, sizeof(label), "FilmicToneCurve.GrayRamp[%u]", index);
        if (RunFloatProbe(context, label, request, ramp)) {
            uint32 decreasing = 0;
            double largestStep = 0.0;
            double worstReference = 0.0;
            for (uint32 c = 0; c < CaseCount(request); ++c) {
                double gray[3] = {c / 256.0, c / 256.0, c / 256.0};
                double expected[3];
                FilmicDisplay(gray, rounded, expected);
                for (uint32 k = 0; k < 3; ++k) {
                    worstReference = fmax(worstReference, fabs(ramp[c * 4u + k] - expected[k]));
                    if (c > 0) {
                        const double step = ramp[c * 4u + k] - ramp[(c - 1) * 4u + k];
                        decreasing += step < -1.0e-6 ? 1u : 0u;
                        largestStep = fmax(largestStep, fabs(step));
                    }
                }
            }

            constexpr double StepLimit = 0.02;
            char detail[160];
            snprintf(
                detail, sizeof(detail),
                "4096 grays, decreasing steps %u, largest step %.4f (limit %.2f), max diff %.3g",
                decreasing, largestStep, StepLimit, worstReference);
            snprintf(label, sizeof(label), "FilmicToneCurve.MonotonicContinuous[%u]", index);
            ReportProbe(context, label,
                        decreasing == 0 && largestStep <= StepLimit &&
                            worstReference <= FilmicTolerance,
                        detail);
        }
    }
}

} // namespace

void RunColorProbes(ProbeContext& context) noexcept {
    RunMatrixProbes(context);
    RunFunctionProbes(context);
    RunAdaptationProbes(context);
    RunTransferProbes(context);
    RunFilmicProbes(context);
}

} // namespace sandbox::engine_shaders
