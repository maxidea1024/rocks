#include <Base/Compiler.h>
#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>
#include <Platform/Cpu.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처:
// - ChaCha20-Poly1305: RFC 8439 2.8.2(암호문·tag가 RFC 값과 같다).
// - XChaCha20-Poly1305: draft-irtf-cfrg-xchacha-03 A.3.1(공개된 출력과 같다, pycryptodome으로
//   재현).
// - AES-256-GCM: RFC 8439 2.8.2의 키·AD·평문에 nonce 00..0b를 cryptography(OpenSSL)와
//   pycryptodome으로 계산했다.
// - 빈 평문 행: 같은 키·nonce로 위 두 라이브러리가 낸 tag다.
// - AEGIS-128L·256: draft-irtf-cfrg-aegis-aead 부록 TV1–TV3(tag256)과, libsodium 1.0.22-stable
//   test/default/aead_aegis*.c 무작위 표의 행이다. 표 행은 draft 규격대로 짠 독립 Python 구현(draft
//   AESRound·TV1–TV3 통과)으로 다시 계산해 확인했다.
// - AEAD 키 타입 파생: Python hashlib.blake2b로 계산했다.
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

bool AllEqual(Span<const uint8> bytes, uint8 value) {
    for (uint8 byte : bytes) {
        if (byte != value) {
            return false;
        }
    }

    return true;
}

bool AllZero(Span<const uint8> bytes) {
    return AllEqual(bytes, 0);
}

bool IsAvailable(AeadAlgorithm algorithm) {
    auto info = TryGetAeadInfo(algorithm);
    return info && info.Value().available;
}

void FillPattern(Span<uint8> bytes, uint8 seed) {
    for (usize i = 0; i < bytes.Size(); ++i) {
        bytes[i] = static_cast<uint8>(seed + i * 7);
    }
}

// RFC 8439 2.8.2 평문(114 byte)이다.
constexpr const char* Sunscreen =
    "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, "
    "sunscreen would be it.";

// RFC 8439 2.8.2 키(80..9f)와 AD다.
constexpr const char* RfcKey = "808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f";
constexpr const char* RfcAd = "50515253c0c1c2c3c4c5c6c7";
constexpr const char* IetfNonce = "070000004041424344454647";
constexpr const char* XChaChaNonce = "404142434445464748494a4b4c4d4e4f5051525354555657";
constexpr const char* GcmNonce = "000102030405060708090a0b";

// AEGIS draft 부록의 키·nonce·TV3 입력이다.
constexpr const char* DraftKey128 = "10010000000000000000000000000000";
constexpr const char* DraftNonce128 = "10000200000000000000000000000000";
constexpr const char* DraftKey256 =
    "1001000000000000000000000000000000000000000000000000000000000000";
constexpr const char* DraftNonce256 =
    "1000020000000000000000000000000000000000000000000000000000000000";
