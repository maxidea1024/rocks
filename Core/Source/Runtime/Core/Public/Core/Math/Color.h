#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <Platform/PlatformMath.h>

namespace rocks {

struct LinearColor;

namespace detail {

// 저장용 포화 규칙이다. NaN·음수·-Inf·±0은 0, 1 이상·+Inf는 1이다. NaN은 bit 표현으로 판정하므로
// compiler의 fast-math 설정이 비교식을 단순화해도 NaN이 255로 바뀌지 않는다.
// 결과는 float 값을 정확히 확장한 double이다.
constexpr double ColorClampUnit(float value) noexcept {
    if (platform::PlatformMath::IsNaN(value) || value <= 0.0f) {
        return 0.0;
    }

    if (value >= 1.0f) {
        return 1.0;
    }

    return static_cast<double>(value);
}

// float(24bit 가수) * 255(8bit)는 double(53bit)에서 정확하다. x >= 2^-22이면 0.5를 더한 합도
// 정확하고, 그보다 작은 x는 합이 반올림되어도 1 미만이라 결과 0이 바뀌지 않는다. 따라서 절사 결과는
// 실수 x*255를 반올림(0.5는 올림)한 값과 항상 같다. 결과는 최대 255.5의 절사이므로 uint8 범위다.
constexpr uint8 ColorQuantizeRound(float value) noexcept {
    return static_cast<uint8>(ColorClampUnit(value) * 255.0 + 0.5);
}

constexpr uint8 ColorQuantizeFloor(float value) noexcept {
    return static_cast<uint8>(ColorClampUnit(value) * 255.0);
}

// NaN 성분은 비교가 모두 false이므로 그대로 반환된다.
constexpr float ColorClampChannel(float value, float minimum, float maximum) noexcept {
    if (value < minimum) {
        return minimum;
    }

    if (value > maximum) {
        return maximum;
    }

    return value;
}

// t < 0.5는 시작점에서 더하고 그 밖은 끝점에서 빼므로 유한한 입력의 t=0과 t=1에서 끝점이 정확히
// 나온다. 같은 두 값 사이의 보간은 모든 유한한 t에서 그 값이다. NaN t는 두 번째 식에서 NaN이 된다.
constexpr float ColorLerpChannel(float from, float to, float t) noexcept {
    const double start = static_cast<double>(from);
    const double end = static_cast<double>(to);
    const double weight = static_cast<double>(t);
    const double difference = end - start;

    if (weight < 0.5) {
        return static_cast<float>(start + difference * weight);
    }

    return static_cast<float>(end - difference * (1.0 - weight));
}

// ASCII 16진 숫자 한 글자의 값이다. 그 밖의 byte(NUL·공백·비ASCII 포함)는 -1이다.
constexpr int32 ColorHexDigitValue(char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }

    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }

    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }

    return -1;
}

} // namespace detail

// 채널당 8bit unsigned normalized RGBA 값 타입이다. 내부 상태나 잠금이 없는 일반 값이다.
// byte가 어떤 색 공간의 값인지는 타입이 기록하지 않으므로 변환 함수 이름으로 명시한다.
// - sRGB로 인코딩된 색(일반 이미지·UI 색): LinearColor::FromSRGB / LinearColor::ToSRGB
// - 색이 아닌 데이터(마스크·노멀 등) 또는 QuantizeRound 결과: ReinterpretAsLinear / QuantizeRound
// alpha는 항상 선형 불투명도(straight alpha)다. sRGB 변환을 적용하지 않고 RGB에 미리 곱하지 않는다.
//
// 기본 생성은 모든 채널 0(투명 검정)이다. 채널 생성자의 alpha 기본값은 255(불투명)이므로
// Color()와 Color(0, 0, 0)은 다르다. 예: Color(255, 128, 0), Color::FromPackedRGBA(0xff8000ffu).
//
// 메모리 배치는 멤버 순서대로 r, g, b, a byte지만 파일·네트워크·GPU 포맷 계약이 아니다.
// 외부 포맷에는 ToPacked*/FromPacked* 또는 채널별 byte 쓰기를 사용한다. UE FColor는
// little-endian에서 메모리 순서가 B, G, R, A이고 uint32 Bits가 ARGB 정수와 같으므로 두 타입을
// memcpy로 옮기면 채널이 뒤바뀐다. UE의 Bits 값과 FColor(uint32) 생성자에 해당하는 Rocks 함수는
// ToPackedARGB/FromPackedARGB다.
struct Color {
    uint8 r = 0;
    uint8 g = 0;
    uint8 b = 0;
    uint8 a = 0;

