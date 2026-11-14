#include <Crypto/Bytes.h>
#include <Crypto/Mac.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: HMAC은 RFC 4231 시험 사례 1·2·3·4·6·7(5는 잘린 출력이라 뺀다)이고, BLAKE2b
// keyed는 Python hashlib.blake2b(key=..., digest_size=32)로 계산한 값이다(libsodium과 독립된 구현).
namespace {

struct HexBytes {
    uint8 bytes[512];
    usize size = 0;

    explicit HexBytes(const char* hex) {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    Span<const uint8> View() const {
        return Span<const uint8>(bytes, size);
    }
};

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

// key가 nullptr이면 사례 6·7의 131 byte 0xaa 키다(블록보다 긴 키).
struct HmacCase {
    const char* key;
    const char* data;
    const char* sha256;
    const char* sha512;
};

const HmacCase HmacCases[] = {
    {"0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b", "4869205468657265",
     "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
     "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
     "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854"},
    {"4a656665", "7768617420646f2079612077616e7420666f72206e6f7468696e673f",
     "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
     "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
     "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737"},
    {"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
     "ddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"
     "ddddddd",
     "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
     "fa73b0089d56a284efb0f0756c890be9b1b5dbdd8ee81a3655f83e33b2279d39"
     "bf3e848279a722c806b485a47e67c807b946a337bee8942674278859e13292fb"},
    {"0102030405060708090a0b0c0d0e0f10111213141516171819",
     "cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdc"
     "dcdcdcd",
     "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
     "b0ba465637458c6990e5a8c5f61d4af7e576d97ff94b872de76f8050361ee3db"
     "a91ca5c11aa25eb4d679275cc5788063a5f19741120c4f2de2adebeb10a298dd"},
    {nullptr,
     "54657374205573696e67204c6172676572205468616e20426c6f636b2d53697a65204b6579202d2048617368204b6"
     "579204669727374",
     "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
     "80b24263c7c1a3ebb71493c1dd7be8b49b46d1f41b4aeec1121b013783f8f352"
     "6b56d037e05f2598bd0fd2215d6a1e5295e64f73f63f0aec8b915a985d786598"},
    {nullptr,
     "5468697320697320612074657374207573696e672061206c6172676572207468616e20626c6f636b2d73697a65206"
     "b657920616e64"
     "2061206c6172676572207468616e20626c6f636b2d73697a6520646174612e20546865206b6579206e65656473207"
     "46f2062652068"
     "6173686564206265666f7265206265696e6720757365642062792074686520484d414320616c676f726974686d2e",
     "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
     "e37b6a775dc87dbaa4dfa9f96e5e3ffddebd71f8867289865df5a32d20cdc944"
     "b6022cac3c4982b10d5eeb55c3e4de15134676fb6de0446065c97440fa8c6a58"},
};

MacKey Import(MacAlgorithm algorithm, Span<const uint8> bytes) {
    auto key = MacKey::TryImport(algorithm, bytes);
    return key ? Move(key.Value()) : MacKey();
}

} // namespace

TEST(CryptoMacHmacRfc4231) {
    for (const HmacCase& item : HmacCases) {
        uint8 longKey[131];
        for (uint8& byte : longKey) {
            byte = 0xAA;
        }

        const HexBytes hexKey(item.key != nullptr ? item.key : "");
        const Span<const uint8> keyBytes =
            item.key != nullptr ? hexKey.View() : Span<const uint8>(longKey);
        const HexBytes data(item.data);
        const HexBytes sha256(item.sha256);
        const HexBytes sha512(item.sha512);
        REQUIRE(hexKey.size != MaxSize && data.size != MaxSize && sha256.size == 32 &&
                sha512.size == 64);

        const MacKey key256 = Import(MacAlgorithm::HmacSha256, keyBytes);
        const MacKey key512 = Import(MacAlgorithm::HmacSha512, keyBytes);
        REQUIRE(!key256.IsEmpty() && !key512.IsEmpty());

        uint8 tag256[HmacSha256TagBytes];
        uint8 tag512[HmacSha512TagBytes];
        REQUIRE(TryMac(key256, data.View(), Span<uint8>(tag256)));
        REQUIRE(TryMac(key512, data.View(), Span<uint8>(tag512)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag256), sha256.View()));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag512), sha512.View()));
        CHECK(TryVerifyMac(key256, data.View(), sha256.View()));
        CHECK(TryVerifyMac(key512, data.View(), sha512.View()));
    }
}

