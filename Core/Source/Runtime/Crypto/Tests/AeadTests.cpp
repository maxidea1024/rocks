#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: ChaCha20-Poly1305는 RFC 8439 2.8.2(tag를 RFC 값과 대조), XChaCha20-Poly1305는
// draft-irtf-cfrg-xchacha-03 A.3.1 입력을 pycryptodome으로, AES-256-GCM은 같은 키·AD·평문에 nonce
// 00..0b를 cryptography(OpenSSL)로 계산한 값이다. AEGIS-256·128L은 libsodium 1.0.22-stable
// test/default/aead_aegis256.c·aead_aegis128l.c 표(AEGIS draft 시험 벡터)에서 짧은 항목을 골랐다.
// AEAD 키 타입 파생은 Python hashlib.blake2b로 계산했다.
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

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

bool AllZero(Span<const uint8> bytes) {
    for (uint8 value : bytes) {
        if (value != 0) {
            return false;
        }
    }

    return true;
}

bool IsAvailable(AeadAlgorithm algorithm) {
    auto info = TryGetAeadInfo(algorithm);
    return info && info.Value().available;
}

// RFC 8439 2.8.2 평문(114 byte)이다.
constexpr const char* Sunscreen =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
    "sunscreen would be it.";

// RFC 8439 2.8.2 키(80..9f)와 AD다.
constexpr const char* RfcKey = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
constexpr const char* RfcAd = "50515253c0c1c2c3c4c5c6c7";

struct AeadCase {
    AeadAlgorithm algorithm;
    const char* key;
    const char* nonce;
    const char* associatedData;
    // text가 nullptr이 아니면 평문은 그 문자열이고, 아니면 messageHex다.
    const char* text;
    const char* messageHex;
    const char* ciphertext;
    const char* tag;
};

const AeadCase AeadCases[] = {
    {AeadAlgorithm::ChaCha20Poly1305Ietf, RfcKey, "070000004041424344454647", RfcAd, Sunscreen,
     nullptr,
     "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92"
     "728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b"
     "4831d7bc3ff4def08e4b7a9de576d26586cec64b6116",
     "1ae10b594f09e26a7e902ecbd0600691"},
    {AeadAlgorithm::XChaCha20Poly1305, RfcKey, "404142434445464748494a4b4c4d4e4f5051525354555657",
     RfcAd, Sunscreen, nullptr,
     "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda"
     "7e39ae64c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b23835"
     "65d3fff921f9664c97637da9768812f615c68b13b52e",
     "c0875924c1c7987947deafd8780acf49"},
    {AeadAlgorithm::Aes256Gcm, RfcKey, "000102030405060708090a0b", RfcAd, Sunscreen, nullptr,
     "bf211b4070555acaa810792b4d3889cd91eac3a754c8b19eb534a69adb101541ee7068a7ca0a0272c6ebca2c13af"
     "f13be2d0b19bde135ac688c6c68c4e3052502b8ec4f2356b8fd22e4d31517de98c59cd421d624544c2c1353f5dca"
     "ec4ab8d8126963d5f9f5635048d8a5e56426febe50c3",
     "a963bb65f084c6f825d6ce9857156a28"},
    {AeadAlgorithm::Aegis256, "5958a371e26fff28efef8a6e71a0b81b4a14e3cf57ac75d215376e050468806b",
     "1293abdd7b6c43483f8caa43836922fb3a92feb4eb1476f4fa5ec4f06a0431f7", "5f6cd8814bf4915f08cbf1",
     nullptr, "06544eb4f4baafd8880df8a4e1da38d3111149aef41669b56ae2",
     "e4b718de939d6fbb41e32b57098b08fa16bd39ccc085625d0546",
     "d1a48afe22b3c7d28e239f103b93b0a200428f4bb8e80fcb2e5110fd1ed780eb"},
    {AeadAlgorithm::Aegis256, "96cfab5f246dfcf8b33a9e80fb15f90a089a9078dabacbe767082da806cb4fc9",
     "6a0ad0d16594d33730b03a7b40b86581fda3661264ea17f3a4327160a30f181d",
     "a62d02eac118d047cae4ebd58ce97a7c99ed90f4a4bee9a442", nullptr, "61b0d79387c11ac4a87c37a9b3",
     "d0762aa4c8d20934e91a999ca5",
     "c0dc96d5ad1cdff9445e163c0df739880bb4dd741f4ca70eef655b213b53773e"},
    {AeadAlgorithm::Aegis128L, "e2e2a29db958c6a3f68a52825b844c2a",
     "3210fe0cede911318435fefee1d921d9", "91209d1202574e", nullptr, "45f5fc3a", "2067b789",
     "8869621138c4b08670fd8b6ede57933e4036e9c2a635e367f12a4dd7b19e1d73"},
    {AeadAlgorithm::Aegis128L, "37263267c4f24129d9db09a2a96d7c14",
     "39e5c4f2b36c9ed5077765b89cea1bed", "6ea6a9f99350a38601162f2e24928ee2", nullptr, "3581b4424c",
     "9ace0569f7", "746536436bc496acc8bab10e6ae17d5d1d6113b3fad96df462107c3b4b6bf96e"},
};

