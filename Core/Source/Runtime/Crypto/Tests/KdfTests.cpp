#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/Mac.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace kdf_tests_file {

using namespace rocks::crypto;

// 알려진 답의 출처: HKDF는 RFC 5869 시험 사례 1–3(SHA-256)과 같은 입력의 SHA-512를 Python hmac으로
// 계산한 값(사례 1 SHA-256은 RFC 값과 대조), BLAKE2b KDF는 libsodium 1.0.22-stable
// test/default/kdf.exp(master 0..31, context "KDF test")이고 Python hashlib.blake2b(salt=id,
// person=context)로 독립 재현했다.
namespace {

struct HexBytes {
    uint8 bytes[256];
    usize size = 0;

    explicit HexBytes(const char* hex) {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    Span<const uint8> View() const {
        return Span<const uint8>(bytes, size);
    }
};

bool Equals(Span<const uint8> actual, const char* hex) {
    const HexBytes expected(hex);
    return expected.size != MaxSize && ConstantTimeEquals(actual, expected.View());
}

struct HkdfCase {
    HkdfAlgorithm algorithm;
    const char* ikm;
    const char* salt;
    const char* info;
    const char* prk;
    const char* okm;
};

constexpr const char* Ikm2 = "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f"
                             "202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f"
                             "404142434445464748494a4b4c4d4e4f";
constexpr const char* Salt2 = "606162636465666768696a6b6c6d6e6f707172737475767778797a7b7c7d7e7f"
                              "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f"
                              "a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
constexpr const char* Info2 = "b0b1b2b3b4b5b6b7b8b9babbbcbdbebfc0c1c2c3c4c5c6c7c8c9cacbcccdcecf"
                              "d0d1d2d3d4d5d6d7d8d9dadbdcdddedfe0e1e2e3e4e5e6e7e8e9eaebecedeeef"
                              "f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff";
constexpr const char* Ikm13 = "0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b0b";

const HkdfCase HkdfCases[] = {
    {HkdfAlgorithm::Sha256, Ikm13, "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9",
     "077709362c2e32df0ddc3f0dc47bba6390b6c73bb50f9c3122ec844ad7c2b3e5",
     "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865"},
    {HkdfAlgorithm::Sha256, Ikm2, Salt2, Info2,
     "06a6b88c5853361a06104c9ceb35b45cef760014904671014a193f40c15fc244",
     "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c59045a99cac7827271cb41c65e590"
     "e09da3275600c2f09b8"
     "367793a9aca3db71cc30c58179ec3e87c14c01d5c1f3434f1d87"},
    {HkdfAlgorithm::Sha256, Ikm13, "", "",
     "19ef24a32c717b167f33a91d6f648bdf96596776afdb6377ac434c1c293ccb04",
     "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d9d201395faa4b61a96c8"},
    {HkdfAlgorithm::Sha512, Ikm13, "000102030405060708090a0b0c", "f0f1f2f3f4f5f6f7f8f9",
     "665799823737ded04a88e47e54a5890bb2c3d247c7a4254a8e61350723590a26c36238127d8661b88cf80ef802d57"
     "e2f7cebcf1e00e083"
     "848be19929c61b4237",
     "832390086cda71fb47625bb5ceb168e4c8e26a1a16ed34d9fc7fe92c1481579338da362cb8d9f925d7cb"},
    {HkdfAlgorithm::Sha512, Ikm2, Salt2, Info2,
     "35672542907d4e142c00e84499e74e1de08be86535f924e022804ad775dde27ec86cd1e5b7d178c74489bdbeb3071"
     "2beb82d4f97416c5a"
     "94ea81ebdf3e629e4a",
     "ce6c97192805b346e6161e821ed165673b84f400a2b514b2fe23d84cd189ddf1b695b48cbd1c8388441137b3ce28f"
     "16aa64ba33ba466b24d"
     "f6cfcb021ecff235f6a2056ce3af1de44d572097a8505d9e7a93"},
    {HkdfAlgorithm::Sha512, Ikm13, "", "",
     "fd200c4987ac491313bd4a2a13287121247239e11c9ef82802044b66ef357e5b194498d0682611382348572a7b161"
     "1de54764094286320"
     "578a863f36562b0df6",
     "f5fa02b18298a72a8c23898a8703472c6eb179dc204c03425c970e3b164bf90fff22d04836d0e2343bac"},
};

constexpr KdfContext TestContext("KDF test");

KdfKey TestMaster() {
    uint8 bytes[32];
    for (usize i = 0; i < 32; ++i) {
        bytes[i] = static_cast<uint8>(i);
    }

    auto key = KdfKey::TryImport(Span<const uint8>(bytes));
    return key ? Move(key.Value()) : KdfKey();
}

} // namespace

TEST(CryptoHkdfRfc5869) {
    for (const HkdfCase& item : HkdfCases) {
        const HexBytes ikm(item.ikm);
        const HexBytes salt(item.salt);
        const HexBytes info(item.info);
        const HexBytes okm(item.okm);
        REQUIRE(ikm.size != MaxSize && salt.size != MaxSize && info.size != MaxSize &&
                okm.size != MaxSize);

        // 한 번에, 그리고 extract → expand로 나눠서 같은 결과다.
        uint8 output[128];
        const Span<uint8> view = Span<uint8>(output).Left(okm.size);
        REQUIRE(TryHkdf(item.algorithm, ikm.View(), salt.View(), info.View(), view));
        CHECK(ConstantTimeEquals(view, okm.View()));

        uint8 prk[64];
        const usize prkBytes =
            item.algorithm == HkdfAlgorithm::Sha256 ? HkdfSha256PrkBytes : HkdfSha512PrkBytes;
        REQUIRE(TryHkdfExtract(item.algorithm, salt.View(), ikm.View(),
                               Span<uint8>(prk).Left(prkBytes)));
        CHECK(Equals(Span<const uint8>(prk).Left(prkBytes), item.prk));
        memset(output, 0, sizeof(output));
        REQUIRE(TryHkdfExpand(item.algorithm, Span<const uint8>(prk).Left(prkBytes), info.View(),
                              view));
        CHECK(ConstantTimeEquals(view, okm.View()));
    }
}

TEST(CryptoHkdfRejectsMisuse) {
    const uint8 ikm[32] = {1};
    uint8 prk[64] = {};
    uint8 big[HkdfSha512MaxOutputBytes + 1];

    // 출력 길이 상한(해시 길이 × 255)과 빈 출력.
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), {}, {},
                  Span<uint8>(big).Left(HkdfSha256MaxOutputBytes)));
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), {}, {},
                  Span<uint8>(big).Left(HkdfSha256MaxOutputBytes + 1))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdf(HkdfAlgorithm::Sha512, Span<const uint8>(ikm), {}, {},
                  Span<uint8>(big).Left(HkdfSha512MaxOutputBytes)));
    CHECK(
        TryHkdf(HkdfAlgorithm::Sha512, Span<const uint8>(ikm), {}, {}, Span<uint8>(big)).Error() ==
        ErrorCode::InvalidArgument);
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), {}, {}, Span<uint8>()));

    // PRK 길이, 알고리즘, 겹침.
    CHECK(TryHkdfExtract(HkdfAlgorithm::Sha256, {}, Span<const uint8>(ikm), Span<uint8>(prk))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdfExpand(HkdfAlgorithm::Sha512, Span<const uint8>(prk).Left(32), {},
                        Span<uint8>(big).Left(16))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdf(static_cast<HkdfAlgorithm>(9), Span<const uint8>(ikm), {}, {},
                  Span<uint8>(big).Left(16))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(big).Left(32), {}, {},
                  Span<uint8>(big).Mid(16, 32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdfExpand(HkdfAlgorithm::Sha256, Span<const uint8>(prk).Left(32), {},
                        Span<uint8>(prk).Mid(16, 32))
              .Error() == ErrorCode::InvalidArgument);
}

