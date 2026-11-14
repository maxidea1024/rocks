#include <Crypto/Bytes.h>
#include <Crypto/Crypto.h>
#include <Crypto/Random.h>
#include <Platform/Cpu.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

namespace {

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

bool IsFilled(Span<const uint8> bytes, uint8 value) {
    for (uint8 byte : bytes) {
        if (byte != value) {
            return false;
        }
    }

    return true;
}

} // namespace

TEST(CryptoRuntimeUsesRocksRandomAndMatchesCpuFeatures) {
    const CryptoRuntimeInfo& info = GetCryptoRuntimeInfo();
    CHECK(&info == &GetCryptoRuntimeInfo());
    CHECK(info.sodiumVersion == StringView("1.0.22"));
    // libsodium 내부 난수가 Rocks OS 난수로 바뀌었다(sodium_init 전에 교체).
    CHECK(info.randomImplementation == StringView("rocks-os"));

    // libsodium이 감지한 CPU 기능이 PlatformBase와 다르면 빌드 정의가 빠져 하드웨어 경로가 조용히
    // 꺼진 것이다.
    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    if constexpr (IsX64) {
        CHECK(info.sodiumAesni == cpu.aes);
        CHECK(info.sodiumPclmul == cpu.pclmulqdq);
        CHECK(info.sodiumAvx == cpu.avx);
        CHECK(info.sodiumAvx2 == cpu.avx2);
        CHECK(!info.sodiumArmCrypto);
    } else {
        CHECK(info.sodiumArmCrypto == cpu.armAes);
        CHECK(!info.sodiumAesni && !info.sodiumAvx);
    }
}

TEST(CryptoConstantTimeCompareAndZero) {
    const uint8 a[4] = {1, 2, 3, 4};
    const uint8 b[4] = {1, 2, 3, 4};
    const uint8 c[4] = {1, 2, 3, 5};
    CHECK(ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(b)));
    CHECK(!ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(c)));
    CHECK(!ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(b).Left(3)));
    CHECK(ConstantTimeEquals(Span<const uint8>(), Span<const uint8>()));

    uint8 secret[16];
    memset(secret, 0xA5, sizeof(secret));
    CHECK(!IsAllZero(Span<const uint8>(secret)));
    SecureZero(Span<uint8>(secret));
    CHECK(IsAllZero(Span<const uint8>(secret)));
    CHECK(IsAllZero(Span<const uint8>()));
}