TEST(CryptoMacBlake2bKeyedKnownAnswers) {
    uint8 key32[32];
    uint8 key16[16];
    uint8 key64[64];
    for (usize i = 0; i < 64; ++i) {
        key64[i] = static_cast<uint8>(i);
        if (i < 32) {
            key32[i] = static_cast<uint8>(i);
        }

        if (i < 16) {
            key16[i] = static_cast<uint8>(i);
        }
    }

    struct Case {
        Span<const uint8> key;
        const char* message;
        const char* tag;
    };

    const Case cases[] = {
        {Span<const uint8>(key32), "",
         "4e51e7a913fc80137da52880fecca175bf81e117d5c68126dc2774033517ea0d"},
        {Span<const uint8>(key32), "abc",
         "d63a32d3e44738d7907f964316c241adaba0abfeabc32349677578a15a203f7f"},
        {Span<const uint8>(key32), "The quick brown fox jumps over the lazy dog",
         "5d9461aff732d77d0cc98725ea29298c914fd5193b4c08ec9e3ad6b28c3e2faf"},
        {Span<const uint8>(key16), "abc",
         "3fd8fd31501cdbe942607b204368e3dc4ebce5744de1a955a6025f5c9848bb20"},
        {Span<const uint8>(key64), "abc",
         "dff38c978666dff5631db35ca15535520d134f5c8060ea569c6a178ad393719f"},
    };
    for (const Case& item : cases) {
        const MacKey key = Import(MacAlgorithm::Blake2bKeyed, item.key);
        REQUIRE(!key.IsEmpty());
        uint8 tag[Blake2bKeyedTagBytes];
        REQUIRE(TryMac(key, Text(item.message), Span<uint8>(tag)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag), HexBytes(item.tag).View()));
    }
}

