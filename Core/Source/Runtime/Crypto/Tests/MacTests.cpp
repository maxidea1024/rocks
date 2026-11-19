#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Mac.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <stdio.h>
#include <string.h>

using namespace rocks;
namespace mac_tests_file {

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

// RFC 4231 사례 2처럼 16 byte보다 짧은 HMAC 키는 hazmat 경로로만 받는다.
MacKey ImportHmac(MacAlgorithm algorithm, Span<const uint8> bytes) {
    if (bytes.Size() >= MinMacKeyBytes) {
        return Import(algorithm, bytes);
    }

    auto key = hazmat::TryImportShortMacKey(algorithm, bytes);
    return key ? Move(key.Value()) : MacKey();
}

// 경계 시험 입력: 키 byte i % 251, 메시지 byte (i * 7 + 3) % 256.
void FillKey(Span<uint8> key) {
    for (usize i = 0; i < key.Size(); ++i) {
        key[i] = static_cast<uint8>(i % 251);
    }
}

void FillMessage(Span<uint8> message) {
    for (usize i = 0; i < message.Size(); ++i) {
        message[i] = static_cast<uint8>(i * 7 + 3);
    }
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

        const MacKey key256 = ImportHmac(MacAlgorithm::HmacSha256, keyBytes);
        const MacKey key512 = ImportHmac(MacAlgorithm::HmacSha512, keyBytes);
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

TEST(CryptoMacBoundaryKnownAnswers) {
    // HMAC 키 길이 경계: 16(하한), 64·65(SHA-256 블록), 128·129(SHA-512 블록), 4096(상한). 메시지는
    // 100 byte다. 값은 Python hmac으로 계산했다.
    struct KeyCase {
        usize keyBytes;
        const char* sha256;
        const char* sha512;
    };

    const KeyCase keyCases[] = {
        {16, "a3ab48ff57f3ec22152892bf6043dd7e30a8babb65e94fe5a545700f3dcf0e11",
         "93df5c9fedd9d805cff93ed4607433c5119736e2a6aa16054c87bc24ad880a2f"
         "33028cbed2b6e59301bd970c84ea89252a3d61288553fded62a6de90b47da7f7"},
        {64, "05889c552cc7accc8012e09bc442fab2e4eea4f1423dda065a88341b92f624da",
         "b6b4b5dc08452e7c7ba5c31c7c55718e3f1f85096f945b86ba95651ea7700b90"
         "49db83b0b5ce4a4f6f44a4a8f82f8d84b0ff70ac83ab6aea0e0a06d6554f4482"},
        {65, "af6249e93181d713c3037483211f698f5a392f015e01a04cbe403cbbdf1552d7",
         "bea71b2b3fac4efbb4122f6b061558fc0404e6780b7ae331026faca29966d4a8"
         "1103f43f3da2587a0bfac896eaac62eba72911eaef29128dde7252a3d6821acb"},
        {128, "6d70b40b9e48bb462693e53fdd716277677a14ca69b9e3837545216e7ba8e5a7",
         "47806324862501c52540df5925db3d6a23d9bad7fd073f0503869a3d5da2a8f2"
         "54595469486ad26ee61e5b65f2571d75d25625e27931c79cc8214eeec70e6fd9"},
        {129, "06239a76432b711fbde4b79699a2f191b99027298561881bb9780247d1ce786a",
         "0af18b86428d1bd72ad3132ec1ffae6113d541c3291141fe27fef84c2690dff9"
         "3685d1e3f5b62943b4f9fae904dd258a0ace40a7eedd02b75a79338cf706e6a3"},
        {4096, "e802d3512c71b45bc6aee8e07684b1264608e7ba5913522b5829caed44811dac",
         "7d4701f7b031bb2a66fde8a783aaab69801305c2e36d2b2c82d11097ed73c094"
         "64370018d7fde5c122a35d2f0ccafd6c78adb4a12d046d46f4dacf44d117baa2"},
    };
    uint8 keyBytes[4096];
    FillKey(Span<uint8>(keyBytes));
    uint8 message[256];
    FillMessage(Span<uint8>(message));
    for (const KeyCase& item : keyCases) {
        const Span<const uint8> key = Span<const uint8>(keyBytes).Left(item.keyBytes);
        const MacKey key256 = Import(MacAlgorithm::HmacSha256, key);
        const MacKey key512 = Import(MacAlgorithm::HmacSha512, key);
        REQUIRE(!key256.IsEmpty() && !key512.IsEmpty());
        uint8 tag256[HmacSha256TagBytes];
        uint8 tag512[HmacSha512TagBytes];
        REQUIRE(TryMac(key256, Span<const uint8>(message).Left(100), Span<uint8>(tag256)));
        REQUIRE(TryMac(key512, Span<const uint8>(message).Left(100), Span<uint8>(tag512)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag256), HexBytes(item.sha256).View()));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag512), HexBytes(item.sha512).View()));
    }

    // BLAKE2b keyed 블록(128 byte) 경계: 128·129·256 byte 메시지, 키는 0..31(Python hashlib).
    uint8 key32[32];
    for (usize i = 0; i < 32; ++i) {
        key32[i] = static_cast<uint8>(i);
    }

    const MacKey blake = Import(MacAlgorithm::Blake2bKeyed, Span<const uint8>(key32));
    REQUIRE(!blake.IsEmpty());
    struct MessageCase {
        usize bytes;
        const char* tag;
    };

    const MessageCase messageCases[] = {
        {128, "45b27f69ea3c381242d369fafdfc8cbd54e710c242bea80d0372c8df23776cda"},
        {129, "b0534a69d4c565fa8c962fb05d0f2ce99f70dddad00a95a220abf0b2ea4f41c8"},
        {256, "80408e16e6976cfa903a875c075c9affc55d71dd0674fd02a279cab70a312a96"},
    };
    for (const MessageCase& item : messageCases) {
        uint8 tag[Blake2bKeyedTagBytes];
        REQUIRE(TryMac(blake, Span<const uint8>(message).Left(item.bytes), Span<uint8>(tag)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag), HexBytes(item.tag).View()));
    }
}