    constexpr Color() noexcept = default;

    constexpr Color(uint8 red, uint8 green, uint8 blue, uint8 alpha = 255) noexcept
        : r(red), g(green), b(blue), a(alpha) {
    }

    friend constexpr bool operator==(const Color&, const Color&) noexcept = default;

    constexpr Color WithAlpha(uint8 alpha) const noexcept {
        return Color(r, g, b, alpha);
    }

    // ---- 32bit 정수 packing ----
    // 함수 이름의 채널 순서대로 정수의 최상위 byte부터 채운다.
    // 예: Color(0x11, 0x22, 0x33, 0x44).ToPackedARGB()는 0x44112233u다.
    // UE ToPacked*와 같은 bit 순서이고 FromPacked*는 그 정확한 역변환이다. 정수의 메모리 byte
    // 순서는 호스트 endian을 따르므로 이 정수를 그대로 파일에 쓰지 않는다.
    constexpr uint32 ToPackedRGBA() const noexcept {
        return Pack(r, g, b, a);
    }

    constexpr uint32 ToPackedARGB() const noexcept {
        return Pack(a, r, g, b);
    }

    constexpr uint32 ToPackedBGRA() const noexcept {
        return Pack(b, g, r, a);
    }

    constexpr uint32 ToPackedABGR() const noexcept {
        return Pack(a, b, g, r);
    }

    static constexpr Color FromPackedRGBA(uint32 packed) noexcept {
        return Color(Byte(packed, 3), Byte(packed, 2), Byte(packed, 1), Byte(packed, 0));
    }

    static constexpr Color FromPackedARGB(uint32 packed) noexcept {
        return Color(Byte(packed, 2), Byte(packed, 1), Byte(packed, 0), Byte(packed, 3));
    }

    static constexpr Color FromPackedBGRA(uint32 packed) noexcept {
        return Color(Byte(packed, 1), Byte(packed, 2), Byte(packed, 3), Byte(packed, 0));
    }

    static constexpr Color FromPackedABGR(uint32 packed) noexcept {
        return Color(Byte(packed, 0), Byte(packed, 1), Byte(packed, 2), Byte(packed, 3));
    }

    // 네 채널을 모두 float 255로 한 번 나눈다(IEEE 정확 반올림). sRGB decode를 하지 않으므로
    // 데이터 텍스처와 QuantizeRound 결과의 역변환에 사용한다. 256개 값 모두 QuantizeRound로 같은
    // byte가 된다. UE는 c * (1/255) 곱셈이라 256개 중 126개 byte에서 1ulp 크다(예: 3).
    constexpr LinearColor ReinterpretAsLinear() const noexcept;

    // ---- 16진수 문자열 ----
    // WriteHex 출력 버퍼의 크기다. 16진수 8자와 끝의 NUL이다.
    static constexpr usize HexTextCapacity = 9;

    // 선택적 '#' 하나 뒤의 16진 숫자(대소문자 무관)만 허용한다. 허용 길이는 다음 넷이다.
    //   3 RGB      → 각 숫자를 반복한다(F → FF). alpha는 255다.
    //   4 RGBA     → 3자리와 같고 alpha를 포함한다(CSS Color 4 형식).
    //   6 RRGGBB   → alpha는 255다.
    //   8 RRGGBBAA
    // 빈 문자열, '#'만 있는 문자열, 다른 길이, 공백, "0x" 접두사, 부호, NUL, 비ASCII byte는 실패다.
    // 실패하면 false를 반환하고 output을 바꾸지 않는다. 앞뒤 공백을 자르지 않으므로 필요하면
    // 호출자가 StringView::TrimAscii를 먼저 적용한다.
    // UE FColor::FromHex는 3/6/8자리만 받고 다른 길이(4자리 포함)는 (0,0,0,0)을 반환한다. 글자의
    // 유효성은 검사하지 않고 FParse::HexDigit 값을 그대로 조합하므로 실패를 구분할 수 없다.
    [[nodiscard]] static constexpr bool TryParseHex(StringView text, Color& output) noexcept {
        const char* digits = text.Data();
        usize digitCount = text.ByteCount();
        if (digitCount != 0 && digits[0] == '#') {
            ++digits;
            --digitCount;
        }

        if (digitCount != 3 && digitCount != 4 && digitCount != 6 && digitCount != 8) {
            return false;
        }

        // 모든 글자를 검사한 뒤 output을 한 번에 쓴다. 중간 실패가 일부 채널만 바꾸지 않는다.
        uint8 nibbles[8] = {};
        for (usize index = 0; index < digitCount; ++index) {
            const int32 value = detail::ColorHexDigitValue(digits[index]);
            if (value < 0) {
                return false;
            }
            nibbles[index] = static_cast<uint8>(value);
        }

        const bool shortForm = digitCount <= 4;
        const usize channelCount = (digitCount == 4 || digitCount == 8) ? 4u : 3u;
        uint8 channels[4] = {0, 0, 0, 255};
        for (usize channel = 0; channel < channelCount; ++channel) {
            if (shortForm) {
                channels[channel] = static_cast<uint8>(nibbles[channel] * 17);
            } else {
                const usize high = channel * 2;
                channels[channel] = static_cast<uint8>((nibbles[high] << 4) | nibbles[high + 1]);
            }
        }

        output = Color(channels[0], channels[1], channels[2], channels[3]);
        return true;
    }