TEST(CryptoMacVerifyRejectsTamperingAndWrongLength) {
    auto key = MacKey::TryGenerate(MacAlgorithm::HmacSha256);
    REQUIRE(key);
    const Span<const uint8> message = Text("authenticated message");
    uint8 tag[HmacSha256TagBytes];
    REQUIRE(TryMac(key.Value(), message, Span<uint8>(tag)));
    REQUIRE(TryVerifyMac(key.Value(), message, Span<const uint8>(tag)));

    // tag·메시지의 bit 하나, 잘린 tag, 긴 tag, 다른 키는 모두 AuthenticationFailed다.
    for (usize bit = 0; bit < 8 * sizeof(tag); bit += 37) {
        uint8 tampered[HmacSha256TagBytes];
        memcpy(tampered, tag, sizeof(tag));
        tampered[bit / 8] ^= static_cast<uint8>(1u << (bit % 8));
        CHECK(TryVerifyMac(key.Value(), message, Span<const uint8>(tampered)).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    CHECK(
        TryVerifyMac(key.Value(), Text("authenticated messagE"), Span<const uint8>(tag)).Error() ==
        ErrorCode::AuthenticationFailed);
    CHECK(TryVerifyMac(key.Value(), message, Span<const uint8>(tag).Left(31)).Error() ==
          ErrorCode::AuthenticationFailed);
    uint8 longer[HmacSha256TagBytes + 1] = {};
    memcpy(longer, tag, sizeof(tag));
    CHECK(TryVerifyMac(key.Value(), message, Span<const uint8>(longer)).Error() ==
          ErrorCode::AuthenticationFailed);
    auto other = MacKey::TryGenerate(MacAlgorithm::HmacSha256);
    REQUIRE(other);
    CHECK(TryVerifyMac(other.Value(), message, Span<const uint8>(tag)).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoMacIncrementalAndPiecesMatchOneShot) {
    uint8 message[777];
    for (usize i = 0; i < sizeof(message); ++i) {
        message[i] = static_cast<uint8>(i * 7 + 3);
    }

    const Span<const uint8> all(message);
    const MacAlgorithm algorithms[] = {MacAlgorithm::HmacSha256, MacAlgorithm::HmacSha512,
                                       MacAlgorithm::Blake2bKeyed};
    for (MacAlgorithm algorithm : algorithms) {
        auto key = MacKey::TryGenerate(algorithm);
        REQUIRE(key);
        const usize bytes = TryGetMacTagBytes(algorithm).Value();
        uint8 expected[64];
        uint8 actual[64];
        REQUIRE(TryMac(key.Value(), all, Span<uint8>(expected, bytes)));

        const Span<const uint8> pieces[] = {all.Left(5), Span<const uint8>(), all.Mid(5, 128),
                                            all.RightChop(133)};
        REQUIRE(
            TryMac(key.Value(), Span<const Span<const uint8>>(pieces), Span<uint8>(actual, bytes)));
        CHECK(ConstantTimeEquals(Span<const uint8>(expected, bytes),
                                 Span<const uint8>(actual, bytes)));
        CHECK(TryVerifyMac(key.Value(), Span<const Span<const uint8>>(pieces),
                           Span<const uint8>(expected, bytes)));

        // 증분 상태는 키 저장소 슬롯을 쓰고, 끝나면 돌려준다.
        const usize used = GetSecretMemoryStatus().arenaUsedBytes;
        {
            auto state = MacState::TryCreate(key.Value());
            REQUIRE(state);
            CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
            for (usize offset = 0; offset < all.Size(); offset += 100) {
                REQUIRE(state.Value().TryUpdate(all.Mid(offset, 100)));
            }

            REQUIRE(state.Value().TryFinish(Span<uint8>(actual, bytes)));
            CHECK(ConstantTimeEquals(Span<const uint8>(expected, bytes),
                                     Span<const uint8>(actual, bytes)));
            CHECK(state.Value().TryUpdate(all).Error() == ErrorCode::InvalidArgument);
            CHECK(state.Value().TryFinish(Span<uint8>(actual, bytes)).Error() ==
                  ErrorCode::InvalidArgument);

            auto verify = MacState::TryCreate(key.Value());
            REQUIRE(verify);
            REQUIRE(verify.Value().TryUpdate(all));
            CHECK(verify.Value().TryFinishVerify(Span<const uint8>(expected, bytes)));
            CHECK(verify.Value().TryFinishVerify(Span<const uint8>(expected, bytes)).Error() ==
                  ErrorCode::InvalidArgument);

            auto wrong = MacState::TryCreate(key.Value());
            REQUIRE(wrong);
            REQUIRE(wrong.Value().TryUpdate(all.Left(10)));
            CHECK(wrong.Value().TryFinishVerify(Span<const uint8>(expected, bytes)).Error() ==
                  ErrorCode::AuthenticationFailed);

            MacState moved(Move(state.Value()));
            CHECK(state.Value().TryUpdate(all).Error() == ErrorCode::StaleHandle);
        }

        CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
    }
}

TEST(CryptoMacKeyContract) {
    uint8 bytes[4097] = {};
    // 키 길이: HMAC 1–4096, BLAKE2b keyed 16–64.
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha256, Span<const uint8>()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha256, Span<const uint8>(bytes)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha512, Span<const uint8>(bytes).Left(4096)));
    CHECK(
        MacKey::TryImport(MacAlgorithm::Blake2bKeyed, Span<const uint8>(bytes).Left(15)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(
        MacKey::TryImport(MacAlgorithm::Blake2bKeyed, Span<const uint8>(bytes).Left(65)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(MacKey::TryImport(static_cast<MacAlgorithm>(9), Span<const uint8>(bytes).Left(32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(MacKey::TryGenerate(static_cast<MacAlgorithm>(0)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetMacTagBytes(MacAlgorithm::HmacSha512).Value() == 64);
    CHECK(TryGetMacTagBytes(static_cast<MacAlgorithm>(9)).Error() == ErrorCode::InvalidArgument);

    // 생성한 키 길이와 내보내기 왕복.
    auto generated = MacKey::TryGenerate(MacAlgorithm::HmacSha512);
    REQUIRE(generated);
    CHECK(generated.Value().Algorithm() == MacAlgorithm::HmacSha512);
    uint8 exported[64];
    REQUIRE(generated.Value().TryExport(Span<uint8>(exported)));
    CHECK(!IsAllZero(Span<const uint8>(exported)));
    CHECK(generated.Value().TryExport(Span<uint8>(exported).Left(63)).Error() ==
          ErrorCode::InvalidArgument);
    const MacKey copy = Import(MacAlgorithm::HmacSha512, Span<const uint8>(exported));
    uint8 tagA[64];
    uint8 tagB[64];
    REQUIRE(TryMac(generated.Value(), Text("x"), Span<uint8>(tagA)));
    REQUIRE(TryMac(copy, Text("x"), Span<uint8>(tagB)));
    CHECK(ConstantTimeEquals(Span<const uint8>(tagA), Span<const uint8>(tagB)));

    // 빈 키는 StaleHandle, tag 길이 오류·입력과 겹친 출력은 InvalidArgument다.
    const MacKey empty;
    CHECK(TryMac(empty, Text("x"), Span<uint8>(tagA)).Error() == ErrorCode::StaleHandle);
    CHECK(TryVerifyMac(empty, Text("x"), Span<const uint8>(tagA)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(MacState::TryCreate(empty).Error() == ErrorCode::StaleHandle);
    CHECK(empty.TryExport(Span<uint8>(exported)).Error() == ErrorCode::StaleHandle);
    CHECK(TryMac(copy, Text("x"), Span<uint8>(tagA).Left(32)).Error() ==
          ErrorCode::InvalidArgument);
    uint8 shared[128] = {};
    CHECK(
        TryMac(copy, Span<const uint8>(shared).Left(70), Span<uint8>(shared).Mid(64, 64)).Error() ==
        ErrorCode::InvalidArgument);
}