constexpr const char* DraftAd3 = "0001020304050607";
constexpr const char* DraftMessage3 =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f";
constexpr const char* Zero16 = "00000000000000000000000000000000";

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
    {AeadAlgorithm::ChaCha20Poly1305Ietf, RfcKey, IetfNonce, RfcAd, Sunscreen, nullptr,
     "d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d63dbea45e8ca9671282fafb69da92"
     "728b1a71de0a9e060b2905d6a5b67ecd3b3692ddbd7f2d778b8c9803aee328091b58fab324e4fad675945585808b"
     "4831d7bc3ff4def08e4b7a9de576d26586cec64b6116",
     "1ae10b594f09e26a7e902ecbd0600691"},
    {AeadAlgorithm::ChaCha20Poly1305Ietf, RfcKey, IetfNonce, RfcAd, nullptr, "", "",
     "e622e5647a38d967a7ecbcb46c7f675c"},
    {AeadAlgorithm::ChaCha20Poly1305Ietf, RfcKey, IetfNonce, "", nullptr, "", "",
     "a0784d7a4716f3feb4f64e7f4b39bf04"},
    {AeadAlgorithm::XChaCha20Poly1305, RfcKey, XChaChaNonce, RfcAd, Sunscreen, nullptr,
     "bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda"
     "7e39ae64c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b23835"
     "65d3fff921f9664c97637da9768812f615c68b13b52e",
     "c0875924c1c7987947deafd8780acf49"},
    {AeadAlgorithm::XChaCha20Poly1305, RfcKey, XChaChaNonce, RfcAd, nullptr, "", "",
     "e4c5191f68fd06d9592f83754480d19d"},
    {AeadAlgorithm::XChaCha20Poly1305, RfcKey, XChaChaNonce, "", nullptr, "", "",
     "1dac8f73146d1e9da796cb7f7221a5df"},
    {AeadAlgorithm::Aes256Gcm, RfcKey, GcmNonce, RfcAd, Sunscreen, nullptr,
     "bf211b4070555acaa810792b4d3889cd91eac3a754c8b19eb534a69adb101541ee7068a7ca0a0272c6ebca2c13af"
     "f13be2d0b19bde135ac688c6c68c4e3052502b8ec4f2356b8fd22e4d31517de98c59cd421d624544c2c1353f5dca"
     "ec4ab8d8126963d5f9f5635048d8a5e56426febe50c3",
     "a963bb65f084c6f825d6ce9857156a28"},
    {AeadAlgorithm::Aes256Gcm, RfcKey, GcmNonce, RfcAd, nullptr, "", "",
     "c80871bc777c9cffc0a6017037da7cb6"},
    {AeadAlgorithm::Aes256Gcm, RfcKey, GcmNonce, "", nullptr, "", "",
     "c1a3615f0acceab275f6be05b1b42ded"},
    // AEGIS draft 부록 TV1–TV3(tag256)이다.
    {AeadAlgorithm::Aegis128L, DraftKey128, DraftNonce128, "", nullptr, Zero16,
     "c1c0e58bd913006feba00f4b3cc3594e",
     "25835bfbb21632176cf03840687cb968cace4617af1bd0f7d064c639a5c79ee4"},
    {AeadAlgorithm::Aegis128L, DraftKey128, DraftNonce128, "", nullptr, "", "",
     "1360dc9db8ae42455f6e5b6a9d488ea4f2184c4e12120249335c4ee84bafe25d"},
    {AeadAlgorithm::Aegis128L, DraftKey128, DraftNonce128, DraftAd3, nullptr, DraftMessage3,
     "79d94593d8c2119d7e8fd9b8fc77845c5c077a05b2528b6ac54b563aed8efe84",
     "022cb796fe7e0ae1197525ff67e309484cfbab6528ddef89f17d74ef8ecd82b3"},
    {AeadAlgorithm::Aegis256, DraftKey256, DraftNonce256, "", nullptr, Zero16,
     "754fc3d8c973246dcc6d741412a4b236",
     "1181a1d18091082bf0266f66297d167d2e68b845f61a3b0527d31fc7b7b89f13"},
    {AeadAlgorithm::Aegis256, DraftKey256, DraftNonce256, "", nullptr, "", "",
     "6a348c930adbd654896e1666aad67de989ea75ebaa2b82fb588977b1ffec864a"},
    {AeadAlgorithm::Aegis256, DraftKey256, DraftNonce256, DraftAd3, nullptr, DraftMessage3,
     "f373079ed84b2709faee373584585d60accd191db310ef5d8b11833df9dec711",
     "b7d28d0c3c0ebd409fd22b44160503073a547412da0854bfb9723020dab8da1a"},
    // libsodium 시험 표의 행이다(짧은 입력, 여러 블록, 빈 AD, 긴 AD).
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
    {AeadAlgorithm::Aegis256, "8011b1043674d753172302aa123478a121640daf4317957545749d0be6a91698",
     "57bd1ac0f3db407989f88a762f60b3eabd03d3bc3bae577f3818b15c0974ae9c", "", nullptr,
     "be1833fd169fd745acaa7d8584c457657433e6a3237225a086d47806804120613d78344e097ecc6a5f869d07",
     "e34dff511e16bf12570a6828843c414b8fdced120db36ea0223e8700f57bea4c9dfbec5d3195caa633d52ee8",
     "0ac3f0459608a7f38b5b77c3f38c73f9ebc48253b316830b9583bcd51ba5c995"},
    {AeadAlgorithm::Aegis256, "38a7aa902690a3e1b285953e0121eae7304815e12a015fa98cbd227e6f7d73c1",
     "f23f9ac01c0b118b684f10031836f7c92e8a70eea0e916dba2952b685ae2c148",
     "6b726ca02fa44684e7d92ddb0b6e2f30d6a6e75b537f209d21bf2375718b4092286ea592f3c1750af21b12f649"
     "611370ee4bfac05e0281c9731242c507a56d9d6522c26e172fb406f3e61efbf3c346917988a1dc85c829d51d39"
     "54a0825d2ca4d0e2a784c78ea07bfc5973e80fe6b34ccfe72457",
     nullptr,
     "995580acc337ee1216802b1a45daf0df12280eb94953ac61916d35eee038e5ea1d1f53da1c6a3e17d54dd1555e"
     "79c4ac988494f805715f59f2404eee2fdb592fa538928d",
     "c73f3832441f59ce54910fd16dca9e2bd59a168ccd658ba3eb87fbb1ba561f63ad73ecf9618481bc6e8c8020c6"
     "0e8194cc65bdea155f0f6cc79adaf2334c099793efba4a",
     "5c52422a24f79990f26224082d375bf81eaf5df242389c894cf89c4d0131d01e"},
    {AeadAlgorithm::Aegis128L, "e2e2a29db958c6a3f68a52825b844c2a",
     "3210fe0cede911318435fefee1d921d9", "91209d1202574e", nullptr, "45f5fc3a", "2067b789",
     "8869621138c4b08670fd8b6ede57933e4036e9c2a635e367f12a4dd7b19e1d73"},
    {AeadAlgorithm::Aegis128L, "37263267c4f24129d9db09a2a96d7c14",
     "39e5c4f2b36c9ed5077765b89cea1bed", "6ea6a9f99350a38601162f2e24928ee2", nullptr, "3581b4424c",
     "9ace0569f7", "746536436bc496acc8bab10e6ae17d5d1d6113b3fad96df462107c3b4b6bf96e"},
    {AeadAlgorithm::Aegis128L, "8e6f1217eaf84aee8e5897f5860f184c",
     "a4e099068ad0b67f28b6902a40921dca",
     "4bb7fccecf15f0b32be37860507fc53812713194e2844855894ef916abbf9b5d", nullptr,
     "53c939f8d167e49980f8fd3ccc4a2ae3", "92e47292a4f02cc22d3392d1b6a089ce",
     "a3eb3e03808499409b00f0bb635c6fbf12062469edb45f5bb252c08748e131ed"},
    {AeadAlgorithm::Aegis128L, "711a437629429db2e14058e2a826dcbf",
     "eb036d6e483a212ff6ee25d970fe1ac3", "", nullptr,
     "29937c0efb36ed27fe7709d7179b4f38a2fc191b5e8d9616b58f6dc9ba2ab74e13bbdcd233e8726d90f7ded06c"
     "3861582f27158732f997df9091446befe75855ab05b348d68f96e45445f44c31e9ba3e4d7be96d9c8e806535e7"
     "9079139c71fcc599fea8701e0c2edf606986eff1535afdfa51d1be2dfdee",
     "4a61f5d6b8e746bf6fb49ca2b16c22f4e9ffcdc89a3137b39bf5445fb6b989d5200f0c8d5538891a5e8979b5cd"
     "8c734128b4e4ad98b0cd598c40ec9be74725dbca84c65a52f17ac983330b0b74e4193540f6357c3bcde4e8d8fc"
     "6942314ba68115bf2a682756e3c42008803a81532708a0e7b5e3b8436145",
     "4af113e2b6165247c2760ab445c6985306c81fb9ccebb8df0e57b0b044c52736"},
    {AeadAlgorithm::Aegis128L, "adff46e4d7d78b3db5c74c712534db37",
     "c54185637dd281ebf672393cf9bad28f",
     "59bc7a834189b930c8cbff769ef63b5e1a08c352ed779853b36bcd3d0ca7b4e35bd6cdaf2538ebf0e3a0d7cbcf"
     "3bcd2b66b910967c226a1da42f84c4a8f81e19161c6593e2c0a0fdddd3c6ab3a864037fbf976e8aebd33d4450b"
     "e9893da2e37e728916b663944e3fa6ba543d1010",
     nullptr,
     "b3850ad942e221753e4bf30140eb5569cfd9972246b9a6a35f7a8512db333aec59d380973d6a6505d99cb004dd"
     "47b33e32f4f238b1342e6756d3619414c31bde45",
     "5cc93a30fd8f71befd87fc50112c156b53abfc97466f36e3315915a7d4147f0b3641177b9d08ec13e7315957d0"
     "78ec73eb0a93a3b7a51e3db63a396e6ea2adfba7",
     "9c14e5bcf26a00fa0bb04256cc32736d0f2300ea93a51f8e4ba69d15ff11121a"},
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

} // namespace