    // "RRGGBBAA" 대문자 16진수 8자와 NUL을 output에 쓰며 할당하지 않는다. UE ToHex와 같은 채널
    // 순서·대소문자이고 TryParseHex로 같은 색을 되읽는다.
    constexpr void WriteHex(char (&output)[HexTextCapacity]) const noexcept {
        constexpr char digits[] = "0123456789ABCDEF";
        const uint8 channels[4] = {r, g, b, a};
        for (usize channel = 0; channel < 4; ++channel) {
            output[channel * 2] = digits[channels[channel] >> 4];
            output[channel * 2 + 1] = digits[channels[channel] & 0x0f];
        }
        output[8] = '\0';
    }

    // ---- 이름 있는 색: 정적 초기화 순서 문제가 없도록 constexpr 함수로 제공한다. ----
    // sRGB byte로 해석하면 sRGB 원색·2차색이다. 모두 불투명이며 Transparent만 (0,0,0,0)이다.
    static constexpr Color White() noexcept {
        return Color(255, 255, 255);
    }

    static constexpr Color Black() noexcept {
        return Color(0, 0, 0);
    }

    static constexpr Color Transparent() noexcept {
        return Color(0, 0, 0, 0);
    }

    static constexpr Color Red() noexcept {
        return Color(255, 0, 0);
    }

    static constexpr Color Green() noexcept {
        return Color(0, 255, 0);
    }

    static constexpr Color Blue() noexcept {
        return Color(0, 0, 255);
    }

    static constexpr Color Yellow() noexcept {
        return Color(255, 255, 0);
    }

    static constexpr Color Cyan() noexcept {
        return Color(0, 255, 255);
    }

    static constexpr Color Magenta() noexcept {
        return Color(255, 0, 255);
    }

private:
    static constexpr uint32 Pack(uint8 first, uint8 second, uint8 third, uint8 fourth) noexcept {
        return (static_cast<uint32>(first) << 24) | (static_cast<uint32>(second) << 16) |
               (static_cast<uint32>(third) << 8) | static_cast<uint32>(fourth);
    }

    // byteIndex 0이 최하위 byte다.
    static constexpr uint8 Byte(uint32 packed, uint32 byteIndex) noexcept {
        return static_cast<uint8>((packed >> (byteIndex * 8)) & 0xffu);
    }
};

