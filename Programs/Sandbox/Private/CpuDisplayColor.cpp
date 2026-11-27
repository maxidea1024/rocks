#include "CpuDisplayColor.h"

#include <math.h>

namespace sandbox {

namespace {

// --- CPU 기준 tone map(Engine/Shaders/Private/Color/FilmicToneCurve.rsh와 같은 식) ---

struct Matrix3 {
    double m[3][3];
};

struct Color3 {
    double c[3];
};

constexpr Matrix3 AP0ToXyz = {{{0.9525523959, 0.0000000000, 0.0000936786},
                               {0.3439664498, 0.7281660966, -0.0721325464},
                               {0.0000000000, 0.0000000000, 1.0088251844}}};
constexpr Matrix3 XyzToAP0 = {{{1.0498110175, 0.0000000000, -0.0000974845},
                               {-0.4959030231, 1.3733130458, 0.0982400361},
                               {0.0000000000, 0.0000000000, 0.9912520182}}};
constexpr Matrix3 AP1ToXyz = {{{0.6624541811, 0.1340042065, 0.1561876870},
                               {0.2722287168, 0.6740817658, 0.0536895174},
                               {-0.0055746495, 0.0040607335, 1.0103391003}}};
constexpr Matrix3 XyzToAP1 = {{{1.6410233797, -0.3248032942, -0.2364246952},
                               {-0.6636628587, 1.6153315917, 0.0167563477},
                               {0.0117218943, -0.0082844420, 0.9883948585}}};
constexpr Matrix3 AP0ToAP1 = {{{1.4514393161, -0.2365107469, -0.2149285693},
                               {-0.0765537734, 1.1762296998, -0.0996759264},
                               {0.0083161484, -0.0060324498, 0.9977163014}}};
constexpr Color3 AP1ToY = {{0.2722287168, 0.6740817658, 0.0536895174}};
constexpr Matrix3 XyzToSrgb = {{{3.2409699419, -1.5373831776, -0.4986107603},
                                {-0.9692436363, 1.8759675015, 0.0415550574},
                                {0.0556300797, -0.2039769589, 1.0569715142}}};
constexpr Matrix3 SrgbToXyz = {{{0.4123907993, 0.3575843394, 0.1804807884},
                                {0.2126390059, 0.7151686788, 0.0721923154},
                                {0.0193308187, 0.1191947798, 0.9505321522}}};
constexpr Matrix3 D65ToD60 = {{{1.0130349146, 0.0061052578, -0.0149709436},
                               {0.0076982301, 0.9981633521, -0.0050320385},
                               {-0.0028413174, 0.0046851567, 0.9245061375}}};
constexpr Matrix3 D60ToD65 = {{{0.9872240087, -0.0061132286, 0.0159532883},
                               {-0.0075983718, 1.0018614847, 0.0053300358},
                               {0.0030725771, -0.0050959615, 1.0816806031}}};
constexpr Matrix3 WideToXyz = {{{0.5441691, 0.2395926, 0.1666943},
                                {0.2394656, 0.7021530, 0.0583814},
                                {-0.0023439, 0.0361834, 1.0552183}}};
constexpr Matrix3 BlueCorrect = {{{0.9404372683, -0.0183068787, 0.0778696104},
                                  {0.0083786969, 0.8286599939, 0.1629613092},
                                  {0.0005471261, -0.0008833746, 1.0003362486}}};
constexpr Matrix3 BlueCorrectInverse = {{{1.06318, 0.0233956, -0.0865726},
                                         {-0.0106337, 1.20632, -0.19569},
                                         {-0.000590887, 0.00105248, 0.999538}}};

Matrix3 Multiply(const Matrix3& a, const Matrix3& b) noexcept {
    Matrix3 out{};
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            double sum = 0.0;
            for (int k = 0; k < 3; ++k) {
                sum += a.m[row][k] * b.m[k][column];
            }

            out.m[row][column] = sum;
        }
    }

    return out;
}