TEST(CryptoAeadInfo) {
    struct Expected {
        AeadAlgorithm algorithm;
        usize keyBytes;
        usize nonceBytes;
        usize tagBytes;
        uint64 maxMessageBytes;
        bool randomNonceSafe;
    };

    // 평문 상한은 11절 표의 값이다(64 bit 대상). AES-256-GCM은 libsodium 구현이 받는 값(헤더
    // 상수보다 한 블록 작다)이고, AES-128-GCM은 NIST 상한이다.
    const uint64 sizeMax = MaxSize;
    const Expected expected[] = {
        {AeadAlgorithm::XChaCha20Poly1305, 32, 24, 16, sizeMax - 16, true},
        {AeadAlgorithm::ChaCha20Poly1305Ietf, 32, 12, 16, 64 * ((uint64{1} << 32) - 1), false},
        {AeadAlgorithm::Aes256Gcm, 32, 12, 16, 16 * ((uint64{1} << 32) - 3), false},
        {AeadAlgorithm::Aes128Gcm, 16, 12, 16, 16 * ((uint64{1} << 32) - 2), false},
        {AeadAlgorithm::Aegis256, 32, 32, 32, (uint64{1} << 61) - 1, true},
        {AeadAlgorithm::Aegis128L, 16, 16, 32, (uint64{1} << 61) - 1, false},
    };
    for (const Expected& item : expected) {
        auto info = TryGetAeadInfo(item.algorithm);
        REQUIRE(info);
        CHECK(info.Value().keyBytes == item.keyBytes);
        CHECK(info.Value().nonceBytes == item.nonceBytes);
        CHECK(info.Value().tagBytes == item.tagBytes);
        CHECK(info.Value().maxMessageBytes == item.maxMessageBytes);
        CHECK(info.Value().randomNonceSafe == item.randomNonceSafe);
        // 쓸 수 있으면 구현 이름이 있고, 없으면 빈 문자열이다.
        CHECK(info.Value().available == !info.Value().implementation.IsEmpty());
    }

    // ChaCha20 계열은 모든 기기에서 쓸 수 있고 AES-128-GCM은 AES 경량 계층 전까지 없다.
    CHECK(IsAvailable(AeadAlgorithm::XChaCha20Poly1305));
    CHECK(IsAvailable(AeadAlgorithm::ChaCha20Poly1305Ietf));
    CHECK(!IsAvailable(AeadAlgorithm::Aes128Gcm));
    CHECK(TryGetAeadInfo(AeadAlgorithm::XChaCha20Poly1305).Value().implementation ==
          StringView("libsodium"));
    CHECK(TryGetAeadInfo(AeadAlgorithm::ChaCha20Poly1305Ietf).Value().implementation ==
          StringView("libsodium"));

    // AES-256-GCM·AEGIS는 6.1절의 HW 조건 하나를 함께 쓴다. CPU에 명령이 있는데 쓸 수 없다고 나오면
    // libsodium이 HW 구현 없이 빌드된 것이다(빌드 정의 누락을 이 단언이 잡는다, 그러면 아래 HW
    // 시험이 조용히 건너뛰어진다).
    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    bool hardware = false;
    if constexpr (IsX64) {
        hardware = cpu.aes && cpu.pclmulqdq && cpu.avx;
    } else {
        hardware = cpu.armAes && cpu.armPmull;
    }

    const StringView hardwareName =
        IsX64 ? StringView("libsodium/aesni") : StringView("libsodium/armcrypto");
    const AeadAlgorithm hardwareAlgorithms[] = {AeadAlgorithm::Aes256Gcm, AeadAlgorithm::Aegis256,
                                                AeadAlgorithm::Aegis128L};
    for (AeadAlgorithm algorithm : hardwareAlgorithms) {
        const AeadInfo info = TryGetAeadInfo(algorithm).Value();
        CHECK(info.available == hardware);
        if (hardware) {
            CHECK(info.implementation == hardwareName);
        }
    }

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
            // HW가 없으면 soft 구현으로 내려가지 않고 Unsupported다(결정 D3). 이 기기에서 HW가
            // 있어야 하는지는 CryptoAeadInfo가 CpuFeatures로 단언한다.
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

        // 제자리(입력과 출력이 같은 주소에서 시작)도 같은 결과다. 결합 형식과 분리 형식 둘 다다.
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

        const Span<uint8> inPlaceText = Span<uint8>(inPlace).Left(message.Size());
        REQUIRE(hazmat::TrySealDetached(key.Value(), nonce.View(), associatedData.View(),
                                        inPlaceText, inPlaceText, tagOut));
        CHECK(ConstantTimeEquals(inPlaceText, ciphertext.View()));
        CHECK(ConstantTimeEquals(tagOut, tag.View()));
        REQUIRE(hazmat::TryOpenDetached(key.Value(), nonce.View(), associatedData.View(),
                                        inPlaceText, tagOut, inPlaceText));
        CHECK(ConstantTimeEquals(inPlaceText, message));
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
}

