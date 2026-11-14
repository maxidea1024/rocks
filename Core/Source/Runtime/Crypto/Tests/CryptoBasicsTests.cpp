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

} // namespace

TEST(CryptoRuntimeUsesRocksRandomAndMatchesCpuFeatures) {
    const CryptoRuntimeInfo& info = GetCryptoRuntimeInfo();
    CHECK(&info == &GetCryptoRuntimeInfo());
    CHECK(info.sodiumVersion == StringView("1.0.22"));
    // libsodium 내부 난수가 Rocks OS 난수로 바뀌었다(sodium_init 전에 교체).
    CHECK(info.randomImplementation == StringView("rocks-os"));

    // libsodium이 감지한 CPU 기능이 PlatformBase와 다르면 빌드 정의가 빠져 하드웨어 경로가 조용히 꺼진 것이다.
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
    CHECK(TryToHex(Span<const uint8>(bytes), Span<char>(small)).Error() == ErrorCode::CapacityExceeded);
    CHECK(small[0] == 'x');

    uint8 decoded[3] = {};
    auto parsed = TryFromHex(StringView("00ABff"), Span<uint8>(decoded));
    REQUIRE(parsed.IsOk());
    CHECK(parsed.Value() == 3 && memcmp(decoded, bytes, 3) == 0);

    uint8 output[4] = {9, 9, 9, 9};
    CHECK(TryFromHex(StringView("abc"), Span<uint8>(output)).Error() == ErrorCode::InvalidArgument);
    CHECK(IsAllZero(Span<const uint8>(output)));   // 실패하면 출력을 지운다
    CHECK(TryFromHex(StringView("zz"), Span<uint8>(output)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromHex(StringView("00 1"), Span<uint8>(output)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromHex(StringView("0011223344"), Span<uint8>(output)).Error() == ErrorCode::CapacityExceeded);
    CHECK(TryFromHex(StringView(""), Span<uint8>()).Value() == 0);
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
        CHECK(TryGetBase64Chars(strlen(plain[i]), Base64Variant::StandardNoPadding).Value() == strlen(unpadded[i]));

        auto unpaddedWritten = TryToBase64(Text(plain[i]), Base64Variant::UrlSafeNoPadding, Span<char>(text));
        REQUIRE(unpaddedWritten.IsOk());
        CHECK(strcmp(text, unpadded[i]) == 0);

        uint8 decoded[8] = {};
        auto parsed = TryFromBase64(StringView(padded[i]), Base64Variant::Standard, Span<uint8>(decoded));
        REQUIRE(parsed.IsOk());
        CHECK(parsed.Value() == strlen(plain[i]) && memcmp(decoded, plain[i], parsed.Value()) == 0);
        auto parsedUnpadded = TryFromBase64(StringView(unpadded[i]), Base64Variant::UrlSafeNoPadding, Span<uint8>(decoded));
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

    uint8 decoded[4] = {7, 7, 7, 7};
    CHECK(TryFromBase64(StringView("-_8="), Base64Variant::Standard, Span<uint8>(decoded)).Error() == ErrorCode::InvalidArgument);
    CHECK(IsAllZero(Span<const uint8>(decoded)));
    CHECK(TryFromBase64(StringView("Zg"), Base64Variant::Standard, Span<uint8>(decoded)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromBase64(StringView("Zm9v Yg=="), Base64Variant::Standard, Span<uint8>(decoded)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromBase64(StringView("Z"), Base64Variant::StandardNoPadding, Span<uint8>(decoded)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryFromBase64(StringView("Zm9vYmFy"), Base64Variant::Standard, Span<uint8>(decoded)).Error() == ErrorCode::CapacityExceeded);
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
