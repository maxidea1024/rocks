#include <Core/Math/Color.h>

#include <Platform/PlatformMath.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

using platform::PlatformMath;

// HSV 변환이 정의되지 않는 입력의 결과로 쓰는 quiet NaN이다.
constexpr float QuietNaN = PlatformMath::QuietNaN<float>();

// IEC 61966-2-1의 decode 식이다. byte/255와 이후 계산을 double로 수행해 float로 한 번만 반올림한다.
float DecodeSrgbChannel(uint8 encodedByte) noexcept {
    const double encoded = static_cast<double>(encodedByte) / 255.0;
    if (encoded <= 0.04045) {
        return static_cast<float>(encoded / 12.92);
    }

    return static_cast<float>(PlatformMath::Pow((encoded + 0.055) / 1.055, 2.4));
}

// IEC 61966-2-1의 encode 식이다. 포화(NaN → 0 포함)를 먼저 적용하므로 pow에는 [0,1]만 전달된다.
// encode 결과는 [0,1]이고 반올림 규칙은 QuantizeRound와 같은 0.5 올림이다.
uint8 EncodeSrgbChannel(float linearValue) noexcept {
    const double linear = detail::ColorClampUnit(linearValue);
    const double encoded =
        linear <= 0.0031308 ? linear * 12.92 : 1.055 * PlatformMath::Pow(linear, 1.0 / 2.4) - 0.055;

    // 두 구간 식 모두 0 이상이고 linear = 1의 결과는 double 오차 몇 ulp 안의 1이다. 따라서 scaled는
    // [0.5, 256) 안에 있고 절사가 반올림(0.5 올림)이며 결과는 uint8 범위다.
    const double scaled = encoded * 255.0 + 0.5;
    return static_cast<uint8>(scaled);
}

// 결과 Hue를 [0, 360)으로 유지한다. double에서 360 미만이어도 float 변환이 360으로 올릴 수 있고
// 360도는 0도와 같은 색상이다.
float StoreHueDegrees(double hueDegrees) noexcept {
    const float hue = static_cast<float>(hueDegrees);
    return hue >= 360.0f ? 0.0f : hue;
}

} // namespace

LinearColor LinearColor::FromSRGB(Color encoded) noexcept {
    return LinearColor(DecodeSrgbChannel(encoded.r), DecodeSrgbChannel(encoded.g),
                       DecodeSrgbChannel(encoded.b), static_cast<float>(encoded.a) / 255.0f);
}

Color LinearColor::ToSRGB() const noexcept {
    return Color(EncodeSrgbChannel(r), EncodeSrgbChannel(g), EncodeSrgbChannel(b),
                 detail::ColorQuantizeRound(a));
}

LinearColor LinearColor::LinearRGBToHSV() const noexcept {
    if (!PlatformMath::IsFinite(r) || !PlatformMath::IsFinite(g) || !PlatformMath::IsFinite(b)) {
        return LinearColor(QuietNaN, QuietNaN, QuietNaN, a);
    }

    const double red = static_cast<double>(r);
    const double green = static_cast<double>(g);
    const double blue = static_cast<double>(b);
    const double maximum = PlatformMath::Max(PlatformMath::Max(red, green), blue);
    const double minimum = PlatformMath::Min(PlatformMath::Min(red, green), blue);
    const double range = maximum - minimum;

    // 유한 float의 차이는 double에서 overflow하지 않으므로 range가 0이 아니면 나눗셈이 유한하다.
    double hue = 0.0;
    if (range > 0.0) {
        if (maximum == red) {
            hue = 60.0 * ((green - blue) / range);
            if (hue < 0.0) {
                hue += 360.0;
            }
        } else if (maximum == green) {
            hue = 60.0 * ((blue - red) / range) + 120.0;
        } else {
            hue = 60.0 * ((red - green) / range) + 240.0;
        }
    }

    const double saturation = maximum == 0.0 ? 0.0 : range / maximum;
    return LinearColor(StoreHueDegrees(hue), static_cast<float>(saturation),
                       static_cast<float>(maximum), a);
}

LinearColor LinearColor::HSVToLinearRGB() const noexcept {
    if (!PlatformMath::IsFinite(r)) {
        return LinearColor(QuietNaN, QuietNaN, QuietNaN, a);
    }

    // Fmod는 정확하며 결과 부호가 피제수를 따른다. 음수 hue는 360을 더해 [0, 360)으로 옮긴다.
    // 아주 작은 음수에 360을 더하면 360으로 반올림될 수 있으며 이 값은 0도와 같다.
    double hue = PlatformMath::Fmod(static_cast<double>(r), 360.0);
    if (hue < 0.0) {
        hue += 360.0;
    }

    // 360 바로 아래 hue의 hue/60도 6으로 반올림될 수 있다. 6구간은 0구간의 시작과 같다.
    double sectorPosition = hue / 60.0;
    if (sectorPosition >= 6.0) {
        sectorPosition = 0.0;
    }

    const double sectorStart = PlatformMath::Floor(sectorPosition);
    const double fraction = sectorPosition - sectorStart;
    const int32 sector = static_cast<int32>(sectorStart);

    const double saturation = static_cast<double>(g);
    const double value = static_cast<double>(b);
    const double lowest = value * (1.0 - saturation);
    const double falling = value * (1.0 - saturation * fraction);
    const double rising = value * (1.0 - saturation * (1.0 - fraction));

    // 60도 구간마다 최대 성분(value), 최소 성분(lowest), 증가·감소 성분의 위치가 바뀐다.
    double red = 0.0;
    double green = 0.0;
    double blue = 0.0;
    switch (sector) {
    case 0:
        red = value;
        green = rising;
        blue = lowest;
        break;
    case 1:
        red = falling;
        green = value;
        blue = lowest;
        break;
    case 2:
        red = lowest;
        green = value;
        blue = rising;
        break;
    case 3:
        red = lowest;
        green = falling;
        blue = value;
        break;
    case 4:
        red = rising;
        green = lowest;
        blue = value;
        break;
    case 5:
        red = value;
        green = lowest;
        blue = falling;
        break;
    default:
        // 위 정규화로 sectorPosition은 [0, 6)이므로 도달하면 구현 불변식이 깨진 것이다.
        platform::Fatal("LinearColor::HSVToLinearRGB produced an invalid hue sector");
    }

    return LinearColor(static_cast<float>(red), static_cast<float>(green), static_cast<float>(blue),
                       a);
}

} // namespace rocks