Color3 Apply(const Matrix3& a, const Color3& v) noexcept {
    Color3 out{};
    for (int row = 0; row < 3; ++row) {
        out.c[row] = a.m[row][0] * v.c[0] + a.m[row][1] * v.c[1] + a.m[row][2] * v.c[2];
    }

    return out;
}

double Dot(const Color3& a, const Color3& b) noexcept {
    return a.c[0] * b.c[0] + a.c[1] * b.c[1] + a.c[2] * b.c[2];
}

Color3 Lerp(const Color3& a, const Color3& b, double t) noexcept {
    Color3 out{};
    for (int index = 0; index < 3; ++index) {
        out.c[index] = a.c[index] + (b.c[index] - a.c[index]) * t;
    }

    return out;
}

Color3 LerpScalar(double a, const Color3& b, double t) noexcept {
    return Lerp(Color3{{a, a, a}}, b, t);
}

double Clamp01(double value) noexcept {
    return value < 0.0 ? 0.0 : (value > 1.0 ? 1.0 : value);
}

double Smoothstep(double edge0, double edge1, double x) noexcept {
    const double t = Clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0 - 2.0 * t);
}

double FilmChannel(double logColor, const render::TonemapConstants& constants, double toeMatch,
                   double straightMatch, double shoulderMatch) noexcept {
    const double slope = constants.film[0];
    const double toe = constants.film[1];
    const double shoulder = constants.film[2];
    const double blackClip = constants.film[3];
    const double whiteClip = constants.filmWhite[0];
    const double toeScale = 1.0 + blackClip - toe;
    const double shoulderScale = 1.0 + whiteClip - shoulder;
    const double straight = slope * (logColor + straightMatch);
    double toeColor =
        -blackClip +
        (2.0 * toeScale) / (1.0 + exp((-2.0 * slope / toeScale) * (logColor - toeMatch)));
    double shoulderColor =
        (1.0 + whiteClip) - (2.0 * shoulderScale) / (1.0 + exp((2.0 * slope / shoulderScale) *
                                                               (logColor - shoulderMatch)));
    if (!(logColor < toeMatch)) {
        toeColor = straight;
    }

    if (!(logColor > shoulderMatch)) {
        shoulderColor = straight;
    }

    double t = Clamp01((logColor - toeMatch) / (shoulderMatch - toeMatch));
    if (shoulderMatch < toeMatch) {
        t = 1.0 - t;
    }

    t = (3.0 - 2.0 * t) * t * t;
    return toeColor + (shoulderColor - toeColor) * t;
}