// 선형 RGB와 straight alpha의 float 색이다. RGB 원색과 백색점은 sRGB/Rec.709(D65)다.
// RGB는 HDR을 위해 [0,1] 밖의 값(음수 포함)을 그대로 보관하고 산술 연산은 범위를 제한하지 않는다.
// alpha는 RGB에 미리 곱하지 않은 선형 불투명도다. [0,1] 포화는 8bit 변환(ToSRGB, QuantizeRound,
// QuantizeFloor)과 명시적 GetClamped에서만 일어난다.
//
// 기본 생성은 모든 성분 0(투명 검정)이며 RGB 생성자의 alpha 기본값은 1이다.
// operator==는 성분별 정확 비교다. NaN 성분은 자기 자신과도 같지 않고 -0.0과 +0.0은 같다.
// 오차 비교는 Equals를 사용한다.
// UE FLinearColor와 달리 Color에서의 암시적 변환 생성자(UE에서는 sRGB decode)가 없다. 호출자가
// FromSRGB(sRGB decode)와 Color::ReinterpretAsLinear(단순 /255) 중 하나를 명시한다.
struct LinearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;

    constexpr LinearColor() noexcept = default;

    constexpr LinearColor(float red, float green, float blue, float alpha = 1.0f) noexcept
        : r(red), g(green), b(blue), a(alpha) {
    }

    friend constexpr bool operator==(const LinearColor&, const LinearColor&) noexcept = default;

    constexpr LinearColor WithAlpha(float alpha) const noexcept {
        return LinearColor(r, g, b, alpha);
    }

    // ---- 산술: alpha를 포함한 성분별 IEEE float 연산이다. 범위 제한과 특수값 보정은 없다. ----
    constexpr LinearColor operator+(const LinearColor& other) const noexcept {
        return LinearColor(r + other.r, g + other.g, b + other.b, a + other.a);
    }

    constexpr LinearColor operator-(const LinearColor& other) const noexcept {
        return LinearColor(r - other.r, g - other.g, b - other.b, a - other.a);
    }

    constexpr LinearColor operator*(const LinearColor& other) const noexcept {
        return LinearColor(r * other.r, g * other.g, b * other.b, a * other.a);
    }

    constexpr LinearColor operator*(float scalar) const noexcept {
        return LinearColor(r * scalar, g * scalar, b * scalar, a * scalar);
    }

    // 성분마다 정확히 나눈다. UE는 1/scalar를 곱하므로 결과가 1ulp 다를 수 있다.
    // 0으로 나누면 IEEE 규칙대로 ±Inf 또는 NaN이다.
    constexpr LinearColor operator/(float scalar) const noexcept {
        return LinearColor(r / scalar, g / scalar, b / scalar, a / scalar);
    }

    constexpr LinearColor& operator+=(const LinearColor& other) noexcept {
        *this = *this + other;
        return *this;
    }

    constexpr LinearColor& operator-=(const LinearColor& other) noexcept {
        *this = *this - other;
        return *this;
    }

    constexpr LinearColor& operator*=(float scalar) noexcept {
        *this = *this * scalar;
        return *this;
    }

    // ---- sRGB 변환 ----
    // RGB에 IEC 61966-2-1 decode를 적용한다. c = byte/255일 때 c <= 0.04045이면 c/12.92, 아니면
    // ((c+0.055)/1.055)^2.4다. 결과는 정확한 값을 float로 반올림한 값이며(결정적 수학 모듈의 생성
    // 표 — 정확한 유리수·정수 비교로 만들어 플랫폼·CRT와 무관하다) alpha는 변환 없이 /255다. 256개
    // byte 모두 ToSRGB로 같은 byte가 된다. UE는 미리 계산한 float 표를 읽으며 그 표는 25개 byte에서
    // 정확 반올림보다 1ulp 작다.
    static LinearColor FromSRGB(Color encoded) noexcept;

    // RGB를 [0,1]로 포화한 뒤 sRGB encode하고 255배 값을 반올림(0.5는 올림)한다.
    //   encode(c) = c <= 0.0031308 ? 12.92c : 1.055c^(1/2.4) - 0.055
    // alpha는 sRGB 변환 없이 QuantizeRound와 같은 규칙이다.
    // 특수값은 채널마다 독립적이다: NaN·음수·-Inf → 0, 1 초과·+Inf → 255.
    // UE ToFColorSRGB는 구간 표 보간 encoder(stb 방식)라서 [2^-13, 1)의 float 입력 중 547,648개에서
    // 정확 반올림과 ±1 다르다. 예: 0x1.3dap-13f는 UE 1, Rocks 0이다. byte round trip은 둘 다
    // 성립한다. Rocks는 결정적 수학 모듈의 경계 표(정확한 비교로 생성)를 이분 탐색하므로 모든
    // 입력에서 정확한 반올림이고 플랫폼과 무관하다.
    Color ToSRGB() const noexcept;

    // sRGB 변환 없이 네 성분을 clamp(x,0,1)*255의 가장 가까운 정수로 바꾼다(0.5는 올림).
    // 실수 x*255의 정확한 반올림이며 float 입력의 정확한 동점은 x = 0.5(→128) 하나뿐이다.
    // Color::ReinterpretAsLinear의 역변환이다.
    // 특수값: NaN·음수·-Inf → 0, 1 초과·+Inf → 255. D3D FLOAT→UNORM 변환의 NaN → 0 규칙과 같다.
    // UE QuantizeRound는 같은 특수값 규칙이지만 (uint8)(0.5f + x*255.f)를 float로 계산한다.
    // 그래서 반올림 경계 바로 아래의 float 128개에서 1 큰 값을 낸다.
    // 예: 0x1.020202p-1f는 UE 129, Rocks 128이다.
    constexpr Color QuantizeRound() const noexcept {
        return Color(detail::ColorQuantizeRound(r), detail::ColorQuantizeRound(g),
                     detail::ColorQuantizeRound(b), detail::ColorQuantizeRound(a));
    }

    // clamp(x,0,1)*255의 내림이다. 1.0만 255가 되므로 GPU UNORM 변환과 다르다. 이미지·텍스처에는
    // QuantizeRound를 사용한다. 특수값 규칙은 QuantizeRound와 같고 [0,1]의 모든 float에서 UE
    // QuantizeFloor와 같은 결과다(전수 비교).
    constexpr Color QuantizeFloor() const noexcept {
        return Color(detail::ColorQuantizeFloor(r), detail::ColorQuantizeFloor(g),
                     detail::ColorQuantizeFloor(b), detail::ColorQuantizeFloor(a));
    }

    // ---- 범위·비교·분류 ----
    // alpha를 포함한 각 성분을 [minimum, maximum]으로 제한한다. NaN 성분은 NaN으로 남긴다. NaN을
    // 그럴듯한 값으로 바꾸는 저장용 포화는 Quantize*/ToSRGB가 담당한다. minimum <= maximum이 아니면
    // (NaN 경계 포함) 계약 위반으로 Fatal이다. UE GetClamped는 Max(Min(x, max), min)이라 NaN이
    // max가 된다.
    constexpr LinearColor GetClamped(float minimum = 0.0f, float maximum = 1.0f) const noexcept {
        if (!(minimum <= maximum)) {
            platform::Fatal("LinearColor::GetClamped requires minimum <= maximum");
        }

        return LinearColor(detail::ColorClampChannel(r, minimum, maximum),
                           detail::ColorClampChannel(g, minimum, maximum),
                           detail::ColorClampChannel(b, minimum, maximum),
                           detail::ColorClampChannel(a, minimum, maximum));
    }

    // 네 성분 모두 |차이| <= tolerance면 true다. 차이는 float로 계산한다. NaN 성분이나 NaN
    // tolerance는 false이고 음수 tolerance는 항상 false다. 같은 부호의 ±Inf 성분끼리도 차이가
    // NaN이므로 false다. 정확한 동일성은 operator==로 검사한다. Core Vector의 Equals와 같은 포함
    // 경계다. UE Equals는 엄격한 < 비교와 기본값 1e-4를 쓰지만 Rocks는 전역 기본 오차를 두지
    // 않는다.
    constexpr bool Equals(const LinearColor& other, float tolerance) const noexcept {
        using platform::PlatformMath;
        return PlatformMath::Abs(r - other.r) <= tolerance &&
               PlatformMath::Abs(g - other.g) <= tolerance &&
               PlatformMath::Abs(b - other.b) <= tolerance &&
               PlatformMath::Abs(a - other.a) <= tolerance;
    }

    // 네 성분 모두 NaN·±Inf가 아니면 true다. HDR 값(>1, 음수)은 유한값이다.
    constexpr bool IsFinite() const noexcept {
        using platform::PlatformMath;
        return PlatformMath::IsFinite(r) && PlatformMath::IsFinite(g) &&
               PlatformMath::IsFinite(b) && PlatformMath::IsFinite(a);
    }

    // 한 성분이라도 NaN이면 true다. ±Inf는 NaN이 아니다.
    constexpr bool ContainsNaN() const noexcept {
        using platform::PlatformMath;
        return PlatformMath::IsNaN(r) || PlatformMath::IsNaN(g) || PlatformMath::IsNaN(b) ||
               PlatformMath::IsNaN(a);
    }

    // 선형 RGB의 상대 휘도 Y = 0.2126R + 0.7152G + 0.0722B다(Rec.709/sRGB 원색, 계수 합 1).
    // alpha는 무시하고 HDR·음수 값은 그대로 가중한다. double로 계산하므로 White()는 정확히 1이다.
    // sRGB 인코딩 값이나 다른 원색의 RGB에 적용하면 의미가 없다. UE GetLuminance는
    // r.LegacyLuminanceFactors가 켜져 있으면 0.3/0.59/0.11, 아니면 작업 색 공간의 계수를 첫 호출 때
    // 정적 변수에 캐시해 쓰므로 실행 설정에 따라 결과가 달라진다.
    constexpr float Luminance() const noexcept {
        const double luminance = 0.2126 * static_cast<double>(r) + 0.7152 * static_cast<double>(g) +
                                 0.0722 * static_cast<double>(b);
        return static_cast<float>(luminance);
    }

    // alpha를 포함한 성분별 선형 보간이다. 유한한 입력에서 t=0이면 정확히 from, t=1이면 정확히
    // to이고 [0,1] 밖의 t는 외삽한다. double로 계산해 float로 한 번 반올림하며 float 범위를 넘는
    // 외삽은 ±Inf가 된다. NaN t는 NaN이다. straight alpha이므로 alpha가 다른 색을 보간하면 투명한
    // 쪽의 RGB가 섞여 보인다. 합성용 보간에는 RGB에 alpha를 곱한 값을 사용한다.
    static constexpr LinearColor Lerp(const LinearColor& from, const LinearColor& to,
                                      float t) noexcept {
        return LinearColor(
            detail::ColorLerpChannel(from.r, to.r, t), detail::ColorLerpChannel(from.g, to.g, t),
            detail::ColorLerpChannel(from.b, to.b, t), detail::ColorLerpChannel(from.a, to.a, t));
    }

    // ---- HSV: UE와 같은 성분 규약(r = Hue 도, g = Saturation, b = Value, a = alpha 유지) ----
    // Value = max(R,G,B), Saturation = (max-min)/max(max가 0이면 0), Hue는 [0, 360) 도다.
    // 무채색(max == min)의 Hue는 0이고 최대 성분이 여럿이면 R, G, B 순으로 우선한다.
    // 음수 성분도 같은 식으로 계산하지만 round trip은 음수가 아닌 유한 RGB에서만 보장한다.
    // RGB에 NaN·Inf가 있으면 H, S, V가 모두 NaN이다. double로 계산해 float로 반올림한다.
    LinearColor LinearRGBToHSV() const noexcept;

    // this를 r = Hue(도), g = Saturation, b = Value, a = alpha로 읽어 선형 RGB를 만든다. Hue는
    // 360도로 감는다(360·720 → 0, -90 → 270). S·V는 제한하지 않으며 V > 1은 HDR 값이 된다. Hue가
    // NaN·Inf면 RGB가 모두 NaN이고 alpha는 유지한다. UE는 음수 Hue를 uint32로 변환해 정의되지 않은
    // 동작이 된다. 색 공간의 지각적 균일성은 없는 단순 기하 변환이며 선형 RGB 값에 직접 적용된다.
    LinearColor HSVToLinearRGB() const noexcept;

    // ---- 이름 있는 색: 선형 값이며 Color의 같은 이름과 sRGB 변환으로 대응한다. ----
    static constexpr LinearColor White() noexcept {
        return LinearColor(1.0f, 1.0f, 1.0f);
    }

    static constexpr LinearColor Black() noexcept {
        return LinearColor(0.0f, 0.0f, 0.0f);
    }

    static constexpr LinearColor Transparent() noexcept {
        return LinearColor(0.0f, 0.0f, 0.0f, 0.0f);
    }

    static constexpr LinearColor Red() noexcept {
        return LinearColor(1.0f, 0.0f, 0.0f);
    }

    static constexpr LinearColor Green() noexcept {
        return LinearColor(0.0f, 1.0f, 0.0f);
    }

    static constexpr LinearColor Blue() noexcept {
        return LinearColor(0.0f, 0.0f, 1.0f);
    }

    static constexpr LinearColor Yellow() noexcept {
        return LinearColor(1.0f, 1.0f, 0.0f);
    }

    static constexpr LinearColor Cyan() noexcept {
        return LinearColor(0.0f, 1.0f, 1.0f);
    }

    static constexpr LinearColor Magenta() noexcept {
        return LinearColor(1.0f, 0.0f, 1.0f);
    }
};

constexpr LinearColor operator*(float scalar, const LinearColor& color) noexcept {
    return color * scalar;
}

constexpr LinearColor Color::ReinterpretAsLinear() const noexcept {
    return LinearColor(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                       static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
}

static_assert(sizeof(Color) == 4 && alignof(Color) == 1);
static_assert(sizeof(LinearColor) == 16 && alignof(LinearColor) == 4);
static_assert(__is_trivially_copyable(Color) && __is_trivially_copyable(LinearColor));

} // namespace rocks