TEST(CryptoKdfBlake2bKnownAnswers) {
    const KdfKey master = TestMaster();
    REQUIRE(!master.IsEmpty());

    const char* expected64[] = {
        "a0c724404728c8bb95e5433eb6a9716171144d61efb23e74b873fcbeda51d8071b5d70aae12066dfc94ce943f1"
        "45aa176c055040c3dd73b0"
        "a15e36254d450614",
        "02507f144fa9bf19010bf7c70b235b4c2663cc00e074f929602a5e2c10a780757d2a3993d06debc378a90efdac"
        "196dd841817b977d67b786"
        "804f6d3cd585bab5",
        "1944da61ff18dc2028c3578ac85be904931b83860896598f62468f1cb5471c6a344c945dbc62c9aaf70feb6247"
        "2d17775ea5db6ed5494c68"
        "b7a9a59761f39614",
    };
    for (uint64 id = 0; id < 3; ++id) {
        auto derived = TryDeriveSecret(master, id, TestContext, 64);
        REQUIRE(derived);
        CHECK(Equals(derived.Value().View(), expected64[id]));
    }

    // kdf.exp 뒷부분: 길이 n을 id n으로 뽑은 값. 16·64가 경계이고 15·65는 거부한다.
    auto length16 = TryDeriveSecret(master, 16, TestContext, 16);
    auto length32 = TryDeriveSecret(master, 32, TestContext, 32);
    auto length64 = TryDeriveSecret(master, 64, TestContext, 64);
    REQUIRE(length16 && length32 && length64);
    CHECK(Equals(length16.Value().View(), "a529216624ef9161e4cf117272aafff2"));
    CHECK(Equals(length32.Value().View(),
                 "154b291f11196737f8b7f491e4ca11764e0227d34f94295408a869f007aa8618"));
    CHECK(Equals(length64.Value().View(),
                 "06ae14308eeeda62a00cb6d5edf18d1707029515db98f472bbf0617419301b1d"
                 "4f4f2ab65849446be46f87e1d31c6c74283897b9976f70d8a16253ac927e0d9f"));
    CHECK(TryDeriveSecret(master, 15, TestContext, 15).Error() == ErrorCode::InvalidArgument);
    CHECK(TryDeriveSecret(master, 65, TestContext, 65).Error() == ErrorCode::InvalidArgument);
}