Color3 FilmToneMap(const Color3& colorAP1, const render::TonemapConstants& constants) noexcept {
    Color3 colorAP0 = Apply(Multiply(XyzToAP0, AP1ToXyz), colorAP1);
    const double minimum = fmin(fmin(colorAP0.c[0], colorAP0.c[1]), colorAP0.c[2]);
    const double maximum = fmax(fmax(colorAP0.c[0], colorAP0.c[1]), colorAP0.c[2]);
    const double saturation = (fmax(maximum, 1e-10) - fmax(minimum, 1e-10)) / fmax(maximum, 1e-2);
    const double r = colorAP0.c[0];
    const double g = colorAP0.c[1];
    const double b = colorAP0.c[2];
    const double chroma = sqrt(b * (b - g) + g * (g - r) + r * (r - b));
    const double yc = (b + g + r + 1.75 * chroma) / 3.0;
    const double x = (saturation - 0.4) / 0.2;
    const double shaperT = fmax(1.0 - fabs(0.5 * x), 0.0);
    const double sign = x > 0.0 ? 1.0 : (x < 0.0 ? -1.0 : 0.0);
    const double s = 0.5 * (1.0 + sign * (1.0 - shaperT * shaperT));
    const double glowGain = 0.05 * s;
    const double glowMid = 0.08;
    double glow = 0.0;
    if (yc <= 2.0 / 3.0 * glowMid) {
        glow = glowGain;
    } else if (yc < 2.0 * glowMid) {
        glow = glowGain * (glowMid / yc - 0.5);
    }

    for (double& channel : colorAP0.c) {
        channel *= 1.0 + glow;
    }

    double hue = 0.0;
    if (colorAP0.c[0] != colorAP0.c[1] || colorAP0.c[1] != colorAP0.c[2]) {
        hue =
            (180.0 / 3.14159265358979) * atan2(sqrt(3.0) * (colorAP0.c[1] - colorAP0.c[2]),
                                               2.0 * colorAP0.c[0] - colorAP0.c[1] - colorAP0.c[2]);
    }

    if (hue < 0.0) {
        hue += 360.0;
    }

    double centered = hue;
    if (centered > 180.0) {
        centered -= 360.0;
    }

    const double weight = Smoothstep(0.0, 1.0, 1.0 - fabs(2.0 * centered / 135.0));
    colorAP0.c[0] += weight * weight * saturation * (0.03 - colorAP0.c[0]) * (1.0 - 0.82);
    Color3 working = Apply(AP0ToAP1, colorAP0);
    for (double& channel : working.c) {
        channel = fmax(channel, 0.0);
    }

    working = LerpScalar(Dot(working, AP1ToY), working, 0.96);
    const double slope = constants.film[0];
    const double toe = constants.film[1];
    const double shoulder = constants.film[2];
    const double blackClip = constants.film[3];
    const double toeScale = 1.0 + blackClip - toe;
    double toeMatch = 0.0;
    if (toe > 0.8) {
        toeMatch = (1.0 - toe - 0.18) / slope + log10(0.18);
    } else {
        const double bt = (0.18 + blackClip) / toeScale - 1.0;
        toeMatch = log10(0.18) - 0.5 * log((1.0 + bt) / (1.0 - bt)) * (toeScale / slope);
    }

    const double straightMatch = (1.0 - toe) / slope - toeMatch;
    const double shoulderMatch = shoulder / slope - straightMatch;
    Color3 tone{};
    for (int index = 0; index < 3; ++index) {
        tone.c[index] = FilmChannel(log10(fmax(working.c[index], 1e-10)), constants, toeMatch,
                                    straightMatch, shoulderMatch);
    }

    tone = LerpScalar(Dot(tone, AP1ToY), tone, 0.93);
    for (double& channel : tone.c) {
        channel = fmax(channel, 0.0);
    }

    return tone;
}

Color3 FilmicDisplayColor(const Color3& linearSrgb,
                          const render::TonemapConstants& constants) noexcept {
    const Matrix3 srgbToAP1 = Multiply(XyzToAP1, Multiply(D65ToD60, SrgbToXyz));
    const Matrix3 ap1ToSrgb = Multiply(XyzToSrgb, Multiply(D60ToD65, AP1ToXyz));
    const Matrix3 ap1ToAP0 = Multiply(XyzToAP0, AP1ToXyz);
    const Matrix3 ap0ToAP1 = Multiply(XyzToAP1, AP0ToXyz);
    Color3 colorAP1 = Apply(srgbToAP1, linearSrgb);
    const double luma = Dot(colorAP1, AP1ToY);
    if (luma > 0.0) {
        double distance = 0.0;
        for (double channel : colorAP1.c) {
            const double offset = channel / luma - 1.0;
            distance += offset * offset;
        }

        const double amount = (1.0 - exp2(-4.0 * distance)) *
                              (1.0 - exp2(-4.0 * constants.exposure[2] * luma * luma));
        const Matrix3 expand = Multiply(Multiply(XyzToAP1, WideToXyz), ap1ToSrgb);
        colorAP1 = Lerp(colorAP1, Apply(expand, colorAP1), amount);
    }

    const Matrix3 blue = Multiply(ap0ToAP1, Multiply(BlueCorrect, ap1ToAP0));
    const Matrix3 blueInverse = Multiply(ap0ToAP1, Multiply(BlueCorrectInverse, ap1ToAP0));
    const double blueAmount = constants.exposure[1];
    colorAP1 = Lerp(colorAP1, Apply(blue, colorAP1), blueAmount);
    colorAP1 = Lerp(colorAP1, FilmToneMap(colorAP1, constants), constants.exposure[3]);
    colorAP1 = Lerp(colorAP1, Apply(blueInverse, colorAP1), blueAmount);
    Color3 out = Apply(ap1ToSrgb, colorAP1);
    for (double& channel : out.c) {
        channel = fmax(channel, 0.0);
    }

    return out;
}