TEST(CryptoMacStateErrorPaths) {
    auto key = MacKey::TryGenerate(MacAlgorithm::Blake2bKeyed);
    REQUIRE(key);
    const Span<const uint8> message = Text("incremental message");
    uint8 expected[Blake2bKeyedTagBytes];
    REQUIRE(TryMac(key.Value(), message, Span<uint8>(expected)));

    // 길이가 틀린 TryFinish는 상태를 바꾸지 않는다. 맞는 버퍼로 다시 부르면 끝난다.
    auto state = MacState::TryCreate(key.Value());
    REQUIRE(state);
    CHECK(state.Value().Algorithm() == MacAlgorithm::Blake2bKeyed);
    CHECK(state.Value().TagBytes() == Blake2bKeyedTagBytes);
    const Span<const uint8> pieces[] = {message.Left(5), message.RightChop(5)};
    REQUIRE(state.Value().TryUpdate(Span<const Span<const uint8>>(pieces)));
    uint8 wrong[Blake2bKeyedTagBytes + 1];
    CHECK(state.Value().TryFinish(Span<uint8>(wrong)).Error() == ErrorCode::InvalidArgument);

    // 이동 대입한 객체가 이어서 맞는 tag를 낸다.
    MacState moved;
    moved = Move(state.Value());
    CHECK(state.Value().TryUpdate(message).Error() == ErrorCode::StaleHandle);
    uint8 tag[Blake2bKeyedTagBytes];
    REQUIRE(moved.TryFinish(Span<uint8>(tag)));
    CHECK(ConstantTimeEquals(Span<const uint8>(tag), Span<const uint8>(expected)));
    CHECK(moved.TryUpdate(message).Error() == ErrorCode::InvalidArgument);

    // 검증 실패는 상태를 끝내고 sticky다. 길이가 틀린 tag도 AuthenticationFailed다.
    auto failing = MacState::TryCreate(key.Value());
    REQUIRE(failing);
    REQUIRE(failing.Value().TryUpdate(message));
    CHECK(failing.Value().TryFinishVerify(Span<const uint8>(expected).Left(16)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(failing.Value().TryUpdate(message).Error() == ErrorCode::AuthenticationFailed);
    CHECK(failing.Value().TryFinishVerify(Span<const uint8>(expected)).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoMacVerifyRejectsTamperingAndWrongLength) {
    // 세 알고리즘 모두 tag bit 하나를 바꾸면 거부한다.
    const MacAlgorithm algorithms[] = {MacAlgorithm::HmacSha256, MacAlgorithm::HmacSha512,
                                       MacAlgorithm::Blake2bKeyed};
    for (MacAlgorithm algorithm : algorithms) {
        auto any = MacKey::TryGenerate(algorithm);
        REQUIRE(any);
        uint8 anyTag[MaxMacTagBytes];
        const Span<uint8> anyView = Span<uint8>(anyTag).Left(TryGetMacTagBytes(algorithm).Value());
        REQUIRE(TryMac(any.Value(), Text("m"), anyView));
        anyTag[anyView.Size() - 1] ^= 0x80;
        CHECK(TryVerifyMac(any.Value(), Text("m"), anyView).Error() ==
              ErrorCode::AuthenticationFailed);
    }

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
    // 키 길이: HMAC 16–4096(hazmat 경로는 1–4096), BLAKE2b keyed 16–64.
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha256, Span<const uint8>()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha256, Span<const uint8>(bytes).Left(15)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MacKey::TryImport(MacAlgorithm::HmacSha256, Span<const uint8>(bytes).Left(16)));
    CHECK(hazmat::TryImportShortMacKey(MacAlgorithm::HmacSha256, Span<const uint8>(bytes).Left(1)));
    CHECK(hazmat::TryImportShortMacKey(MacAlgorithm::HmacSha512, Span<const uint8>()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(
        hazmat::TryImportShortMacKey(MacAlgorithm::Blake2bKeyed, Span<const uint8>(bytes).Left(15))
            .Error() == ErrorCode::InvalidArgument);
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
    REQUIRE(hazmat::TryExportKey(generated.Value(), Span<uint8>(exported)));
    CHECK(!IsAllZero(Span<const uint8>(exported)));
    CHECK(hazmat::TryExportKey(generated.Value(), Span<uint8>(exported).Left(63)).Error() ==
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
    CHECK(hazmat::TryExportKey(empty, Span<uint8>(exported)).Error() == ErrorCode::StaleHandle);
    CHECK(TryMac(copy, Text("x"), Span<uint8>(tagA).Left(32)).Error() ==
          ErrorCode::InvalidArgument);
    Span<const uint8> pieceArray[4] = {};
    CHECK(TryMac(copy, Span<const Span<const uint8>>(pieceArray),
                 AsWritableBytes(Span<Span<const uint8>>(pieceArray)).Left(64))
              .Error() == ErrorCode::InvalidArgument);
    uint8 shared[128] = {};
    CHECK(
        TryMac(copy, Span<const uint8>(shared).Left(70), Span<uint8>(shared).Mid(64, 64)).Error() ==
        ErrorCode::InvalidArgument);
}

// AES-CMAC 알려진 답: RFC 4493 4절 예 1–4(AES-128)와 NIST SP 800-38B 부록 D.3 예 9–12(AES-256)다.
// 메시지는 SP 800-38A 평문의 앞 0·16·40·64 byte다. Python cryptography 50.0.2와 pycryptodome
// 4.4.0이 같은 값을 냄을 확인했다(2026-11-15). 빈·40 byte 메시지는 부분 블록(K2), 16·64 byte는 찬
// 블록(K1)이고, AES-128 L은 맨 앞 bit가 0, K1·AES-256 L은 1이라 doubling의 두 갈래를 모두 지난다.
TEST(CryptoMacAesCmacKnownAnswers) {
    const HexBytes message("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    struct CmacCase {
        const char* key;
        usize length;
        const char* tag;
    };

    const CmacCase cases[] = {
        {"2b7e151628aed2a6abf7158809cf4f3c", 0, "bb1d6929e95937287fa37d129b756746"},
        {"2b7e151628aed2a6abf7158809cf4f3c", 16, "070a16b46b4d4144f79bdd9dd04a287c"},
        {"2b7e151628aed2a6abf7158809cf4f3c", 40, "dfa66747de9ae63030ca32611497c827"},
        {"2b7e151628aed2a6abf7158809cf4f3c", 64, "51f0bebf7e3b9d92fc49741779363cfe"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", 0,
         "028962f61b7bf89efc6b551f4667d983"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", 16,
         "28a7023f452e8f82bd4bf28d8c37c35c"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", 40,
         "aaf3d8f1de5640c232f5b169b9c911e6"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4", 64,
         "e1992190549f6ed5696a2c056c315410"},
    };

    // AES 조건은 Aead.h와 같다(AeadTests가 CPU 기능과의 일치를 단언한다).
    const bool aes = TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available;
    for (const CmacCase& item : cases) {
        const HexBytes keyBytes(item.key);
        const HexBytes expected(item.tag);
        auto key = MacKey::TryImport(MacAlgorithm::AesCmac, keyBytes.View());
        if (!aes) {
            CHECK(key.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(key);
        CHECK(key.Value().KeyBytes() == keyBytes.size);
        const Span<const uint8> input = message.View().Left(item.length);
        uint8 tag[AesCmacTagBytes];
        REQUIRE(TryMac(key.Value(), input, Span<uint8>(tag)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag), expected.View()));
        CHECK(TryVerifyMac(key.Value(), input, expected.View()));

        // 증분: 1 byte씩 넣어도 같다(마지막 블록 보류가 블록 경계마다 바뀐다).
        auto state = MacState::TryCreate(key.Value());
        REQUIRE(state);
        CHECK(state.Value().TagBytes() == AesCmacTagBytes);
        for (usize i = 0; i < input.Size(); ++i) {
            REQUIRE(state.Value().TryUpdate(input.Mid(i, 1)));
        }

        REQUIRE(state.Value().TryFinish(Span<uint8>(tag)));
        CHECK(ConstantTimeEquals(Span<const uint8>(tag), expected.View()));

        // tag의 bit 하나를 바꾸면 AuthenticationFailed다.
        uint8 tampered[AesCmacTagBytes];
        memcpy(tampered, expected.bytes, sizeof(tampered));
        tampered[item.length % AesCmacTagBytes] ^= 0x01;
        CHECK(TryVerifyMac(key.Value(), input, Span<const uint8>(tampered)).Error() ==
              ErrorCode::AuthenticationFailed);
    }
}

TEST(CryptoMacAesCmacContract) {
    uint8 bytes[64] = {};
    CHECK(TryGetMacTagBytes(MacAlgorithm::AesCmac).Value() == AesCmacTagBytes);
    if (!TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available) {
        // 알고리즘 판정이 길이보다 먼저다(AeadKey와 같은 순서).
        CHECK(MacKey::TryGenerate(MacAlgorithm::AesCmac).Error() == ErrorCode::Unsupported);
        CHECK(MacKey::TryImport(MacAlgorithm::AesCmac, Span<const uint8>(bytes).Left(24)).Error() ==
              ErrorCode::Unsupported);
        printf("  CryptoMacAesCmacContract: AES is unavailable on this CPU (Unsupported path)\n");
        return;
    }

    // 키는 16·32 byte만 받는다(AES-192와 다른 길이는 hazmat 짧은 키 경로도 InvalidArgument).
    const usize rejected[] = {0, 1, 15, 17, 24, 31, 33, 64};
    for (usize length : rejected) {
        CHECK(MacKey::TryImport(MacAlgorithm::AesCmac, Span<const uint8>(bytes).Left(length))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(hazmat::TryImportShortMacKey(MacAlgorithm::AesCmac,
                                           Span<const uint8>(bytes).Left(length))
                  .Error() == ErrorCode::InvalidArgument);
    }

    auto generated = MacKey::TryGenerate(MacAlgorithm::AesCmac);
    REQUIRE(generated);
    CHECK(generated.Value().KeyBytes() == 32);

    // 이동한 키는 빈 키다. 증분 상태는 키 스케줄 사본을 두므로 키를 버린 뒤에도 쓴다.
    MacKey key = Move(generated.Value());
    CHECK(generated.Value().IsEmpty());
    uint8 expected[AesCmacTagBytes];
    CHECK(TryMac(generated.Value(), Span<const uint8>(bytes), Span<uint8>(expected)).Error() ==
          ErrorCode::StaleHandle);
    REQUIRE(TryMac(key, Span<const uint8>(bytes).Left(33), Span<uint8>(expected)));
    auto state = MacState::TryCreate(key);
    REQUIRE(state);
    key = MacKey();
    REQUIRE(state.Value().TryUpdate(Span<const uint8>(bytes).Left(17)));
    REQUIRE(state.Value().TryUpdate(Span<const uint8>(bytes).Mid(17, 16)));
    CHECK(state.Value().TryFinishVerify(Span<const uint8>(expected)));
}

// 일회성·조각 입력·증분 상태가 무작위 길이와 조각 나누기에서 같은 tag를 낸다(AES-128·AES-256).
TEST(CryptoMacAesCmacChunking) {
    if (!TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available) {
        printf("  CryptoMacAesCmacChunking: AES is unavailable on this CPU\n");
        return;
    }

    uint8 message[300];
    FillMessage(Span<uint8>(message));
    uint8 keyBytes[32];
    FillKey(Span<uint8>(keyBytes));
    uint64 seed = 0x2545f4914f6cdd1dull ^ test::CurrentSeed();
    auto next = [&seed](usize bound) {
        seed = seed * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<usize>((seed >> 33) % bound);
    };

    for (usize keyLength = 16; keyLength <= 32; keyLength += 16) {
        const MacKey key =
            Import(MacAlgorithm::AesCmac, Span<const uint8>(keyBytes).Left(keyLength));
        REQUIRE(!key.IsEmpty());
        for (usize iteration = 0; iteration < 120; ++iteration) {
            const usize length = iteration < 50 ? iteration : next(sizeof(message) + 1);
            const Span<const uint8> input = Span<const uint8>(message).Left(length);
            uint8 oneShot[AesCmacTagBytes];
            REQUIRE(TryMac(key, input, Span<uint8>(oneShot)));

            // 조각 경계: 무작위이고, 절반은 16의 배수에 둔다(마지막 찬 블록 보류 경로).
            Span<const uint8> pieces[4];
            usize offset = 0;
            auto mac = MacState::TryCreate(key);
            REQUIRE(mac);
            for (usize i = 0; i < 3; ++i) {
                usize piece = next(length - offset + 1);
                if (iteration % 2 == 0) {
                    piece -= piece % 16;
                }

                pieces[i] = input.Mid(offset, piece);
                REQUIRE(mac.Value().TryUpdate(pieces[i]));
                offset += piece;
            }

            pieces[3] = input.RightChop(offset);
            REQUIRE(mac.Value().TryUpdate(pieces[3]));
            uint8 incremental[AesCmacTagBytes];
            REQUIRE(mac.Value().TryFinish(Span<uint8>(incremental)));
            uint8 joined[AesCmacTagBytes];
            REQUIRE(TryMac(key, Span<const Span<const uint8>>(pieces), Span<uint8>(joined)));
            CHECK(ConstantTimeEquals(Span<const uint8>(oneShot), Span<const uint8>(incremental)));
            CHECK(ConstantTimeEquals(Span<const uint8>(oneShot), Span<const uint8>(joined)));
        }
    }
}
} // namespace mac_tests_file
