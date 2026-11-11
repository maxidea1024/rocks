#include <Core/Math/Color.h>
#include <Platform/PlatformMath.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

using platform::PlatformMath;

constexpr float NaN = __builtin_nanf("");
constexpr float Infinity = __builtin_inff();

// 양의 유한 float의 바로 위·아래 값이다. 반올림 경계 양쪽을 검사하는 데 사용한다.
float NextUp(float value) noexcept {
    return __builtin_bit_cast(float, __builtin_bit_cast(uint32, value) + 1u);
}

float NextDown(float value) noexcept {
    return __builtin_bit_cast(float, __builtin_bit_cast(uint32, value) - 1u);
}

bool Near(float actual, double expected, double tolerance) noexcept {
    return PlatformMath::Abs(static_cast<double>(actual) - expected) <= tolerance;
}

// 테스트 대상과 별개로 표준 식을 그대로 계산한다. 정확 비교는 아래 Python 골든값이 담당한다.
double ReferenceDecode(uint32 byte) noexcept {
    const double encoded = static_cast<double>(byte) / 255.0;
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }
    return PlatformMath::Pow((encoded + 0.055) / 1.055, 2.4);
}

LinearColor WithChannel(LinearColor color, uint32 channel, float value) noexcept {
    float* channels[] = {&color.r, &color.g, &color.b, &color.a};
    *channels[channel] = value;
    return color;
}

float ChannelOf(const LinearColor& color, uint32 channel) noexcept {
    const float channels[] = {color.r, color.g, color.b, color.a};
    return channels[channel];
}

uint8 ChannelOf(Color color, uint32 channel) noexcept {
    const uint8 channels[] = {color.r, color.g, color.b, color.a};
    return channels[channel];
}

uint32 NextRandom(uint32& state) noexcept {
    state = state * 1664525u + 1013904223u;
    return state;
}

// 실패하면 sentinel을 그대로 돌려주므로 상수 평가에서도 성공 여부와 결과를 함께 확인할 수 있다.
constexpr Color ParseOr(StringView text, Color sentinel) noexcept {
    Color output = sentinel;
    if (!Color::TryParseHex(text, output)) {
        return sentinel;
    }
    return output;
}