constexpr AeadAlgorithm AllAlgorithms[] = {
    AeadAlgorithm::XChaCha20Poly1305, AeadAlgorithm::ChaCha20Poly1305Ietf,
    AeadAlgorithm::Aes256Gcm,         AeadAlgorithm::Aes128Gcm,
    AeadAlgorithm::Aegis256,          AeadAlgorithm::Aegis128L,
};

// 고수준 TrySeal·TryOpen을 받는 알고리즘(무작위 nonce가 안전한 것)이다.
constexpr AeadAlgorithm RandomNonceAlgorithms[] = {
    AeadAlgorithm::XChaCha20Poly1305,
    AeadAlgorithm::Aegis256,
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

void FillPattern(Span<uint8> bytes, uint8 seed) {
    for (usize i = 0; i < bytes.Size(); ++i) {
        bytes[i] = static_cast<uint8>(seed + i * 7);
    }
}

} // namespace

TEST(CryptoAeadInfo) {
    struct Expected {
        AeadAlgorithm algorithm;
        usize keyBytes;
        usize nonceBytes;
        usize tagBytes;
        bool randomNonceSafe;
    };

    const Expected expected[] = {
        {AeadAlgorithm::XChaCha20Poly1305, 32, 24, 16, true},
        {AeadAlgorithm::ChaCha20Poly1305Ietf, 32, 12, 16, false},
        {AeadAlgorithm::Aes256Gcm, 32, 12, 16, false},
        {AeadAlgorithm::Aes128Gcm, 16, 12, 16, false},
        {AeadAlgorithm::Aegis256, 32, 32, 32, true},
        {AeadAlgorithm::Aegis128L, 16, 16, 32, false},
    };
    for (const Expected& item : expected) {
        auto info = TryGetAeadInfo(item.algorithm);
        REQUIRE(info);
        CHECK(info.Value().keyBytes == item.keyBytes);
        CHECK(info.Value().nonceBytes == item.nonceBytes);
        CHECK(info.Value().tagBytes == item.tagBytes);
        CHECK(info.Value().randomNonceSafe == item.randomNonceSafe);
        CHECK(info.Value().maxMessageBytes > (uint64{1} << 32));
        // 쓸 수 있으면 구현 이름이 있고, 없으면 빈 문자열이다.
        CHECK(info.Value().available == !info.Value().implementation.IsEmpty());
    }

    // ChaCha20 계열은 모든 기기에서 쓸 수 있고 AES-128-GCM은 AES 경량 계층 전까지 없다.
    CHECK(IsAvailable(AeadAlgorithm::XChaCha20Poly1305));
    CHECK(IsAvailable(AeadAlgorithm::ChaCha20Poly1305Ietf));
    CHECK(!IsAvailable(AeadAlgorithm::Aes128Gcm));
    CHECK(TryGetAeadInfo(AeadAlgorithm::XChaCha20Poly1305).Value().implementation ==
          StringView("libsodium"));
    // AEGIS-128L과 AEGIS-256은 같은 HW 조건이다.
    CHECK(IsAvailable(AeadAlgorithm::Aegis256) == IsAvailable(AeadAlgorithm::Aegis128L));

    CHECK(GetPreferredAead() == AeadAlgorithm::XChaCha20Poly1305);
    CHECK(TryGetAeadInfo(static_cast<AeadAlgorithm>(0)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetAeadInfo(static_cast<AeadAlgorithm>(7)).Error() == ErrorCode::InvalidArgument);

    CHECK(TryGetSealedBytes(AeadAlgorithm::XChaCha20Poly1305, 10).Value() == 50);
    CHECK(TryGetSealedBytes(AeadAlgorithm::Aegis256, 0).Value() == 64);
    CHECK(TryGetSealedBytes(AeadAlgorithm::XChaCha20Poly1305, MaxSize - 40).Value() == MaxSize);
    CHECK(TryGetSealedBytes(AeadAlgorithm::XChaCha20Poly1305, MaxSize - 39).Error() ==
          ErrorCode::Overflow);
    CHECK(TryGetSealedBytes(static_cast<AeadAlgorithm>(0), 1).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(CryptoAeadKnownAnswers) {
    for (const AeadCase& item : AeadCases) {
        const HexBytes keyBytes(item.key);
        const HexBytes nonce(item.nonce);
        const HexBytes associatedData(item.associatedData);
        const HexBytes messageHex(item.text != nullptr ? "" : item.messageHex);
        const Span<const uint8> message =
            item.text != nullptr ? Text(item.text) : messageHex.View();
        const HexBytes ciphertext(item.ciphertext);
        const HexBytes tag(item.tag);
        REQUIRE(keyBytes.size != MaxSize && nonce.size != MaxSize &&
                associatedData.size != MaxSize && messageHex.size != MaxSize &&
                ciphertext.size == message.Size() && tag.size != MaxSize);

        auto key = AeadKey::TryImport(item.algorithm, keyBytes.View());
        if (!IsAvailable(item.algorithm)) {
            // HW가 없으면 soft 구현으로 내려가지 않고 Unsupported다(결정 D3).
            CHECK(key.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(key);
        CHECK(key.Value().Algorithm() == item.algorithm);

        // 결합 형식(암호문 ‖ tag)이다.
        uint8 sealed[256];
        auto written = hazmat::TrySealWithNonce(key.Value(), nonce.View(), associatedData.View(),
                                                message, Span<uint8>(sealed));
        REQUIRE(written);
        REQUIRE(written.Value() == message.Size() + tag.size);
        CHECK(
            ConstantTimeEquals(Span<const uint8>(sealed).Left(message.Size()), ciphertext.View()));
        CHECK(ConstantTimeEquals(Span<const uint8>(sealed).Mid(message.Size(), tag.size),
                                 tag.View()));

        uint8 opened[256];
        auto openedBytes = hazmat::TryOpenWithNonce(
            key.Value(), nonce.View(), associatedData.View(),
            Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened));
        REQUIRE(openedBytes);
        REQUIRE(openedBytes.Value() == message.Size());
        CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(message.Size()), message));

        // 분리 형식이다.
        uint8 detachedCiphertext[256];
        uint8 detachedTag[32];
        const Span<uint8> ciphertextOut = Span<uint8>(detachedCiphertext).Left(message.Size());
        const Span<uint8> tagOut = Span<uint8>(detachedTag).Left(tag.size);
        REQUIRE(hazmat::TrySealDetached(key.Value(), nonce.View(), associatedData.View(), message,
                                        ciphertextOut, tagOut));
        CHECK(ConstantTimeEquals(ciphertextOut, ciphertext.View()));
        CHECK(ConstantTimeEquals(tagOut, tag.View()));
        const Span<uint8> plaintextOut = Span<uint8>(opened).Left(message.Size());
        SecureZero(plaintextOut);
        REQUIRE(hazmat::TryOpenDetached(key.Value(), nonce.View(), associatedData.View(),
                                        ciphertextOut, tagOut, plaintextOut));
        CHECK(ConstantTimeEquals(plaintextOut, message));

        // 제자리(입력과 출력이 같은 주소에서 시작)도 같은 결과다.
        uint8 inPlace[256];
        memcpy(inPlace, message.Data(), message.Size());
        auto inPlaceWritten = hazmat::TrySealWithNonce(
            key.Value(), nonce.View(), associatedData.View(),
            Span<const uint8>(inPlace).Left(message.Size()), Span<uint8>(inPlace));
        REQUIRE(inPlaceWritten);
        CHECK(ConstantTimeEquals(Span<const uint8>(inPlace).Left(written.Value()),
                                 Span<const uint8>(sealed).Left(written.Value())));
        auto inPlaceOpened = hazmat::TryOpenWithNonce(
            key.Value(), nonce.View(), associatedData.View(),
            Span<const uint8>(inPlace).Left(inPlaceWritten.Value()), Span<uint8>(inPlace));
        REQUIRE(inPlaceOpened);
        CHECK(ConstantTimeEquals(Span<const uint8>(inPlace).Left(message.Size()), message));
    }
}

TEST(CryptoAeadSealOpenRoundTrip) {
    for (AeadAlgorithm algorithm : RandomNonceAlgorithms) {
        auto key = AeadKey::TryGenerate(algorithm);
        if (!IsAvailable(algorithm)) {
            CHECK(key.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(key);
        const AeadInfo info = TryGetAeadInfo(algorithm).Value();
        const usize overhead = info.nonceBytes + info.tagBytes;
        const Span<const uint8> associatedData = Text("header v1");
        const Span<const uint8> message = Text(Sunscreen);

        uint8 sealed[256];
        auto written = TrySeal(key.Value(), associatedData, message, Span<uint8>(sealed));
        REQUIRE(written);
        CHECK(written.Value() == TryGetSealedBytes(algorithm, message.Size()).Value());
        CHECK(written.Value() == message.Size() + overhead);

        uint8 opened[256];
        auto openedBytes =
            TryOpen(key.Value(), associatedData, Span<const uint8>(sealed).Left(written.Value()),
                    Span<uint8>(opened));
        REQUIRE(openedBytes);
        REQUIRE(openedBytes.Value() == message.Size());
        CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(message.Size()), message));

        // 같은 입력도 nonce가 매번 달라 결과가 다르다.
        uint8 again[256];
        REQUIRE(TrySeal(key.Value(), associatedData, message, Span<uint8>(again)));
        CHECK(!ConstantTimeEquals(Span<const uint8>(sealed).Left(info.nonceBytes),
                                  Span<const uint8>(again).Left(info.nonceBytes)));

        // 빈 평문·빈 AD도 봉인한다(출력은 nonce ‖ tag).
        uint8 empty[64];
        auto emptyWritten =
            TrySeal(key.Value(), Span<const uint8>(), Span<const uint8>(), Span<uint8>(empty));
        REQUIRE(emptyWritten);
        CHECK(emptyWritten.Value() == overhead);
        auto emptyOpened = TryOpen(key.Value(), Span<const uint8>(),
                                   Span<const uint8>(empty).Left(overhead), Span<uint8>());
        REQUIRE(emptyOpened);
        CHECK(emptyOpened.Value() == 0);

        // 제자리: 평문을 버퍼의 nonce 자리 뒤에 두고 봉인하고, 같은 자리로 연다.
        uint8 buffer[256];
        memcpy(buffer + info.nonceBytes, message.Data(), message.Size());
        auto inPlaceWritten = TrySeal(
            key.Value(), associatedData,
            Span<const uint8>(buffer).Mid(info.nonceBytes, message.Size()), Span<uint8>(buffer));
        REQUIRE(inPlaceWritten);
        auto inPlaceOpened = TryOpen(key.Value(), associatedData,
                                     Span<const uint8>(buffer).Left(inPlaceWritten.Value()),
                                     Span<uint8>(buffer).RightChop(info.nonceBytes));
        REQUIRE(inPlaceOpened);
        CHECK(ConstantTimeEquals(Span<const uint8>(buffer).Mid(info.nonceBytes, message.Size()),
                                 message));

        // 내보낸 키로 다시 만든 키도 연다.
        uint8 exported[32];
        const Span<uint8> exportedKey = Span<uint8>(exported).Left(info.keyBytes);
        REQUIRE(hazmat::TryExportKey(key.Value(), exportedKey));
        auto imported = AeadKey::TryImport(algorithm, exportedKey);
        SecureZero(exportedKey);
        REQUIRE(imported);
        CHECK(TryOpen(imported.Value(), associatedData,
                      Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened)));
    }
}

TEST(CryptoAeadRejectsTampering) {
    for (AeadAlgorithm algorithm : RandomNonceAlgorithms) {
        if (!IsAvailable(algorithm)) {
            continue;
        }

        auto key = AeadKey::TryGenerate(algorithm);
        auto otherKey = AeadKey::TryGenerate(algorithm);
        REQUIRE(key && otherKey);
        const AeadInfo info = TryGetAeadInfo(algorithm).Value();
        const Span<const uint8> associatedData = Text("header v1");
        uint8 message[100];
        FillPattern(Span<uint8>(message), 3);

        uint8 sealed[256] = {};
        auto written =
            TrySeal(key.Value(), associatedData, Span<const uint8>(message), Span<uint8>(sealed));
        REQUIRE(written);
        const usize sealedBytes = written.Value();

        // 실패하면 AuthenticationFailed이고 plaintext 전체가 0이다.
        uint8 opened[256];
        const auto expectRejected = [&](Span<const uint8> input, Span<const uint8> ad,
                                        const AeadKey& openKey) {
            memset(opened, 0xAA, sizeof(opened));
            auto result = TryOpen(openKey, ad, input, Span<uint8>(opened));
            CHECK(result.Error() == ErrorCode::AuthenticationFailed);
            CHECK(AllZero(Span<const uint8>(opened)));
        };

        // nonce·암호문·tag 어느 한 bit를 바꿔도 실패한다.
        const usize positions[] = {0,
                                   info.nonceBytes - 1,
                                   info.nonceBytes,
                                   info.nonceBytes + 50,
                                   sealedBytes - info.tagBytes,
                                   sealedBytes - 1};
        for (usize position : positions) {
            uint8 tampered[256];
            memcpy(tampered, sealed, sealedBytes);
            tampered[position] ^= 0x01;
            expectRejected(Span<const uint8>(tampered).Left(sealedBytes), associatedData,
                           key.Value());
        }

        const Span<const uint8> good = Span<const uint8>(sealed).Left(sealedBytes);
        expectRejected(good, Text("header v2"), key.Value());
        expectRejected(good, Span<const uint8>(), key.Value());
        expectRejected(good, associatedData, otherKey.Value());
        // 잘리거나 늘어난 입력, nonce + tag보다 짧은 입력, 빈 입력이다.
        expectRejected(good.Left(sealedBytes - 1), associatedData, key.Value());
        expectRejected(Span<const uint8>(sealed).Left(sealedBytes + 1), associatedData,
                       key.Value());
        expectRejected(good.Left(info.nonceBytes + info.tagBytes - 1), associatedData, key.Value());
        expectRejected(Span<const uint8>(), associatedData, key.Value());

        // 원본은 여전히 열린다.
        CHECK(TryOpen(key.Value(), associatedData, good, Span<uint8>(opened)));
    }

    // hazmat 분리 형식: tag 변조·잘린 tag도 AuthenticationFailed이고 plaintext를 지운다.
    auto key = AeadKey::TryGenerate(AeadAlgorithm::ChaCha20Poly1305Ietf);
    REQUIRE(key);
    uint8 nonce[12] = {};
    uint8 message[40];
    FillPattern(Span<uint8>(message), 9);
    uint8 ciphertext[40];
    uint8 tag[16];
    REQUIRE(hazmat::TrySealDetached(key.Value(), Span<const uint8>(nonce), Span<const uint8>(),
                                    Span<const uint8>(message), Span<uint8>(ciphertext),
                                    Span<uint8>(tag)));
    uint8 opened[40];
    tag[3] ^= 0x80;
    memset(opened, 0xAA, sizeof(opened));
    CHECK(hazmat::TryOpenDetached(key.Value(), Span<const uint8>(nonce), Span<const uint8>(),
                                  Span<const uint8>(ciphertext), Span<const uint8>(tag),
                                  Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(opened)));
    tag[3] ^= 0x80;
    memset(opened, 0xAA, sizeof(opened));
    CHECK(hazmat::TryOpenDetached(key.Value(), Span<const uint8>(nonce), Span<const uint8>(),
                                  Span<const uint8>(ciphertext), Span<const uint8>(tag).Left(15),
                                  Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(opened)));
    REQUIRE(hazmat::TryOpenDetached(key.Value(), Span<const uint8>(nonce), Span<const uint8>(),
                                    Span<const uint8>(ciphertext), Span<const uint8>(tag),
                                    Span<uint8>(opened)));
    CHECK(ConstantTimeEquals(Span<const uint8>(opened), Span<const uint8>(message)));

    // 결합 형식에서 tag보다 짧은 입력이다.
    memset(opened, 0xAA, sizeof(opened));
    CHECK(hazmat::TryOpenWithNonce(key.Value(), Span<const uint8>(nonce), Span<const uint8>(),
                                   Span<const uint8>(tag).Left(15), Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(opened)));
}

TEST(CryptoAeadRejectsMisuse) {
    const Span<const uint8> message = Text("message");
    uint8 buffer[128] = {};
    uint8 opened[128] = {};

    // 빈 키와 옮겨 간 키는 StaleHandle이다.
    AeadKey empty;
    CHECK(empty.IsEmpty());
    CHECK(TrySeal(empty, Span<const uint8>(), message, Span<uint8>(buffer)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryOpen(empty, Span<const uint8>(), Span<const uint8>(buffer), Span<uint8>(opened))
              .Error() == ErrorCode::StaleHandle);
    CHECK(hazmat::TryExportKey(empty, Span<uint8>(buffer).Left(32)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(hazmat::TrySealWithNonce(empty, Span<const uint8>(buffer).Left(24), Span<const uint8>(),
                                   message, Span<uint8>(opened))
              .Error() == ErrorCode::StaleHandle);

    auto generated = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(generated);
    AeadKey key = Move(generated.Value());
    CHECK(generated.Value().IsEmpty());
    CHECK(TrySeal(generated.Value(), Span<const uint8>(), message, Span<uint8>(buffer)).Error() ==
          ErrorCode::StaleHandle);

    // 알고리즘·키 길이 검사다.
    CHECK(AeadKey::TryGenerate(AeadAlgorithm::Aes128Gcm).Error() == ErrorCode::Unsupported);
    CHECK(
        AeadKey::TryImport(AeadAlgorithm::Aes128Gcm, Span<const uint8>(buffer).Left(16)).Error() ==
        ErrorCode::Unsupported);
    CHECK(AeadKey::TryGenerate(static_cast<AeadAlgorithm>(0)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(AeadKey::TryImport(static_cast<AeadAlgorithm>(9), Span<const uint8>(buffer).Left(32))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(AeadKey::TryImport(AeadAlgorithm::XChaCha20Poly1305, Span<const uint8>(buffer).Left(31))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(AeadKey::TryImport(AeadAlgorithm::XChaCha20Poly1305, Span<const uint8>(buffer).Left(33))
              .Error() == ErrorCode::InvalidArgument);

    // 96 bit nonce 알고리즘은 고수준 TrySeal·TryOpen을 받지 않는다.
    auto ietf = AeadKey::TryGenerate(AeadAlgorithm::ChaCha20Poly1305Ietf);
    REQUIRE(ietf);
    CHECK(TrySeal(ietf.Value(), Span<const uint8>(), message, Span<uint8>(buffer)).Error() ==
          ErrorCode::Unsupported);
    CHECK(TryOpen(ietf.Value(), Span<const uint8>(), Span<const uint8>(buffer), Span<uint8>(opened))
              .Error() == ErrorCode::Unsupported);
    if (IsAvailable(AeadAlgorithm::Aegis128L)) {
        auto aegis128 = AeadKey::TryGenerate(AeadAlgorithm::Aegis128L);
        REQUIRE(aegis128);
        CHECK(
            TrySeal(aegis128.Value(), Span<const uint8>(), message, Span<uint8>(buffer)).Error() ==
            ErrorCode::Unsupported);
    }

    // 출력이 작으면 CapacityExceeded이고 쓰지 않는다.
    const usize sealedBytes = TryGetSealedBytes(key.Algorithm(), message.Size()).Value();
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(TrySeal(key, Span<const uint8>(), message, Span<uint8>(buffer).Left(sealedBytes - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(buffer[0] == 0x5A && buffer[sealedBytes - 2] == 0x5A);
    REQUIRE(TrySeal(key, Span<const uint8>(), message, Span<uint8>(buffer)).Value() == sealedBytes);
    memset(opened, 0x5A, sizeof(opened));
    CHECK(TryOpen(key, Span<const uint8>(), Span<const uint8>(buffer).Left(sealedBytes),
                  Span<uint8>(opened).Left(message.Size() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(opened[0] == 0x5A);

    // 허용되지 않은 겹침은 쓰기 전에 InvalidArgument다.
    uint8 overlap[128];
    memcpy(overlap, message.Data(), message.Size());
    CHECK(TrySeal(key, Span<const uint8>(), Span<const uint8>(overlap).Left(message.Size()),
                  Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TrySeal(key, Span<const uint8>(overlap).Mid(30, 4), message, Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    memcpy(overlap, buffer, sealedBytes);
    CHECK(TryOpen(key, Span<const uint8>(), Span<const uint8>(overlap).Left(sealedBytes),
                  Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryOpen(key, Span<const uint8>(overlap).Right(4),
                  Span<const uint8>(buffer).Left(sealedBytes), Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ConstantTimeEquals(Span<const uint8>(overlap).Left(sealedBytes),
                             Span<const uint8>(buffer).Left(sealedBytes)));

    // hazmat 인자 검사다.
    const Span<const uint8> nonce24 = Span<const uint8>(buffer).Left(24);
    CHECK(hazmat::TrySealWithNonce(key, nonce24.Left(23), Span<const uint8>(), message,
                                   Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryOpenWithNonce(key, nonce24.Left(23), Span<const uint8>(),
                                   Span<const uint8>(buffer).Left(40), Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealWithNonce(key, Span<const uint8>(opened).Left(24), Span<const uint8>(),
                                   message, Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealWithNonce(key, nonce24, Span<const uint8>(), message,
                                   Span<uint8>(opened).Left(message.Size() + 15))
              .Error() == ErrorCode::CapacityExceeded);
    uint8 ciphertext[16];
    uint8 tag[16];
    CHECK(hazmat::TrySealDetached(key, nonce24, Span<const uint8>(), message,
                                  Span<uint8>(ciphertext), Span<uint8>(tag))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealDetached(key, nonce24, Span<const uint8>(), message,
                                  Span<uint8>(ciphertext).Left(message.Size()),
                                  Span<uint8>(tag).Left(15))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealDetached(key, nonce24, Span<const uint8>(),
                                  Span<const uint8>(ciphertext).Left(message.Size()),
                                  Span<uint8>(ciphertext).Mid(1, message.Size()), Span<uint8>(tag))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryOpenDetached(key, nonce24, Span<const uint8>(),
                                  Span<const uint8>(ciphertext).Left(message.Size()),
                                  Span<const uint8>(tag), Span<uint8>(opened).Left(8))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryOpenDetached(key, nonce24, Span<const uint8>(),
                                  Span<const uint8>(ciphertext).Left(message.Size()),
                                  Span<const uint8>(tag).Left(8), Span<uint8>(tag).Mid(4, 7))
              .Error() == ErrorCode::InvalidArgument);

    // 키 내보내기 길이 검사다.
    CHECK(hazmat::TryExportKey(key, Span<uint8>(buffer).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(CryptoAeadKeyDerivation) {
    const KdfKey master = TestMaster();
    REQUIRE(!master.IsEmpty());

    // AEAD 키 타입 파생은 BLAKE2b(key = master, salt = LE64(id) ‖ 0⁸, personal = context ‖ "RkAead"
    // 0 알고리즘)이다. 저장된 키 계층이 바뀌지 않도록 값으로 고정한다.
    struct Derived {
        AeadAlgorithm algorithm;
        const char* expected;
    };

    const Derived cases[] = {
        {AeadAlgorithm::XChaCha20Poly1305,
         "727c0fcd6d16e74cadcf3d34208644f8d4cb748793215162f6bc5065300c11ff"},
        {AeadAlgorithm::ChaCha20Poly1305Ietf,
         "be8ecf7406501146f27d22de1841fcf58299720bec4715f6089eab22a68a8ba3"},
        {AeadAlgorithm::Aes256Gcm,
         "65b4f9b8693786bc4a584b1ee4f13d1002d7af107c3980f6dd036c2ffcc53d1c"},
        {AeadAlgorithm::Aegis256,
         "b189ca935675849c7206677e561138bf4a4a028a8aa24f85e59dac387f2cbe83"},
        {AeadAlgorithm::Aegis128L, "56b4b4a6d915e374e3a3150cfbc65417"},
    };
    uint8 exported[32];
    for (const Derived& item : cases) {
        auto key = TryDeriveAeadKey(master, 7, TestContext, item.algorithm);
        if (!IsAvailable(item.algorithm)) {
            CHECK(key.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(key);
        CHECK(key.Value().Algorithm() == item.algorithm);
        const usize keyBytes = TryGetAeadInfo(item.algorithm).Value().keyBytes;
        REQUIRE(hazmat::TryExportKey(key.Value(), Span<uint8>(exported).Left(keyBytes)));
        CHECK(Equals(Span<const uint8>(exported).Left(keyBytes), item.expected));
    }

    // 같은 (master, id, context)의 crypto_kdf 파생(KdfTests의 cd49ce51...)과 다르다.
    auto secret = TryDeriveSecret(master, 7, TestContext, 32);
    REQUIRE(secret);
    CHECK(!Equals(secret.Value().View(), cases[0].expected));

    // 파생 키로 봉인하고 다시 파생한 키로 연다.
    auto first = TryDeriveAeadKey(master, 7, TestContext, AeadAlgorithm::XChaCha20Poly1305);
    auto second = TryDeriveAeadKey(master, 7, TestContext, AeadAlgorithm::XChaCha20Poly1305);
    auto otherId = TryDeriveAeadKey(master, 8, TestContext, AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(first && second && otherId);
    uint8 sealed[64];
    auto written = TrySeal(first.Value(), Span<const uint8>(), Text("hello"), Span<uint8>(sealed));
    REQUIRE(written);
    uint8 opened[64];
    CHECK(TryOpen(second.Value(), Span<const uint8>(),
                  Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened)));
    CHECK(TryOpen(otherId.Value(), Span<const uint8>(),
                  Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);

    CHECK(TryDeriveAeadKey(KdfKey(), 7, TestContext, AeadAlgorithm::XChaCha20Poly1305).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryDeriveAeadKey(master, 7, TestContext, AeadAlgorithm::Aes128Gcm).Error() ==
          ErrorCode::Unsupported);
    CHECK(TryDeriveAeadKey(master, 7, TestContext, static_cast<AeadAlgorithm>(0)).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(CryptoAeadKeyLifetime) {
    // 모든 사용 가능한 알고리즘의 키는 키 저장소를 쓰고, 소멸·이동 대입 때 돌려준다. AES-256-GCM은
    // 키 확장 상태 슬롯도 함께 돌려준다.
    for (AeadAlgorithm algorithm : AllAlgorithms) {
        if (!IsAvailable(algorithm)) {
            continue;
        }

        const usize used = GetSecretMemoryStatus().arenaUsedBytes;
        {
            auto generated = AeadKey::TryGenerate(algorithm);
            REQUIRE(generated);
            CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
            const usize oneKey = GetSecretMemoryStatus().arenaUsedBytes;

            // 이동한 키도 같은 결과를 낸다(AES-256-GCM 상태가 함께 옮겨 간다).
            const AeadInfo info = TryGetAeadInfo(algorithm).Value();
            uint8 nonce[32] = {};
            uint8 sealed[64];
            REQUIRE(hazmat::TrySealWithNonce(
                generated.Value(), Span<const uint8>(nonce).Left(info.nonceBytes),
                Span<const uint8>(), Text("lifetime"), Span<uint8>(sealed)));
            AeadKey moved = Move(generated.Value());
            CHECK(GetSecretMemoryStatus().arenaUsedBytes == oneKey);
            uint8 opened[16];
            auto openedBytes = hazmat::TryOpenWithNonce(
                moved, Span<const uint8>(nonce).Left(info.nonceBytes), Span<const uint8>(),
                Span<const uint8>(sealed).Left(8 + info.tagBytes), Span<uint8>(opened));
            REQUIRE(openedBytes);
            CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(8), Text("lifetime")));

            // 이동 대입은 대상의 이전 키를 돌려준다.
            auto replacement = AeadKey::TryGenerate(algorithm);
            REQUIRE(replacement);
            moved = Move(replacement.Value());
            CHECK(GetSecretMemoryStatus().arenaUsedBytes == oneKey);
            CHECK(!moved.IsEmpty());
        }

        CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
    }
}