TEST(CryptoHkdfBoundaries) {
    const uint8 ikm[32] = {7};
    uint8 buffer[256] = {};
    uint8 prk[64] = {};

    // 출력이 salt·info와 겹치면 쓰기 전에 거부한다. extract도 같다.
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), Span<const uint8>(buffer).Left(16),
                  {}, Span<uint8>(buffer).Mid(8, 32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), {},
                  Span<const uint8>(buffer).Mid(100, 16), Span<uint8>(buffer).Mid(90, 32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdfExtract(HkdfAlgorithm::Sha256, Span<const uint8>(prk).Left(16),
                         Span<const uint8>(ikm), Span<uint8>(prk).Left(32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdfExtract(static_cast<HkdfAlgorithm>(9), {}, Span<const uint8>(ikm),
                         Span<uint8>(prk).Left(32))
              .Error() == ErrorCode::InvalidArgument);

    // expand 상한 + 1과 빈 출력.
    REQUIRE(TryHkdfExtract(HkdfAlgorithm::Sha256, {}, Span<const uint8>(ikm),
                           Span<uint8>(prk).Left(32)));
    uint8 large[HkdfSha256MaxOutputBytes + 1];
    CHECK(TryHkdfExpand(HkdfAlgorithm::Sha256, Span<const uint8>(prk).Left(32), {},
                        Span<uint8>(large))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryHkdfExpand(HkdfAlgorithm::Sha256, Span<const uint8>(prk).Left(32), {}, Span<uint8>()));

    // RFC 5869: 비어 있는 salt는 해시 길이의 0 byte salt와 같다.
    const uint8 zeros[64] = {};
    uint8 implicitSalt[42];
    uint8 explicitSalt[42];
    REQUIRE(
        TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm), {}, {}, Span<uint8>(implicitSalt)));
    REQUIRE(TryHkdf(HkdfAlgorithm::Sha256, Span<const uint8>(ikm),
                    Span<const uint8>(zeros).Left(32), {}, Span<uint8>(explicitSalt)));
    CHECK(ConstantTimeEquals(Span<const uint8>(implicitSalt), Span<const uint8>(explicitSalt)));
    REQUIRE(
        TryHkdf(HkdfAlgorithm::Sha512, Span<const uint8>(ikm), {}, {}, Span<uint8>(implicitSalt)));
    REQUIRE(TryHkdf(HkdfAlgorithm::Sha512, Span<const uint8>(ikm), Span<const uint8>(zeros), {},
                    Span<uint8>(explicitSalt)));
    CHECK(ConstantTimeEquals(Span<const uint8>(implicitSalt), Span<const uint8>(explicitSalt)));
}