constexpr bool HexTextIs(Color color, const char (&expected)[Color::HexTextCapacity]) noexcept {
    char text[Color::HexTextCapacity] = {};
    color.WriteHex(text);
    for (usize index = 0; index < Color::HexTextCapacity; ++index) {
        if (text[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

double HueDistance(double first, double second) noexcept {
    const double distance = PlatformMath::Abs(first - second);
    return distance > 180.0 ? 360.0 - distance : distance;
}

} // namespace

TEST(ColorStorageDefaultsAndNamedConstants) {
    static_assert(sizeof(Color) == 4 && sizeof(LinearColor) == 16);

    // 기본 생성은 0이지만 채널 생성자의 alpha 기본값은 불투명이다.
    static_assert(Color() == Color(0, 0, 0, 0));
    static_assert(Color(1, 2, 3) == Color(1, 2, 3, 255));
    static_assert(Color() != Color(0, 0, 0));
    static_assert(LinearColor() == LinearColor(0.0f, 0.0f, 0.0f, 0.0f));
    static_assert(LinearColor(0.25f, 0.5f, 2.0f).a == 1.0f);
    static_assert(Color(1, 2, 3, 4).WithAlpha(9) == Color(1, 2, 3, 9));
    static_assert(LinearColor(1.0f, 2.0f, 3.0f, 4.0f).WithAlpha(0.5f) ==
                  LinearColor(1.0f, 2.0f, 3.0f, 0.5f));

    static_assert(Color::White() == Color(255, 255, 255, 255));
    static_assert(Color::Black() == Color(0, 0, 0, 255));
    static_assert(Color::Transparent() == Color(0, 0, 0, 0));
    static_assert(Color::Red() == Color(255, 0, 0, 255));
    static_assert(Color::Green() == Color(0, 255, 0, 255));
    static_assert(Color::Blue() == Color(0, 0, 255, 255));
    static_assert(Color::Yellow() == Color(255, 255, 0, 255));
    static_assert(Color::Cyan() == Color(0, 255, 255, 255));
    static_assert(Color::Magenta() == Color(255, 0, 255, 255));

    // 0과 1은 sRGB 변환과 단순 양자화의 결과가 같으므로 두 상수 집합이 같은 색을 가리킨다.
    static_assert(LinearColor::White().QuantizeRound() == Color::White());
    static_assert(LinearColor::Black().QuantizeRound() == Color::Black());
    static_assert(LinearColor::Transparent().QuantizeRound() == Color::Transparent());
    static_assert(LinearColor::Red().QuantizeRound() == Color::Red());
    static_assert(LinearColor::Green().QuantizeRound() == Color::Green());
    static_assert(LinearColor::Blue().QuantizeRound() == Color::Blue());
    static_assert(LinearColor::Yellow().QuantizeRound() == Color::Yellow());
    static_assert(LinearColor::Cyan().QuantizeRound() == Color::Cyan());
    static_assert(LinearColor::Magenta().QuantizeRound() == Color::Magenta());
    CHECK(LinearColor::Magenta().ToSRGB() == Color::Magenta());
    CHECK(LinearColor::FromSRGB(Color::Cyan()) == LinearColor::Cyan());
}

TEST(ColorPackingOrdersAreMostSignificantByteFirst) {
    // 모든 byte가 다른 값이어야 채널 순서 오류가 대칭으로 가려지지 않는다.
    constexpr Color value(0x11, 0x22, 0x33, 0x44);
    static_assert(value.ToPackedRGBA() == 0x11223344u);
    static_assert(value.ToPackedARGB() == 0x44112233u);
    static_assert(value.ToPackedBGRA() == 0x33221144u);
    static_assert(value.ToPackedABGR() == 0x44332211u);
    static_assert(Color::FromPackedRGBA(0x11223344u) == value);
    static_assert(Color::FromPackedARGB(0x44112233u) == value);
    static_assert(Color::FromPackedBGRA(0x33221144u) == value);
    static_assert(Color::FromPackedABGR(0x44332211u) == value);

    // 같은 정수를 다른 순서로 읽으면 다른 색이다.
    CHECK(Color::FromPackedRGBA(0x11223344u) == Color(0x11, 0x22, 0x33, 0x44));
    CHECK(Color::FromPackedARGB(0x11223344u) == Color(0x22, 0x33, 0x44, 0x11));
    CHECK(Color::FromPackedBGRA(0x11223344u) == Color(0x33, 0x22, 0x11, 0x44));
    CHECK(Color::FromPackedABGR(0x11223344u) == Color(0x44, 0x33, 0x22, 0x11));

    // 최상위 byte가 0x80 이상이어도 부호 확장 없이 uint32로 조합된다.
    static_assert(Color(0x80, 0x00, 0x00, 0xff).ToPackedARGB() == 0xff800000u);
    static_assert(Color(0xff, 0xfe, 0xfd, 0xfc).ToPackedRGBA() == 0xfffefdfcu);

    uint32 state = 0x9e3779b9u;
    for (uint32 iteration = 0; iteration < 4096; ++iteration) {
        const uint32 packed = NextRandom(state);
        CHECK(Color::FromPackedRGBA(packed).ToPackedRGBA() == packed);
        CHECK(Color::FromPackedARGB(packed).ToPackedARGB() == packed);
        CHECK(Color::FromPackedBGRA(packed).ToPackedBGRA() == packed);
        CHECK(Color::FromPackedABGR(packed).ToPackedABGR() == packed);

        const Color color = Color::FromPackedRGBA(packed);
        CHECK(Color::FromPackedARGB(color.ToPackedARGB()) == color);
        CHECK(Color::FromPackedBGRA(color.ToPackedBGRA()) == color);
        CHECK(Color::FromPackedABGR(color.ToPackedABGR()) == color);
    }
}

TEST(ColorReinterpretAsLinearDividesBy255Exactly) {
    // 3/255는 UE의 c * (1/255) 곱셈에서 0x1.818184p-7로 1ulp 커지는 값이다.
    static_assert(Color(3, 0, 0, 0).ReinterpretAsLinear().r == 0x1.818182p-7f);
    static_assert(Color(255, 0, 0, 255).ReinterpretAsLinear() == LinearColor(1.0f, 0.0f, 0.0f));

    for (uint32 value = 0; value < 256; ++value) {
        const uint8 byte = static_cast<uint8>(value);
        const uint8 mirrored = static_cast<uint8>(255 - value);
        const Color color(byte, mirrored, byte, mirrored);
        const LinearColor linear = color.ReinterpretAsLinear();

        // 독립 기준: double 몫을 float로 반올림한 값. c/255의 이진 전개는 8bit 주기라 이중
        // 반올림이 생기지 않으므로 float 정확 나눗셈과 같아야 한다.
        CHECK(linear.r == static_cast<float>(static_cast<double>(value) / 255.0));
        CHECK(linear.g == static_cast<float>(static_cast<double>(255 - value) / 255.0));
        CHECK(linear.b == linear.r);
        CHECK(linear.a == linear.g);

        // sRGB decode를 하지 않으므로 QuantizeRound가 정확한 역변환이다.
        CHECK(linear.QuantizeRound() == color);
    }
}

TEST(ColorSrgbDecodeMatchesIndependentGoldens) {
    // 골든값: Python 3.13(python -I)에서 IEC 61966-2-1 decode 식을 double로 계산해 float32로
    // 반올림한 비트 값이다. 예: 128 → 0.21586050011389926 → 0x1.ba1512p-3f.
    // 10은 0.04045 이하의 선형 구간, 11은 pow 구간의 첫 값이다. 11은 UE 표가 1ulp 작은
    // 0x1.b6a31ap-9f를 갖는 25개 byte 중 하나다.
    struct Golden {
        uint8 byte;
        float linear;
    };

    constexpr Golden goldens[] = {
        {0, 0.0f},
        {1, 0x1.3e4568p-12f},
        {10, 0x1.8dd6c2p-9f},
        {11, 0x1.b6a31cp-9f},
        {64, 0x1.a3ffd8p-5f},
        {128, 0x1.ba1512p-3f},
        {188, 0x1.017a56p-1f},
        {254, 0x1.fb71bcp-1f},
        {255, 1.0f},
    };

    for (const Golden& golden : goldens) {
        const Color encoded(golden.byte, golden.byte, golden.byte, golden.byte);
        const LinearColor decoded = LinearColor::FromSRGB(encoded);
        CHECK(decoded.r == golden.linear);
        CHECK(decoded.g == golden.linear);
        CHECK(decoded.b == golden.linear);
        CHECK(decoded.a == static_cast<float>(golden.byte) / 255.0f);
    }
}

TEST(ColorSrgbRoundTripsEveryByteAndMatchesFormula) {
    float previous = -1.0f;
    for (uint32 value = 0; value < 256; ++value) {
        const uint8 byte = static_cast<uint8>(value);
        const Color encoded(byte, static_cast<uint8>(255 - value), static_cast<uint8>(value * 7),
                            static_cast<uint8>(value * 13));
        const LinearColor decoded = LinearColor::FromSRGB(encoded);

        // float 한 번 반올림의 오차(상대 2^-24)보다 넉넉하고 이웃 byte 간격보다 훨씬 작다.
        const double expected = ReferenceDecode(value);
        CHECK(Near(decoded.r, expected, expected * 1.2e-7));
        CHECK(Near(decoded.g, ReferenceDecode(255 - value), 1.2e-7));
        CHECK(decoded.a == static_cast<float>(encoded.a) / 255.0f);

        // decode는 강한 단조 증가여야 서로 다른 byte가 같은 선형 값으로 합쳐지지 않는다.
        CHECK(decoded.r > previous);
        previous = decoded.r;

        CHECK(decoded.ToSRGB() == encoded);
    }
}

TEST(ColorSrgbEncodeBoundaries) {
    // Python 오라클이 찾은 인접 float 쌍이다. below는 code-1, atOrAbove는 code로 인코딩된다.
    // 두 값의 encode*255는 각각 경계 k-0.5에서 1e-8 이상 떨어져 있어 libm 오차의 영향이 없다.
    struct Threshold {
        uint8 code;
        float below;
        float atOrAbove;
    };

    constexpr Threshold thresholds[] = {
        {1, 0x1.3e4566p-13f, 0x1.3e4568p-13f}, {10, 0x1.79f26ap-9f, 0x1.79f26cp-9f},
        {11, 0x1.a1e5a0p-9f, 0x1.a1e5a2p-9f},  {64, 0x1.9d91a2p-5f, 0x1.9d91a4p-5f},
        {128, 0x1.b65b32p-3f, 0x1.b65b34p-3f}, {200, 0x1.2610dap-1f, 0x1.2610dcp-1f},
        {255, 0x1.fdb81ap-1f, 0x1.fdb81cp-1f},
    };

    for (const Threshold& threshold : thresholds) {
        const uint8 lower = static_cast<uint8>(threshold.code - 1);
        const float below = threshold.below;
        const float above = threshold.atOrAbove;
        CHECK(LinearColor(below, below, below).ToSRGB() == Color(lower, lower, lower));
        CHECK(LinearColor(above, above, above).ToSRGB() ==
              Color(threshold.code, threshold.code, threshold.code));
    }

    // 0.0031308f(0x1.9a5c38p-9)는 double 0.0031308보다 조금 커서 pow 구간, 바로 아래 float는
    // 선형 구간이다. 두 식의 값은 10.3147 근처로 이어지므로 모두 10이다.
    CHECK(LinearColor(0.0031308f, 0x1.9a5c36p-9f, 0.0f).ToSRGB() == Color(10, 10, 0));

    // 0.5 → 187.516, 0.18 → 117.646, 0.75 → 224.610 (Python 오라클 값 * 255).
    CHECK(LinearColor(0.5f, 0.18f, 0.75f).ToSRGB() == Color(188, 118, 225));

    // encode*255 = 0.49898이므로 정확한 결과는 0이다. UE의 표 보간 encoder는 1을 낸다.
    CHECK(LinearColor(0x1.3dap-13f, 0.0f, 0.0f).ToSRGB() == Color(0, 0, 0));
}

TEST(ColorSaturationIsPerChannelForNaNInfinityAndRange) {
    // 각 특수값을 한 채널에만 넣어 다른 채널과 섞이지 않는지 확인한다. 나머지 채널 0.5는
    // QuantizeRound 128, QuantizeFloor 127, sRGB 188, alpha 128이 기준이다.
    struct Special {
        float value;
        uint8 expected;
    };

    const Special specials[] = {
        {NaN, 0},
        {-NaN, 0},
        {-Infinity, 0},
        {-1.0f, 0},
        {-0.0f, 0},
        {-1.0e-30f, 0},
        {1.0e-45f, 0},
        {Infinity, 255},
        {2.0f, 255},
        {0x1.000002p+0f, 255},
        {3.4028235e38f, 255},
        {1.0f, 255},
    };

    const LinearColor base(0.5f, 0.5f, 0.5f, 0.5f);
    for (uint32 channel = 0; channel < 4; ++channel) {
        for (const Special& special : specials) {
            const LinearColor color = WithChannel(base, channel, special.value);
            const Color rounded = color.QuantizeRound();
            const Color floored = color.QuantizeFloor();
            const Color srgb = color.ToSRGB();
            for (uint32 other = 0; other < 4; ++other) {
                if (other == channel) {
                    CHECK(ChannelOf(rounded, other) == special.expected);
                    CHECK(ChannelOf(floored, other) == special.expected);
                    CHECK(ChannelOf(srgb, other) == special.expected);
                } else {
                    CHECK(ChannelOf(rounded, other) == 128);
                    CHECK(ChannelOf(floored, other) == 127);
                    CHECK(ChannelOf(srgb, other) == (other == 3 ? 128 : 188));
                }
            }
        }
    }
}

TEST(ColorAlphaIsNeverGammaEncoded) {
    CHECK(LinearColor(0.5f, 0.5f, 0.5f, 0.5f).ToSRGB() == Color(188, 188, 188, 128));

    for (uint32 value = 0; value < 256; ++value) {
        const uint8 byte = static_cast<uint8>(value);
        const LinearColor decoded = LinearColor::FromSRGB(Color(byte, byte, byte, byte));
        CHECK(decoded.a == static_cast<float>(value) / 255.0f);

        // 중간 byte의 sRGB decode는 /255보다 작으므로 alpha가 decode되었다면 RGB와 같아진다.
        if (value != 0 && value != 255) {
            CHECK(decoded.r < decoded.a);
        }

        CHECK(decoded.ToSRGB().a == byte);
        CHECK(LinearColor(0.0f, 0.0f, 0.0f, decoded.a).ToSRGB().a == byte);
    }
}

TEST(ColorQuantizationIsExactAtRoundingBoundaries) {
    // float 입력의 정확한 .5 동점은 0.5 하나다. 반올림은 128, 내림은 127이다.
    static_assert(LinearColor(0.5f, 0.0f, 1.0f, 0.0f).QuantizeRound() == Color(128, 0, 255, 0));
    static_assert(LinearColor(0.5f, 0.0f, 1.0f, 0.0f).QuantizeFloor() == Color(127, 0, 255, 0));

    // UE의 0.5f + x*255.f는 float 곱이 k+0.5로 올라가 1 큰 값을 내는 입력이다.
    // 정확한 x*255: 128.49999994, 0.49999997, 254.49999243.
    static_assert(LinearColor(0x1.020202p-1f, 0x1.0101p-9f, 0x1.fefefep-1f, 0.0f).QuantizeRound() ==
                  Color(128, 0, 254, 0));

    // 1보다 작은 가장 큰 float는 반올림 255, 내림 254다.
    static_assert(LinearColor(0x1.fffffep-1f, 0.0f, 0.0f, 0.0f).QuantizeRound().r == 255);
    static_assert(LinearColor(0x1.fffffep-1f, 0.0f, 0.0f, 0.0f).QuantizeFloor().r == 254);

    // 모든 반올림 경계 (k+0.5)/255와 내림 경계 k/255의 인접 float를 검사한다. x*255는 double에서
    // 정확하므로 경계와의 비교식 자체가 독립 기준이다.
    for (uint32 code = 0; code < 255; ++code) {
        const float center = static_cast<float>((static_cast<double>(code) + 0.5) / 255.0);
        const float samples[] = {NextDown(center), center, NextUp(center)};
        for (float sample : samples) {
            const double scaled = static_cast<double>(sample) * 255.0;
            const uint8 expected = static_cast<uint8>(scaled - code < 0.5 ? code : code + 1);
            CHECK(LinearColor(sample, sample, sample, sample).QuantizeRound() ==
                  Color(expected, expected, expected, expected));
        }
    }

    for (uint32 code = 1; code < 256; ++code) {
        const float center = static_cast<float>(static_cast<double>(code) / 255.0);
        const float samples[] = {NextDown(center), center, NextUp(center)};
        for (float sample : samples) {
            // code = 255의 NextUp(1.0)은 1을 넘으므로 포화되어 255가 된다.
            const double scaled = static_cast<double>(sample) * 255.0;
            const uint8 expected = static_cast<uint8>(scaled >= code ? code : code - 1);
            CHECK(LinearColor(sample, 0.0f, 0.0f, 0.0f).QuantizeFloor().r == expected);
        }
    }
}

TEST(ColorHexParsesAcceptedLengths) {
    struct Case {
        const char* text;
        Color expected;
    };

    const Case cases[] = {
        {"F80", Color(0xff, 0x88, 0x00, 0xff)},
        {"#F80", Color(0xff, 0x88, 0x00, 0xff)},
        {"f80a", Color(0xff, 0x88, 0x00, 0xaa)},
        {"#F80A", Color(0xff, 0x88, 0x00, 0xaa)},
        {"FF8000", Color(0xff, 0x80, 0x00, 0xff)},
        {"#ff8000", Color(0xff, 0x80, 0x00, 0xff)},
        {"FF800080", Color(0xff, 0x80, 0x00, 0x80)},
        {"#Ff80007f", Color(0xff, 0x80, 0x00, 0x7f)},
        {"01234567", Color(0x01, 0x23, 0x45, 0x67)},
        {"89ABCDEF", Color(0x89, 0xab, 0xcd, 0xef)},
        {"#89abcdef", Color(0x89, 0xab, 0xcd, 0xef)},
        {"#000", Color(0, 0, 0, 255)},
        {"0000", Color(0, 0, 0, 0)},
        {"00000000", Color(0, 0, 0, 0)},
    };

    for (const Case& testCase : cases) {
        Color output(1, 2, 3, 4);
        CHECK(Color::TryParseHex(testCase.text, output));
        CHECK(output == testCase.expected);
    }

    static_assert(ParseOr("#12345678", Color()) == Color(0x12, 0x34, 0x56, 0x78));
    static_assert(ParseOr("abc", Color()) == Color(0xaa, 0xbb, 0xcc, 0xff));
}

TEST(ColorHexRejectsInvalidInputWithoutChangingOutput) {
    // UE FromHex는 길이가 맞지 않으면 (0,0,0,0)을 반환하고 글자 유효성은 검사하지 않는다.
    // Rocks는 모두 실패로 보고하고 output을 보존해야 한다. "\xc3\xbf\x38\x30"은 UTF-8 'ÿ' 뒤의
    // "80"으로 길이 4이지만 비ASCII byte가 있다.
    const char* const invalid[] = {
        "",
        "#",
        "##F80",
        "F",
        "FF",
        "FF800",
        "FF80000",
        "FF8000000",
        "#FF800",
        "#FF80000",
        "#FF8000000",
        "FG8000",
        "#FF80G0",
        "GGG",
        " FF8000",
        "FF8000 ",
        "0xFF8000",
        "+F80",
        "-F80",
        "F80#",
        "#F8#",
        "FF 800",
        "\xc3\xbf\x38\x30",
        "FF8000\n",
    };

    const Color sentinel(1, 2, 3, 4);
    for (const char* text : invalid) {
        Color output = sentinel;
        CHECK(!Color::TryParseHex(text, output));
        CHECK(output == sentinel);
    }

    // NUL은 C 문자열 생성자에서 끝으로 읽히므로 길이를 명시한 view로 전달한다.
    const char embeddedNul[] = {'F', 'F', '\0', '0', '0', '0'};
    const char trailingNul[] = {'F', '8', '0', '\0'};
    const char hashThenNul[] = {'#', '\0', '0', '0'};
    Color output = sentinel;
    CHECK(!Color::TryParseHex(StringView(embeddedNul, 6), output));
    CHECK(!Color::TryParseHex(StringView(trailingNul, 4), output));
    CHECK(!Color::TryParseHex(StringView(hashThenNul, 4), output));
    CHECK(!Color::TryParseHex(StringView(), output));
    CHECK(output == sentinel);

    static_assert(ParseOr("#12345", Color(9, 9, 9, 9)) == Color(9, 9, 9, 9));
}

TEST(ColorWriteHexIsUppercaseRrggbbaaAndRoundTrips) {
    static_assert(HexTextIs(Color(0x01, 0xab, 0xcd, 0xef), "01ABCDEF"));
    static_assert(HexTextIs(Color(), "00000000"));
    static_assert(HexTextIs(Color::White(), "FFFFFFFF"));

    // 버퍼를 다른 글자로 채워 끝 NUL까지 실제로 쓰는지 확인한다.
    char text[Color::HexTextCapacity] = {'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x', 'x'};
    Color(0x12, 0x34, 0x56, 0x78).WriteHex(text);
    CHECK(StringView(text) == StringView("12345678"));
    CHECK(text[8] == '\0');

    uint32 state = 0x2545f491u;
    for (uint32 iteration = 0; iteration < 4096; ++iteration) {
        const Color color = Color::FromPackedRGBA(NextRandom(state));
        color.WriteHex(text);
        Color parsed;
        CHECK(Color::TryParseHex(StringView(text, 8), parsed));
        CHECK(parsed == color);
        CHECK(StringView(text).ByteCount() == 8);
    }
}

TEST(ColorHsvMatchesIndependentGoldens) {
    // 기대값: Python colorsys.rgb_to_hsv 결과의 hue * 360. 동률 최대 성분은 R, G, B 순으로
    // 우선한다.
    struct Golden {
        LinearColor rgb;
        double hue;
        double saturation;
        double value;
    };

    const Golden goldens[] = {
        {LinearColor(1.0f, 0.5f, 0.0f, 0.25f), 30.0, 1.0, 1.0},
        {LinearColor(0.25f, 0.5f, 0.75f), 210.0, 2.0 / 3.0, 0.75},
        {LinearColor(0.5f, 0.25f, 0.75f), 270.0, 2.0 / 3.0, 0.75},
        {LinearColor(1.0f, 0.0f, 0.5f), 330.0, 1.0, 1.0},
        {LinearColor(0.5f, 0.5f, 0.5f), 0.0, 0.0, 0.5},
        {LinearColor(0.0f, 0.0f, 0.0f), 0.0, 0.0, 0.0},
        {LinearColor(2.0f, 1.0f, 0.0f), 30.0, 1.0, 2.0},
        {LinearColor(1.0f, 1.0f, 0.0f), 60.0, 1.0, 1.0},
        {LinearColor(0.0f, 1.0f, 1.0f), 180.0, 1.0, 1.0},
        {LinearColor(1.0f, 0.0f, 1.0f), 300.0, 1.0, 1.0},
    };

    for (const Golden& golden : goldens) {
        const LinearColor hsv = golden.rgb.LinearRGBToHSV();
        CHECK(Near(hsv.r, golden.hue, 1.0e-5));
        CHECK(Near(hsv.g, golden.saturation, 1.0e-7));
        CHECK(Near(hsv.b, golden.value, 0.0));
        CHECK(hsv.a == golden.rgb.a);
    }

    // 정의할 수 없는 입력은 그럴듯한 색 대신 NaN을 내고 alpha는 유지한다.
    const LinearColor invalid[] = {
        LinearColor(NaN, 0.5f, 0.5f, 0.25f),
        LinearColor(0.5f, Infinity, 0.5f, 0.25f),
        LinearColor(0.5f, 0.5f, -Infinity, 0.25f),
    };
    for (const LinearColor& color : invalid) {
        const LinearColor hsv = color.LinearRGBToHSV();
        CHECK(PlatformMath::IsNaN(hsv.r) && PlatformMath::IsNaN(hsv.g) &&
              PlatformMath::IsNaN(hsv.b));
        CHECK(hsv.a == 0.25f);
    }
}

TEST(ColorHsvToRgbWrapsHueAndKeepsAlpha) {
    const LinearColor orange(1.0f, 0.5f, 0.0f, 0.5f);
    const float equivalentHues[] = {30.0f, 390.0f, 750.0f, -330.0f, -690.0f};
    for (float hue : equivalentHues) {
        CHECK(LinearColor(hue, 1.0f, 1.0f, 0.5f).HSVToLinearRGB() == orange);
    }

    // 360의 배수는 0도와 같다. -1e-30 + 360은 double에서 360으로 반올림되어 0도로 감긴다.
    const float redHues[] = {0.0f, 360.0f, 720.0f, -360.0f, -0.0f, -1.0e-30f};
    for (float hue : redHues) {
        CHECK(LinearColor(hue, 1.0f, 1.0f).HSVToLinearRGB() == LinearColor(1.0f, 0.0f, 0.0f));
    }

    CHECK(LinearColor(-90.0f, 1.0f, 1.0f).HSVToLinearRGB() == LinearColor(0.5f, 0.0f, 1.0f));
    CHECK(LinearColor(270.0f, 1.0f, 1.0f).HSVToLinearRGB() == LinearColor(0.5f, 0.0f, 1.0f));
    CHECK(LinearColor(-1.0e-6f, 1.0f, 1.0f).HSVToLinearRGB().Equals(LinearColor::Red(), 1.0e-7f));

    // 1e9 = 360 * 2777777 + 280이며 Python colorsys의 280도 결과는 (2/3, 0, 1)이다.
    CHECK(LinearColor(1.0e9f, 1.0f, 1.0f)
              .HSVToLinearRGB()
              .Equals(LinearColor(2.0f / 3.0f, 0.0f, 1.0f), 1.0e-6f));

    // 채도 0은 hue와 무관하게 회색이다. HDR 값은 제한하지 않는다.
    const float anyHues[] = {0.0f, 59.5f, 123.0f, 359.9f, -45.0f, 1000.0f};
    for (float hue : anyHues) {
        CHECK(LinearColor(hue, 0.0f, 0.25f).HSVToLinearRGB() == LinearColor(0.25f, 0.25f, 0.25f));
    }
    CHECK(LinearColor(120.0f, 0.5f, 4.0f).HSVToLinearRGB() == LinearColor(2.0f, 4.0f, 2.0f));

    const float invalidHues[] = {NaN, Infinity, -Infinity};
    for (float hue : invalidHues) {
        const LinearColor rgb = LinearColor(hue, 0.5f, 0.5f, 0.75f).HSVToLinearRGB();
        CHECK(PlatformMath::IsNaN(rgb.r) && PlatformMath::IsNaN(rgb.g) &&
              PlatformMath::IsNaN(rgb.b));
        CHECK(rgb.a == 0.75f);
    }
}

TEST(ColorHsvRoundTripsOnGrid) {
    const float saturations[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    const float values[] = {0.0f, 0.125f, 0.5f, 1.0f, 3.0f};
    for (uint32 step = 0; step < 24; ++step) {
        const float hue = static_cast<float>(step) * 15.0f;
        for (float saturation : saturations) {
            for (float value : values) {
                const LinearColor rgb = LinearColor(hue, saturation, value, 0.75f).HSVToLinearRGB();
                const LinearColor hsv = rgb.LinearRGBToHSV();
                const double scale = value > 1.0f ? value : 1.0;
                CHECK(hsv.a == 0.75f);
                CHECK(hsv.r >= 0.0f && hsv.r < 360.0f);
                CHECK(Near(hsv.b, value, 1.0e-7 * scale));

                // V = 0이면 S와 H, S = 0이면 H가 정의되지 않으므로 0으로 돌아온다.
                if (value == 0.0f) {
                    CHECK(hsv.r == 0.0f && hsv.g == 0.0f);
                } else if (saturation == 0.0f) {
                    CHECK(hsv.r == 0.0f && hsv.g == 0.0f);
                } else {
                    CHECK(Near(hsv.g, saturation, 1.0e-6));
                    CHECK(HueDistance(hsv.r, hue) <= 1.0e-4);
                }

                CHECK(hsv.HSVToLinearRGB().Equals(rgb, static_cast<float>(1.0e-6 * scale)));
            }
        }
    }

    // RGB 격자에서 시작하는 역방향 round trip이다. 음수가 아닌 유한 값만 보장 대상이다.
    const float channels[] = {0.0f, 0.1f, 0.5f, 0.9f, 1.0f, 2.0f};
    for (float red : channels) {
        for (float green : channels) {
            for (float blue : channels) {
                const LinearColor rgb(red, green, blue, 0.5f);
                const LinearColor back = rgb.LinearRGBToHSV().HSVToLinearRGB();
                CHECK(back.Equals(rgb, 2.0e-6f));
            }
        }
    }
}

TEST(ColorLerpEndpointsAndExtrapolation) {
    constexpr LinearColor from(0.1f, -2.0f, 1.0e20f, 0.0f);
    constexpr LinearColor to(0.7f, 3.0f, -1.0e-20f, 1.0f);
    static_assert(LinearColor::Lerp(from, to, 0.0f) == from);
    static_assert(LinearColor::Lerp(from, to, 1.0f) == to);
    static_assert(LinearColor::Lerp(from, from, 0.37f) == from);

    constexpr LinearColor zero;
    constexpr LinearColor target(1.0f, 2.0f, 4.0f, 1.0f);
    static_assert(LinearColor::Lerp(zero, target, 0.5f) == LinearColor(0.5f, 1.0f, 2.0f, 0.5f));
    static_assert(LinearColor::Lerp(zero, target, 2.0f) == LinearColor(2.0f, 4.0f, 8.0f, 2.0f));
    static_assert(LinearColor::Lerp(zero, target, -1.0f) ==
                  LinearColor(-1.0f, -2.0f, -4.0f, -1.0f));

    // 임의의 유한 값에서도 끝점이 정확해야 반복 보간에서 끝 색이 어긋나지 않는다.
    uint32 state = 0x1234567u;
    for (uint32 iteration = 0; iteration < 4096; ++iteration) {
        const auto randomFloat = [&state]() noexcept {
            return static_cast<float>(static_cast<int32>(NextRandom(state) >> 8) - 0x800000) *
                   0x1.0p-12f;
        };
        const LinearColor start(randomFloat(), randomFloat(), randomFloat(), randomFloat());
        const LinearColor end(randomFloat(), randomFloat(), randomFloat(), randomFloat());
        CHECK(LinearColor::Lerp(start, end, 0.0f) == start);
        CHECK(LinearColor::Lerp(start, end, 1.0f) == end);
    }

    const LinearColor invalid = LinearColor::Lerp(from, to, NaN);
    CHECK(PlatformMath::IsNaN(invalid.r) && PlatformMath::IsNaN(invalid.g) &&
          PlatformMath::IsNaN(invalid.b) && PlatformMath::IsNaN(invalid.a));
}

TEST(ColorLuminanceUsesRec709Weights) {
    static_assert(LinearColor::White().Luminance() == 1.0f);
    static_assert(LinearColor::Black().Luminance() == 0.0f);
    static_assert(LinearColor::Red().Luminance() == 0.2126f);
    static_assert(LinearColor::Green().Luminance() == 0.7152f);
    static_assert(LinearColor::Blue().Luminance() == 0.0722f);

    // alpha는 휘도에 영향을 주지 않고 HDR 값은 그대로 가중한다.
    static_assert(LinearColor(1.0f, 1.0f, 1.0f, 0.0f).Luminance() == 1.0f);
    static_assert(LinearColor(2.0f, 2.0f, 2.0f).Luminance() == 2.0f);

    // Python: 0.2126*f32(0.2) + 0.7152*f32(0.4) + 0.0722*f32(0.6) = 0.3719200066179037.
    CHECK(Near(LinearColor(0.2f, 0.4f, 0.6f).Luminance(), 0.3719200066179037, 3.0e-8));
    CHECK(PlatformMath::IsNaN(LinearColor(NaN, 0.0f, 0.0f).Luminance()));
}

TEST(ColorEqualsToleranceBoundaries) {
    // 0.5와 0.75의 차이는 정확히 0.25이므로 포함 경계를 float 오차 없이 검사할 수 있다.
    constexpr LinearColor base(0.5f, 0.5f, 0.5f, 0.5f);
    for (uint32 channel = 0; channel < 4; ++channel) {
        const LinearColor other = WithChannel(base, channel, 0.75f);
        CHECK(base.Equals(other, 0.25f));
        CHECK(other.Equals(base, 0.25f));
        CHECK(!base.Equals(other, 0x1.fffffep-3f));
        CHECK(!base.Equals(WithChannel(base, channel, NaN), 1.0f));
    }

    static_assert(base.Equals(base, 0.0f));
    static_assert(
        LinearColor(0.0f, 0.0f, 0.0f, 0.0f).Equals(LinearColor(-0.0f, 0.0f, 0.0f, 0.0f), 0.0f));
    static_assert(!base.Equals(base, -1.0e-30f));
    CHECK(!base.Equals(base, NaN));

    // 같은 무한대 성분끼리는 차이가 NaN이므로 Equals가 false다. 정확 비교는 operator==다.
    const LinearColor infinite(Infinity, 0.0f, 0.0f, 0.0f);
    CHECK(!infinite.Equals(infinite, 1.0f));
    CHECK(infinite == infinite);
}

TEST(ColorIsFiniteAndContainsNaNPerChannel) {
    static_assert(LinearColor::White().IsFinite());
    static_assert(!LinearColor::White().ContainsNaN());
    static_assert(LinearColor(1.0e30f, -1.0e30f, 1.0e-45f, -0.0f).IsFinite());

    const LinearColor base(0.25f, 0.5f, 2.0f, 1.0f);
    for (uint32 channel = 0; channel < 4; ++channel) {
        const LinearColor withNaN = WithChannel(base, channel, NaN);
        CHECK(!withNaN.IsFinite());
        CHECK(withNaN.ContainsNaN());
        CHECK(withNaN != withNaN);

        const LinearColor withInfinity = WithChannel(base, channel, Infinity);
        CHECK(!withInfinity.IsFinite());
        CHECK(!withInfinity.ContainsNaN());

        const LinearColor withNegativeInfinity = WithChannel(base, channel, -Infinity);
        CHECK(!withNegativeInfinity.IsFinite());
        CHECK(!withNegativeInfinity.ContainsNaN());
    }
}

TEST(ColorGetClampedKeepsNaNAndChecksRange) {
    static_assert(LinearColor(-1.0f, 0.5f, 2.0f, 1.5f).GetClamped() ==
                  LinearColor(0.0f, 0.5f, 1.0f, 1.0f));
    static_assert(LinearColor(-1.0f, 0.5f, 20.0f, 1.5f).GetClamped(0.0f, 10.0f) ==
                  LinearColor(0.0f, 0.5f, 10.0f, 1.5f));
    static_assert(LinearColor(-1.0f, 0.5f, 20.0f, 1.5f).GetClamped(0.25f, 0.25f) ==
                  LinearColor(0.25f, 0.25f, 0.25f, 0.25f));

    // NaN은 범위 안의 값으로 바뀌지 않는다. 무한대는 경계로 제한된다.
    const LinearColor base(0.25f, 0.5f, 0.75f, 1.0f);
    for (uint32 channel = 0; channel < 4; ++channel) {
        const LinearColor clamped = WithChannel(base, channel, NaN).GetClamped();
        CHECK(PlatformMath::IsNaN(ChannelOf(clamped, channel)));
        for (uint32 other = 0; other < 4; ++other) {
            if (other != channel) {
                CHECK(ChannelOf(clamped, other) == ChannelOf(base, other));
            }
        }

        CHECK(ChannelOf(WithChannel(base, channel, Infinity).GetClamped(), channel) == 1.0f);
        CHECK(ChannelOf(WithChannel(base, channel, -Infinity).GetClamped(), channel) == 0.0f);
    }
}

TEST(ColorArithmeticKeepsHdrAndAlpha) {
    constexpr LinearColor first(2.0f, -1.0f, 0.5f, 0.25f);
    constexpr LinearColor second(1.0f, 2.0f, 0.5f, 0.5f);
    static_assert(first + second == LinearColor(3.0f, 1.0f, 1.0f, 0.75f));
    static_assert(first - second == LinearColor(1.0f, -3.0f, 0.0f, -0.25f));
    static_assert(first * second == LinearColor(2.0f, -2.0f, 0.25f, 0.125f));
    static_assert(first * 2.0f == LinearColor(4.0f, -2.0f, 1.0f, 0.5f));
    static_assert(2.0f * first == first * 2.0f);
    static_assert(first / 2.0f == LinearColor(1.0f, -0.5f, 0.25f, 0.125f));

    constexpr LinearColor accumulated = [] {
        LinearColor color(1.0f, 2.0f, 3.0f, 4.0f);
        color += LinearColor(1.0f, 1.0f, 1.0f, 1.0f);
        color -= LinearColor(0.5f, 0.5f, 0.5f, 0.5f);
        color *= 2.0f;
        return color;
    }();
    static_assert(accumulated == LinearColor(3.0f, 5.0f, 7.0f, 9.0f));

    // 정확한 나눗셈이다. 0으로 나누면 IEEE 규칙대로 Inf/NaN이 되고 보정하지 않는다.
    volatile float zero = 0.0f;
    const LinearColor divided = LinearColor(1.0f, -1.0f, 0.0f, 3.0f) / zero;
    CHECK(divided.r == Infinity);
    CHECK(divided.g == -Infinity);
    CHECK(PlatformMath::IsNaN(divided.b));
    CHECK(LinearColor(1.0f, 1.0f, 1.0f, 1.0f) / 3.0f ==
          LinearColor(1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f));
}