TEST(CryptoAeadHazmatRejectsTampering) {
    // 모든 사용 가능한 알고리즘의 hazmat 결합·분리 형식이다. AES-GCM은 libsodium이 실패한 출력을
    // 0xd0으로 채우므로 어댑터의 소거가 실제로 관찰된다.
    for (AeadAlgorithm algorithm : AllAlgorithms) {
        if (!IsAvailable(algorithm)) {
            continue;
        }

        const AeadInfo info = TryGetAeadInfo(algorithm).Value();
        auto key = AeadKey::TryGenerate(algorithm);
        REQUIRE(key);
        uint8 nonce[32];
        FillPattern(Span<uint8>(nonce), 0x21);
        const Span<const uint8> nonceView = Span<const uint8>(nonce).Left(info.nonceBytes);
        const Span<const uint8> associatedData = Text("hazmat ad");
        uint8 message[40];
        FillPattern(Span<uint8>(message), 0x42);
        uint8 sealed[128];
        auto written = hazmat::TrySealWithNonce(key.Value(), nonceView, associatedData,
                                                Span<const uint8>(message), Span<uint8>(sealed));
        REQUIRE(written);
        const usize sealedBytes = written.Value();
        const Span<const uint8> good = Span<const uint8>(sealed).Left(sealedBytes);

        // 결합 형식: 실패하면 메시지보다 큰 plaintext 전체가 0이다.
        uint8 opened[128];
        const auto expectCombinedRejected = [&](Span<const uint8> useNonce, Span<const uint8> ad,
                                                Span<const uint8> input) {
            memset(opened, 0xAA, sizeof(opened));
            auto result =
                hazmat::TryOpenWithNonce(key.Value(), useNonce, ad, input, Span<uint8>(opened));
            CHECK(result.Error() == ErrorCode::AuthenticationFailed);
            CHECK(AllZero(Span<const uint8>(opened)));
        };

        // 암호문 첫·끝 byte, tag 첫·가운데·끝 byte(32 byte tag는 뒤 16 byte 시작)다.
        const usize positions[] = {0, sizeof(message) - 1, sizeof(message),
                                   sizeof(message) + info.tagBytes / 2, sealedBytes - 1};
        for (usize position : positions) {
            uint8 tampered[128];
            memcpy(tampered, sealed, sealedBytes);
            tampered[position] ^= 0x04;
            expectCombinedRejected(nonceView, associatedData,
                                   Span<const uint8>(tampered).Left(sealedBytes));
        }

        uint8 otherNonce[32];
        memcpy(otherNonce, nonce, sizeof(nonce));
        otherNonce[info.nonceBytes - 1] ^= 0x01;
        expectCombinedRejected(Span<const uint8>(otherNonce).Left(info.nonceBytes), associatedData,
                               good);
        expectCombinedRejected(nonceView, Text("hazmat ae"), good);
        expectCombinedRejected(nonceView, associatedData, good.Left(sealedBytes - 1));
        expectCombinedRejected(nonceView, associatedData, good.Left(info.tagBytes - 1));

        // 분리 형식: plaintext는 암호문과 같은 크기라 소거 대상 전체가 평문 자리다.
        uint8 tag[40] = {};
        memcpy(tag, sealed + sizeof(message), info.tagBytes);
        const Span<const uint8> ciphertext = good.Left(sizeof(message));
        uint8 plain[sizeof(message)];
        const auto expectDetachedRejected = [&](Span<const uint8> useTag) {
            memset(plain, 0xAA, sizeof(plain));
            auto result = hazmat::TryOpenDetached(key.Value(), nonceView, associatedData,
                                                  ciphertext, useTag, Span<uint8>(plain));
            CHECK(result.Error() == ErrorCode::AuthenticationFailed);
            CHECK(AllZero(Span<const uint8>(plain)));
        };

        const usize tagPositions[] = {0, info.tagBytes / 2, info.tagBytes - 1};
        for (usize position : tagPositions) {
            uint8 badTag[40];
            memcpy(badTag, tag, sizeof(tag));
            badTag[position] ^= 0x80;
            expectDetachedRejected(Span<const uint8>(badTag).Left(info.tagBytes));
        }

        // 길이가 틀린 tag: 1 byte 짧거나 긴 tag, 32 byte tag 알고리즘의 16 byte tag(AEGIS tag128
        // 변형).
        expectDetachedRejected(Span<const uint8>(tag).Left(info.tagBytes - 1));
        expectDetachedRejected(Span<const uint8>(tag).Left(info.tagBytes + 1));
        if (info.tagBytes == 32) {
            expectDetachedRejected(Span<const uint8>(tag).Left(16));
        }

        REQUIRE(hazmat::TryOpenDetached(key.Value(), nonceView, associatedData, ciphertext,
                                        Span<const uint8>(tag).Left(info.tagBytes),
                                        Span<uint8>(plain)));
        CHECK(ConstantTimeEquals(Span<const uint8>(plain), Span<const uint8>(message)));
    }
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

    // 무작위 nonce가 안전하지 않은 알고리즘(ChaCha20-IETF·AES-GCM 96 bit, AEGIS-128L 128 bit
    // nonce)은 고수준 TrySeal·TryOpen을 받지 않는다.
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

    // 출력이 작으면 CapacityExceeded이고 아무것도 쓰지 않는다.
    const usize sealedBytes = TryGetSealedBytes(key.Algorithm(), message.Size()).Value();
    memset(buffer, 0x5A, sizeof(buffer));
    CHECK(TrySeal(key, Span<const uint8>(), message, Span<uint8>(buffer).Left(sealedBytes - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(AllEqual(Span<const uint8>(buffer), 0x5A));
    const Span<const uint8> nonce24 = Text("0123456789abcdefghijklmn");
    REQUIRE(nonce24.Size() == 24);
    CHECK(hazmat::TrySealWithNonce(key, nonce24, Span<const uint8>(), message,
                                   Span<uint8>(buffer).Left(message.Size() + 15))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(AllEqual(Span<const uint8>(buffer), 0x5A));

    REQUIRE(TrySeal(key, Span<const uint8>(), message, Span<uint8>(buffer)).Value() == sealedBytes);
    memset(opened, 0x5A, sizeof(opened));
    CHECK(TryOpen(key, Span<const uint8>(), Span<const uint8>(buffer).Left(sealedBytes),
                  Span<uint8>(opened).Left(message.Size() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(AllEqual(Span<const uint8>(opened), 0x5A));
    uint8 hazmatSealed[64];
    auto hazmatWritten = hazmat::TrySealWithNonce(key, nonce24, Span<const uint8>(), message,
                                                  Span<uint8>(hazmatSealed));
    REQUIRE(hazmatWritten);
    CHECK(hazmat::TryOpenWithNonce(key, nonce24, Span<const uint8>(),
                                   Span<const uint8>(hazmatSealed).Left(hazmatWritten.Value()),
                                   Span<uint8>(opened).Left(message.Size() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(AllEqual(Span<const uint8>(opened), 0x5A));

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

    // 제자리 시작 주소가 한 byte라도 어긋나면 겹침이다(고수준은 nonce 자리 ±1, hazmat은 +1).
    const usize nonceBytes = 24;
    const usize shifts[] = {nonceBytes - 1, nonceBytes + 1};
    for (usize shift : shifts) {
        uint8 shifted[128];
        memcpy(shifted + shift, message.Data(), message.Size());
        CHECK(TrySeal(key, Span<const uint8>(),
                      Span<const uint8>(shifted).Mid(shift, message.Size()), Span<uint8>(shifted))
                  .Error() == ErrorCode::InvalidArgument);
        memcpy(shifted, buffer, sealedBytes);
        CHECK(TryOpen(key, Span<const uint8>(), Span<const uint8>(shifted).Left(sealedBytes),
                      Span<uint8>(shifted).RightChop(shift))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(ConstantTimeEquals(Span<const uint8>(shifted).Left(sealedBytes),
                                 Span<const uint8>(buffer).Left(sealedBytes)));
    }

    uint8 shiftedHazmat[64];
    memcpy(shiftedHazmat + 1, message.Data(), message.Size());
    CHECK(hazmat::TrySealWithNonce(key, nonce24, Span<const uint8>(),
                                   Span<const uint8>(shiftedHazmat).Mid(1, message.Size()),
                                   Span<uint8>(shiftedHazmat))
              .Error() == ErrorCode::InvalidArgument);
    memcpy(shiftedHazmat, hazmatSealed, hazmatWritten.Value());
    CHECK(hazmat::TryOpenWithNonce(key, nonce24, Span<const uint8>(),
                                   Span<const uint8>(shiftedHazmat).Left(hazmatWritten.Value()),
                                   Span<uint8>(shiftedHazmat).RightChop(1))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ConstantTimeEquals(Span<const uint8>(shiftedHazmat).Left(hazmatWritten.Value()),
                             Span<const uint8>(hazmatSealed).Left(hazmatWritten.Value())));

    // 겹침 검사는 길이 검사보다 먼저다. 짧은 입력과 겹친 plaintext는 지우지 않고 InvalidArgument다.
    uint8 shortBuffer[64];
    FillPattern(Span<uint8>(shortBuffer), 0x13);
    uint8 shortCopy[64];
    memcpy(shortCopy, shortBuffer, sizeof(shortBuffer));
    CHECK(TryOpen(key, Span<const uint8>(), Span<const uint8>(shortBuffer).Left(30),
                  Span<uint8>(shortBuffer).Mid(5, 20))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryOpenWithNonce(key, nonce24, Span<const uint8>(),
                                   Span<const uint8>(shortBuffer).Left(10),
                                   Span<uint8>(shortBuffer).Mid(3, 8))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(memcmp(shortBuffer, shortCopy, sizeof(shortBuffer)) == 0);

    // hazmat 인자 검사다.
    CHECK(hazmat::TrySealWithNonce(key, nonce24.Left(23), Span<const uint8>(), message,
                                   Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TryOpenWithNonce(key, nonce24.Left(23), Span<const uint8>(),
                                   Span<const uint8>(buffer).Left(40), Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealWithNonce(key, Span<const uint8>(opened).Left(24), Span<const uint8>(),
                                   message, Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    uint8 ciphertext[16] = {};
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
    memset(tag, 0x3C, sizeof(tag));
    CHECK(hazmat::TryOpenDetached(key, nonce24, Span<const uint8>(),
                                  Span<const uint8>(ciphertext).Left(message.Size()),
                                  Span<const uint8>(tag).Left(8), Span<uint8>(tag).Mid(4, 7))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(AllEqual(Span<const uint8>(tag), 0x3C));

    // nonce 길이: ChaCha20 계열은 InvalidArgument, GCM의 96 bit가 아닌 nonce는 Unsupported다(GCM
    // 표준은 다른 길이를 허용하지만 지원하지 않는다).
    CHECK(hazmat::TrySealWithNonce(ietf.Value(), nonce24.Left(11), Span<const uint8>(), message,
                                   Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(hazmat::TrySealWithNonce(ietf.Value(), Span<const uint8>(), Span<const uint8>(), message,
                                   Span<uint8>(opened))
              .Error() == ErrorCode::InvalidArgument);
    if (IsAvailable(AeadAlgorithm::Aes256Gcm)) {
        auto gcm = AeadKey::TryGenerate(AeadAlgorithm::Aes256Gcm);
        REQUIRE(gcm);
        const usize lengths[] = {0, 8, 11, 13, 16};
        for (usize length : lengths) {
            CHECK(hazmat::TrySealWithNonce(gcm.Value(), nonce24.Left(length), Span<const uint8>(),
                                           message, Span<uint8>(opened))
                      .Error() == ErrorCode::Unsupported);
            CHECK(hazmat::TryOpenDetached(gcm.Value(), nonce24.Left(length), Span<const uint8>(),
                                          Span<const uint8>(ciphertext).Left(message.Size()),
                                          Span<const uint8>(tag),
                                          Span<uint8>(opened).Left(message.Size()))
                      .Error() == ErrorCode::Unsupported);
        }
    }

    // 키 내보내기 길이 검사다.
    CHECK(hazmat::TryExportKey(key, Span<uint8>(buffer).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(CryptoAeadKeyDerivation) {
    const KdfKey master = TestMaster();
    REQUIRE(!master.IsEmpty());

    // AEAD 키 타입 파생은 BLAKE2b(key = master, salt = LE64(id) ‖ 0⁸, personal = context ‖ "RkAead"
    // 0 알고리즘)이다. 저장된 키 계층이 바뀌지 않도록 값으로 고정한다. 같은 (id, context)의
    // crypto_kdf 파생(KdfTests의 cd49ce51...)과도 다르다.
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

    // 파생 키로 봉인하고 다시 파생한 키로 연다. 다른 id의 키로는 열리지 않는다.
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

    // 알고리즘 검사가 빈 master보다 먼저다.
    CHECK(TryDeriveAeadKey(KdfKey(), 7, TestContext, AeadAlgorithm::XChaCha20Poly1305).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryDeriveAeadKey(KdfKey(), 7, TestContext, AeadAlgorithm::Aes128Gcm).Error() ==
          ErrorCode::Unsupported);
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