TEST(CryptoKdfTypedDerivation) {
    const KdfKey master = TestMaster();

    // 타입 파생은 BLAKE2b(key = master, salt = LE64(id) ‖ 0⁸, personal = context ‖ 타입 tag)다.
    // 값은 Python hashlib.blake2b로 계산했다. 이 byte가 바뀌면 저장된 키 계층이 달라지므로 형식으로
    // 고정한다.
    struct Typed {
        MacAlgorithm algorithm;
        usize bytes;
        const char* expected;
    };

    const Typed macCases[] = {
        {MacAlgorithm::HmacSha256, 32,
         "0ab2f8c82c5b1705bdfe63c20c193cc9638e08fabb9951723e7a566901cfe3f3"},
        {MacAlgorithm::HmacSha512, 64,
         "0d5a063eb2b0311cadaf2500ea99bcc67ebbd0f87497c39698911523d8b39b5c"
         "2b243ef8d41e5adcfed33fe6eb0b084efdd5cfa83aaf1907c6c41c6a3336ec5e"},
        {MacAlgorithm::Blake2bKeyed, 32,
         "4d478a80f0e8b3a8504cb81756039b03c4045695ab184871528c393902b78674"},
        {MacAlgorithm::AesCmac, 32,
         "a7ef66aabf21a160bd59bf2430288746248b5eb1ee20bd392874348700b8a094"},
    };
    // AES-CMAC은 AES HW가 있을 때만 파생된다(없으면 파생 전에 Unsupported).
    const bool aes = TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available;
    uint8 exported[64];
    for (const Typed& item : macCases) {
        auto macKey = TryDeriveMacKey(master, 7, TestContext, item.algorithm);
        if (item.algorithm == MacAlgorithm::AesCmac && !aes) {
            CHECK(macKey.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(macKey);
        CHECK(macKey.Value().Algorithm() == item.algorithm);
        REQUIRE(macKey.Value().KeyBytes() == item.bytes);
        REQUIRE(hazmat::TryExportKey(macKey.Value(), Span<uint8>(exported).Left(item.bytes)));
        CHECK(Equals(Span<const uint8>(exported).Left(item.bytes), item.expected));
    }

    auto child = TryDeriveKdfKey(master, 7, TestContext);
    REQUIRE(child);
    REQUIRE(hazmat::TryExportKey(child.Value(), Span<uint8>(exported).Left(KdfKeyBytes)));
    CHECK(Equals(Span<const uint8>(exported).Left(KdfKeyBytes),
                 "43cbcbfa680c9d93c5a1dc84246d54c0258f437e5d277836c67f50693bd29950"));

    // 같은 (id, context)의 TryDeriveSecret(crypto_kdf 호환)은 타입 파생 어느 것과도 다르다.
    auto secret = TryDeriveSecret(master, 7, TestContext, 32);
    REQUIRE(secret);
    CHECK(Equals(secret.Value().View(),
                 "cd49ce518f09d9c0c8b41cc9e199da9b93447895af2405b7a003226c848991ce"));
    CHECK(!ConstantTimeEquals(secret.Value().View(), Span<const uint8>(exported).Left(32)));

    // id·context가 다르면 다른 키다.
    constexpr KdfContext Other("Other ct");
    auto other = TryDeriveSecret(master, 7, Other, 32);
    auto otherId = TryDeriveSecret(master, 8, TestContext, 32);
    REQUIRE(other && otherId);
    CHECK(!ConstantTimeEquals(other.Value().View(), secret.Value().View()));
    CHECK(!ConstantTimeEquals(otherId.Value().View(), secret.Value().View()));
    CHECK(!(Other == TestContext));
    CHECK(TestContext == KdfContext("KDF test"));

    // 키 계층: 하위 마스터에서 다시 뽑아도 결정적이다.
    auto again = TryDeriveKdfKey(master, 7, TestContext);
    REQUIRE(again);
    auto leafA = TryDeriveSecret(child.Value(), 2, TestContext, 32);
    auto leafB = TryDeriveSecret(again.Value(), 2, TestContext, 32);
    REQUIRE(leafA && leafB);
    CHECK(ConstantTimeEquals(leafA.Value().View(), leafB.Value().View()));

    // 키 계약: 32 byte만 받고, 빈 키는 StaleHandle, 알고리즘 검사는 파생보다 먼저다.
    uint8 bytes[33] = {};
    CHECK(KdfKey::TryImport(Span<const uint8>(bytes)).Error() == ErrorCode::InvalidArgument);
    CHECK(KdfKey::TryImport(Span<const uint8>(bytes).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
    auto generated = KdfKey::TryGenerate();
    REQUIRE(generated);
    REQUIRE(hazmat::TryExportKey(generated.Value(), Span<uint8>(bytes).Left(32)));
    CHECK(!IsAllZero(Span<const uint8>(bytes).Left(32)));
    CHECK(hazmat::TryExportKey(generated.Value(), Span<uint8>(bytes)).Error() ==
          ErrorCode::InvalidArgument);
    const KdfKey empty;
    CHECK(TryDeriveSecret(empty, 0, TestContext, 32).Error() == ErrorCode::StaleHandle);
    CHECK(TryDeriveMacKey(empty, 0, TestContext, MacAlgorithm::HmacSha256).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryDeriveKdfKey(empty, 0, TestContext).Error() == ErrorCode::StaleHandle);
    CHECK(hazmat::TryExportKey(empty, Span<uint8>(bytes).Left(32)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryDeriveMacKey(empty, 0, TestContext, static_cast<MacAlgorithm>(9)).Error() ==
          ErrorCode::InvalidArgument);
}
} // namespace kdf_tests_file
