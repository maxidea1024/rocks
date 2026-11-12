#include <Core/Encoding/Sha256.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;

// FIPS 180-4 / NIST CSRC 예제 벡터와 분할 입력 동치성을 확인한다.
namespace {

Span<const uint8> Bytes(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

bool HexEquals(const Sha256Digest& digest, const char* expected) noexcept {
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    return strcmp(hex, expected) == 0;
}

} // namespace

TEST(Sha256_MatchesNistVectors) {
    CHECK(HexEquals(Sha256::Hash(Bytes("")),
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abc")),
                    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")),
                    "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK(HexEquals(Sha256::Hash(Bytes("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmn"
                                       "hijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopqrstnopqrstu")),
                    "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"));
}

TEST(Sha256_MillionAsInChunks) {
    uint8 chunk[1000];
    memset(chunk, 'a', sizeof(chunk));
    Sha256 hasher;
    for (int index = 0; index < 1000; ++index) {
        hasher.Update(Span<const uint8>(chunk, sizeof(chunk)));
    }
    CHECK(HexEquals(hasher.Finish(),
                    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(Sha256_SplitInputMatchesOneShotAndFinishResets) {
    uint8 data[300];
    for (usize index = 0; index < sizeof(data); ++index) {
        data[index] = static_cast<uint8>(index * 31u + 7u);
    }
    // 블록 경계(55·56·63·64·65)를 포함한 모든 길이와 분할 위치를 대조한다.
    for (usize length = 0; length <= sizeof(data); length += 1) {
        const Sha256Digest whole = Sha256::Hash(Span<const uint8>(data, length));
        for (usize split = 0; split <= length; split += length < 130 ? 1 : 37) {
            Sha256 hasher;
            hasher.Update(Span<const uint8>(data, split));
            hasher.Update(Span<const uint8>(data + split, length - split));
            if (!(hasher.Finish() == whole)) {
                CHECK(false);
                return;
            }
        }
    }
    Sha256 reused;
    reused.Update(Bytes("abc"));
    (void)reused.Finish();
    CHECK(HexEquals(reused.Finish(),
                    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
}

TEST(Sha256_HexRoundTripAndRejectsInvalidText) {
    const Sha256Digest digest = Sha256::Hash(Bytes("abc"));
    char hex[Sha256Digest::HexBytes + 1];
    digest.ToHex(hex);
    auto parsed = Sha256Digest::ParseHex(StringView(hex, Sha256Digest::HexBytes));
    REQUIRE(parsed);
    CHECK(parsed.Value() == digest);
    auto upper =
        Sha256Digest::ParseHex("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD");
    REQUIRE(upper);
    CHECK(upper.Value() == digest);
    CHECK(Sha256Digest::ParseHex("abc").Error() == ErrorCode::InvalidArgument);
    CHECK(Sha256Digest::ParseHex("zz7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
              .Error() == ErrorCode::InvalidArgument);
}