double ApplySwizzleSource(const float (&texel)[4], graphics::SwizzleSource source) noexcept {
    switch (source) {
    case graphics::SwizzleSource::Red:
        return texel[0];
    case graphics::SwizzleSource::Green:
        return texel[1];
    case graphics::SwizzleSource::Blue:
        return texel[2];
    case graphics::SwizzleSource::Alpha:
        return texel[3];
    case graphics::SwizzleSource::One:
        return 1.0;
    case graphics::SwizzleSource::Zero:
        break;
    }

    return 0.0;
}

} // namespace

int EncodeSrgb8(double linear) noexcept {
    const double value = Clamp01(linear);
    const double encoded =
        value <= 0.0031308 ? value * 12.92 : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<int>(floor(encoded * 255.0 + 0.5));
}

DisplayPixel ExpectedTonemapPixel(const float (&scene)[3],
                                  const render::TonemapConstants& constants) noexcept {
    const Color3 display = FilmicDisplayColor(
        Color3{{scene[0] * constants.exposure[0], scene[1] * constants.exposure[0],
                scene[2] * constants.exposure[0]}},
        constants);
    return DisplayPixel{
        {EncodeSrgb8(display.c[0]), EncodeSrgb8(display.c[1]), EncodeSrgb8(display.c[2])}};
}

DisplayPixel ExpectedInspectorPixel(const screen::InspectorTexture& texture,
                                    const float (&texel)[4],
                                    const render::TonemapConstants& constants) noexcept {
    double value[4];
    for (uint32 channel = 0; channel < 4; ++channel) {
        value[channel] = ApplySwizzleSource(texel, texture.swizzle.Channel(channel));
        if (texture.component != screen::InspectorComponent::Float) {
            value[channel] *= texture.integerScale;
        }
    }

    switch (texture.display) {
    case render::TextureInspectorDisplay::Hdr: {
        Color3 color{{fmax(value[0] * constants.exposure[0], 0.0),
                      fmax(value[1] * constants.exposure[0], 0.0),
                      fmax(value[2] * constants.exposure[0], 0.0)}};
        const Color3 display = FilmicDisplayColor(color, constants);
        return DisplayPixel{
            {EncodeSrgb8(display.c[0]), EncodeSrgb8(display.c[1]), EncodeSrgb8(display.c[2])}};
    }

    case render::TextureInspectorDisplay::Depth:
        return DisplayPixel{{EncodeSrgb8(value[0]), EncodeSrgb8(value[0]), EncodeSrgb8(value[0])}};
    case render::TextureInspectorDisplay::Normal: {
        // MaterialTextureSampling.rsh DecodeTangentNormal: xy = 2 rg - 1, z = sqrt(saturate(1 -
        // xy·xy)).
        const double x = value[0] * 2.0 - 1.0;
        const double y = value[1] * 2.0 - 1.0;
        const double z = sqrt(Clamp01(1.0 - x * x - y * y));
        return DisplayPixel{
            {EncodeSrgb8(x * 0.5 + 0.5), EncodeSrgb8(y * 0.5 + 0.5), EncodeSrgb8(z * 0.5 + 0.5)}};
    }

    case render::TextureInspectorDisplay::Ldr:
        break;
    }

    return DisplayPixel{{EncodeSrgb8(value[0]), EncodeSrgb8(value[1]), EncodeSrgb8(value[2])}};
}

} // namespace sandbox
