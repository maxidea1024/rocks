#include <TestSupport/Test.h>
#include <Core/Encoding/Base64.h>
#include <Core/Encoding/Crc32.h>

namespace rocks::test {
namespace {

Span<const uint8> Bytes(const char* text) {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), length);
}

bool SameText(const char* actual, usize actualLength, const char* expected) {
    usize length = 0;
    while (expected[length] != '\0') {
        ++length;
    }
    if (length != actualLength) {
        return false;
    }
    for (usize index = 0; index < length; ++index) {
        if (actual[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST(Crc32_StandardCheckValues) {
    // CRC-32/ISO-HDLC 표준 check 값과 널리 쓰이는 예문 값이다.
    CHECK(Crc32(Bytes("123456789")) == 0xcbf43926u);
    CHECK(Crc32(Bytes("The quick brown fox jumps over the lazy dog")) == 0x414fa339u);
    CHECK(Crc32(Bytes("")) == 0u);
    CHECK(Crc32(Bytes(""), 0x12345678u) == 0x12345678u);
    // 나눠 계산해도 한 번에 계산한 값과 같다.
    const Span<const uint8> all = Bytes("123456789");
    const uint32 first = Crc32(Span<const uint8>(all.Data(), 4));
    CHECK(Crc32(Span<const uint8>(all.Data() + 4, 5), first) == 0xcbf43926u);
}

TEST(Crc32_LookupTableMatchesPolynomial) {
    // 헤더의 상수 표를 다항식 0xEDB88320의 bit 단위 계산으로 독립 생성한 값과 전부 대조한다.
    int mismatches = 0;
    for (uint32 index = 0; index < 256; ++index) {
        uint32 value = index;
        for (int bit = 0; bit < 8; ++bit) {
            value = (value & 1u) != 0 ? (value >> 1) ^ 0xedb88320u : value >> 1;
        }
        if (detail::Crc32Table[index] != value) {
            ++mismatches;
        }
    }
    CHECK(mismatches == 0);
    static_assert(detail::Crc32Table[1] == 0x77073096u && detail::Crc32Table[255] == 0x2d02ef8du);
}

TEST(Base64_Rfc4648Vectors) {
    const char* inputs[] = {"", "f", "fo", "foo", "foob", "fooba", "foobar"};
    const char* expected[] = {"", "Zg==", "Zm8=", "Zm9v", "Zm9vYg==", "Zm9vYmE=", "Zm9vYmFy"};
    for (int index = 0; index < 7; ++index) {
        char text[16] = {};
        auto encoded = TryBase64Encode(Bytes(inputs[index]), Span<char>(text));
        REQUIRE(encoded);
        CHECK(SameText(text, encoded.Value(), expected[index]));
        CHECK(Base64EncodedSize(Bytes(inputs[index]).Size()).Value() == encoded.Value());
        uint8 decoded[16] = {};
        auto count = TryBase64Decode(StringView(text, encoded.Value()), Span<uint8>(decoded));
        REQUIRE(count);
        CHECK(SameText(reinterpret_cast<const char*>(decoded), count.Value(), inputs[index]));
    }
}

TEST(Base64_AllByteValuesRoundTrip) {
    uint8 bytes[256];
    for (int value = 0; value < 256; ++value) {
        bytes[value] = static_cast<uint8>(value);
    }
    for (usize length = 0; length <= 256; ++length) {
        char text[344];
        uint8 decoded[256];
        auto encoded = TryBase64Encode(Span<const uint8>(bytes, length), Span<char>(text));
        REQUIRE(encoded);
        auto count = TryBase64Decode(StringView(text, encoded.Value()), Span<uint8>(decoded));
        REQUIRE(count && count.Value() == length);
        bool same = true;
        for (usize index = 0; index < length; ++index) {
            same = same && decoded[index] == bytes[index];
        }
        CHECK(same);
    }
}

TEST(Base64_StrictDecodingRejectsWithoutWriting) {
    const char* invalid[] = {
        "Zg=",      // 길이가 4의 배수가 아님
        "Zg",       // 패딩 생략
        "Z===",     // 패딩 3개
        "====",     // 패딩만
        "Zg==Zg==", // 중간 패딩
        "Zm9v\n",   // 줄바꿈
        "Zm 9",     // 공백
        "Zm-_",     // URL-safe 알파벳
        "Zh==",     // 사용하지 않는 bit가 0이 아님(비정규)
        "Zm9=",     // 사용하지 않는 bit가 0이 아님(비정규)
    };
    for (const char* text : invalid) {
        uint8 output[8] = {0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa};
        auto decoded = TryBase64Decode(StringView(text), Span<uint8>(output));
        CHECK(!decoded && decoded.Error() == ErrorCode::InvalidArgument);
        bool untouched = true;
        for (uint8 value : output) {
            untouched = untouched && value == 0xaa;
        }
        CHECK(untouched);
    }
    const char embedded[] = {'Z', 'g', '\0', '='};
    uint8 output[4] = {};
    CHECK(!TryBase64Decode(StringView(embedded, 4), Span<uint8>(output)));
}

TEST(Base64_CapacityIsCheckedBeforeWriting) {
    char small[3] = {'x', 'x', 'x'};
    auto encoded = TryBase64Encode(Bytes("f"), Span<char>(small));
    CHECK(!encoded && encoded.Error() == ErrorCode::CapacityExceeded);
    CHECK(small[0] == 'x' && small[2] == 'x');
    uint8 tiny[2] = {7, 7};
    auto decoded = TryBase64Decode(StringView("Zm9v"), Span<uint8>(tiny));
    CHECK(!decoded && decoded.Error() == ErrorCode::CapacityExceeded);
    CHECK(tiny[0] == 7 && tiny[1] == 7);
    CHECK(Base64EncodedSize(MaxSize).Error() == ErrorCode::Overflow);
}

} // namespace rocks::test