TEST(CryptoHexRoundTripAndRejection) {
    const uint8 bytes[3] = {0x00, 0xAB, 0xFF};
    char text[7];
    auto written = TryToHex(Span<const uint8>(bytes), Span<char>(text));
    REQUIRE(written.IsOk());
    CHECK(written.Value() == 6 && strcmp(text, "00abff") == 0);

    // NUL 자리가 없으면 아무것도 쓰지 않는다.
    char small[6] = {'x', 'x', 'x', 'x', 'x', 'x'};
    CHECK(TryToHex(Span<const uint8>(bytes), Span<char>(small)).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(small[0] == 'x');

    uint8 decoded[3] = {};
    auto parsed = TryFromHex(StringView("00ABff"), Span<uint8>(decoded));
    REQUIRE(parsed.IsOk());
    CHECK(parsed.Value() == 3 && memcmp(decoded, bytes, 3) == 0);

    // 쓰기 전 거부(홀수 길이, 출력 부족)는 출력을 건드리지 않는다.
    uint8 output[4] = {9, 9, 9, 9};
    CHECK(TryFromHex(StringView("abc"), Span<uint8>(output)).Error() == ErrorCode::InvalidArgument);
    CHECK(IsFilled(Span<const uint8>(output), 9));
    CHECK(TryFromHex(StringView("0011223344"), Span<uint8>(output)).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(IsFilled(Span<const uint8>(output), 9));
    CHECK(TryFromHex(StringView(""), Span<uint8>()).Value() == 0);

    // 디코딩 중에 찾은 형식 오류는 출력 전체를 지운다.
    memset(output, 0xA5, sizeof(output));
    CHECK(TryFromHex(StringView("zz"), Span<uint8>(output)).Error() == ErrorCode::InvalidArgument);
    CHECK(IsAllZero(Span<const uint8>(output)));
    memset(output, 0xA5, sizeof(output));
    CHECK(TryFromHex(StringView("00 1"), Span<uint8>(output)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(IsAllZero(Span<const uint8>(output)));

    // 정확한 크기: 문자 + NUL이면 되고, 그 뒤는 건드리지 않는다. 하나 모자라면 CapacityExceeded다.
    char exact[8];
    memset(exact, 'x', sizeof(exact));
    REQUIRE(TryToHex(Span<const uint8>(bytes), Span<char>(exact)).IsOk());
    CHECK(strcmp(exact, "00abff") == 0 && exact[7] == 'x');
    CHECK(TryFromHex(StringView("00abff"), Span<uint8>(output).Left(2)).Error() ==
          ErrorCode::CapacityExceeded);

    // 길이 경계: libsodium misuse 경계(bytes >= SIZE_MAX / 2)에 닿기 전에 Overflow로 돌려준다.
    CHECK(TryGetHexChars(MaxSize / 2).Error() == ErrorCode::Overflow);
    CHECK(TryGetHexChars(MaxSize / 2 - 1).Value() == (MaxSize / 2 - 1) * 2);

    // 입력과 출력이 겹치면 쓰기 전에 거부한다.
    char shared[16] = "0011";
    CHECK(TryToHex(AsBytes(Span<const char>(shared, 4)), Span<char>(shared)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryFromHex(StringView(shared, 4), AsWritableBytes(Span<char>(shared))).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(strcmp(shared, "0011") == 0);
}

TEST(CryptoBase64Rfc4648VectorsAndVariants) {
    // RFC 4648 10절 시험 벡터.
    const char* plain[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* padded[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    const char* unpadded[] = {"", "Zg", "Zm8", "Zm9v", "Zm9vYg", "Zm9vYmE", "Zm9vYmFy"};
    for (usize i = 0; i < 7; ++i) {
        char text[16];
        auto written = TryToBase64(Text(plain[i]), Base64Variant::Standard, Span<char>(text));
        REQUIRE(written.IsOk());
        CHECK(strcmp(text, padded[i]) == 0 && written.Value() == strlen(padded[i]));
        CHECK(TryGetBase64Chars(strlen(plain[i]), Base64Variant::StandardNoPadding).Value() ==
              strlen(unpadded[i]));

        auto unpaddedWritten =
            TryToBase64(Text(plain[i]), Base64Variant::UrlSafeNoPadding, Span<char>(text));
        REQUIRE(unpaddedWritten.IsOk());
        CHECK(strcmp(text, unpadded[i]) == 0);

        uint8 decoded[8] = {};
        auto parsed =
            TryFromBase64(StringView(padded[i]), Base64Variant::Standard, Span<uint8>(decoded));
        REQUIRE(parsed.IsOk());
        CHECK(parsed.Value() == strlen(plain[i]) && memcmp(decoded, plain[i], parsed.Value()) == 0);
        auto parsedUnpadded = TryFromBase64(StringView(unpadded[i]),
                                            Base64Variant::UrlSafeNoPadding, Span<uint8>(decoded));
        REQUIRE(parsedUnpadded.IsOk());
        CHECK(parsedUnpadded.Value() == strlen(plain[i]));
    }

    // 알파벳: 0xFB 0xFF는 기본 "+/8=", URL 안전 "-_8="이다. 다른 variant의 문자는 거부한다.
    const uint8 high[2] = {0xFB, 0xFF};
    char text[8];
    REQUIRE(TryToBase64(Span<const uint8>(high), Base64Variant::Standard, Span<char>(text)).IsOk());
    CHECK(strcmp(text, "+/8=") == 0);
    REQUIRE(TryToBase64(Span<const uint8>(high), Base64Variant::UrlSafe, Span<char>(text)).IsOk());
    CHECK(strcmp(text, "-_8=") == 0);

    // 디코딩 중에 찾은 형식 오류(다른 알파벳, 공백, 끝 bit가 0이 아님, 중간 패딩)는 출력 전체를
    // 지운다.
    uint8 decoded[4] = {};
    const char* rejectedWhileDecoding[] = {"-_8=", "Zm9 ", "Zh==", "Zm9=", "Zg=a"};
    for (const char* text : rejectedWhileDecoding) {
        memset(decoded, 0xA5, sizeof(decoded));
        CHECK(TryFromBase64(StringView(text), Base64Variant::Standard, Span<uint8>(decoded))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(IsAllZero(Span<const uint8>(decoded)));
    }

    // 길이와 끝 '='만으로 알 수 있는 형식 오류는 쓰기 전에 거부한다. 출력이 작아도
    // CapacityExceeded로 보이지 않는다.
    struct Rejected {
        const char* text;
        Base64Variant variant;
    };

    const Rejected rejectedBeforeWriting[] = {
        {"Zg", Base64Variant::Standard},
        {"Zg=", Base64Variant::Standard},
        {"====", Base64Variant::Standard},
        {"a===", Base64Variant::UrlSafe},
        {"Zm9v====", Base64Variant::Standard},
        {"Z", Base64Variant::StandardNoPadding},
        {"Zg==", Base64Variant::StandardNoPadding},
        {"Zm8=", Base64Variant::UrlSafeNoPadding},
    };
    for (const Rejected& item : rejectedBeforeWriting) {
        memset(decoded, 0xA5, sizeof(decoded));
        CHECK(TryFromBase64(StringView(item.text), item.variant, Span<uint8>(decoded).Left(1))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(IsFilled(Span<const uint8>(decoded), 0xA5));
    }

    memset(decoded, 0xA5, sizeof(decoded));
    CHECK(TryFromBase64(StringView("Zm9vYmFy"), Base64Variant::Standard, Span<uint8>(decoded))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(IsFilled(Span<const uint8>(decoded), 0xA5));
    CHECK(TryFromBase64(StringView("Zm9v"), Base64Variant::Standard, Span<uint8>(decoded).Left(2))
              .Error() == ErrorCode::CapacityExceeded);
}

TEST(CryptoBase64AllVariantsBoundariesAndMisuse) {
    const uint8 bytes[5] = {0xFB, 0xFF, 0x00, 0x10, 0x83};
    const Base64Variant variants[] = {Base64Variant::Standard, Base64Variant::StandardNoPadding,
                                      Base64Variant::UrlSafe, Base64Variant::UrlSafeNoPadding};
    for (Base64Variant variant : variants) {
        // 정확한 크기(문자 + NUL)로 쓰고 그 뒤는 건드리지 않는다. 하나 모자라면 CapacityExceeded다.
        const usize chars = TryGetBase64Chars(sizeof(bytes), variant).Value();
        char text[16];
        memset(text, 'x', sizeof(text));
        CHECK(
            TryToBase64(Span<const uint8>(bytes), variant, Span<char>(text).Left(chars)).Error() ==
            ErrorCode::CapacityExceeded);
        CHECK(text[0] == 'x');
        auto written = TryToBase64(Span<const uint8>(bytes), variant, Span<char>(text));
        REQUIRE(written.IsOk());
        CHECK(written.Value() == chars && text[chars] == 0 && text[chars + 1] == 'x');

        uint8 decoded[5] = {};
        auto parsed = TryFromBase64(StringView(text, chars), variant, Span<uint8>(decoded));
        REQUIRE(parsed.IsOk());
        CHECK(parsed.Value() == sizeof(bytes) && memcmp(decoded, bytes, sizeof(bytes)) == 0);
    }

    // 열거값이 아닌 variant는 쓰기 전에 거부한다.
    const Base64Variant invalid = static_cast<Base64Variant>(9);
    char text[16] = {};
    uint8 decoded[4] = {};
    CHECK(TryGetBase64Chars(3, invalid).Error() == ErrorCode::InvalidArgument);
    CHECK(TryToBase64(Span<const uint8>(bytes), invalid, Span<char>(text)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryFromBase64(StringView("Zm9v"), invalid, Span<uint8>(decoded)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryGetBase64Chars(MaxSize, Base64Variant::Standard).Error() == ErrorCode::Overflow);

    // 입력과 출력이 겹치면 쓰기 전에 거부한다.
    char shared[16] = "Zm9v";
    CHECK(TryToBase64(AsBytes(Span<const char>(shared, 3)), Base64Variant::Standard,
                      Span<char>(shared))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromBase64(StringView(shared, 4), Base64Variant::Standard,
                        AsWritableBytes(Span<char>(shared)))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(strcmp(shared, "Zm9v") == 0);
}

TEST(CryptoRandomFillAndUniform) {
    CHECK(TryFillRandom(Span<uint8>()).IsOk());

    uint8 first[32] = {};
    uint8 second[32] = {};
    REQUIRE(TryFillRandom(Span<uint8>(first)).IsOk());
    REQUIRE(TryFillRandom(Span<uint8>(second)).IsOk());
    CHECK(!ConstantTimeEquals(Span<const uint8>(first), Span<const uint8>(second)));

    CHECK(TryRandomUniform(0).Error() == ErrorCode::InvalidArgument);
    CHECK(TryRandomUniform(1).Value() == 0);

    // 거부 구간이 가장 큰 경계(2^31 + 1: 절반 가까이 버린다)와 최대값.
    for (int i = 0; i < 64; ++i) {
        CHECK(TryRandomUniform(0x80000001u).Value() < 0x80000001u);
        CHECK(TryRandomUniform(0xFFFFFFFFu).Value() < 0xFFFFFFFFu);
    }

    bool seen[6] = {};
    for (int i = 0; i < 600; ++i) {
        auto value = TryRandomUniform(6);
        REQUIRE(value.IsOk() && value.Value() < 6);
        seen[value.Value()] = true;
    }

    // 600번 던져 한 면이 한 번도 안 나올 확률은 6 * (5/6)^600으로 무시할 수 있다.
    for (bool face : seen) {
        CHECK(face);
    }
}
