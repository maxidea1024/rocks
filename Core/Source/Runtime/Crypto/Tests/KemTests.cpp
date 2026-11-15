#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Hash.h>
#include <Crypto/Kdf.h>
#include <Crypto/Kem.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처:
// - ML-KEM-768: seed 00..3f(d ‖ z)와 캡슐화 난수 m = 80..9f를 kyber-py(FIPS 203 순수 Python)로
//   계산하고 Python cryptography(OpenSSL)로 공개키·캡슐 해제·암묵적 거부 값을 대조했다.
// - X-Wing: draft-connolly-cfrg-xwing-kem-11 부록 C 벡터 1(seed·eseed는 draft 값, 공유 비밀을 draft
//   값과 대조)과 libsodium 1.0.22-stable test/default/kem_xwing.c tv0(seed 0, 난수 64…)이다. 같은
//   구성을 Python에서 조립해(SHAKE256 키 확장, ML-KEM은 kyber-py, X25519는 cryptography, SHA3-256
//   combiner) 공개키·암호문을 계산했다. 공개키는 SHA3-256으로 비교한다.
// - transcript 묶기 파생은 Python hashlib.blake2b로 계산했다.
namespace {

struct HexBytes {
    uint8 bytes[1280];
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

bool KdfKeyEquals(const KdfKey& key, const char* hex) {
    uint8 exported[KdfKeyBytes];
    return hazmat::TryExportKey(key, Span<uint8>(exported)) &&
           Equals(Span<const uint8>(exported), hex);
}

bool SameKdfKey(const KdfKey& left, const KdfKey& right) {
    uint8 a[KdfKeyBytes];
    uint8 b[KdfKeyBytes];
    return hazmat::TryExportKey(left, Span<uint8>(a)) &&
           hazmat::TryExportKey(right, Span<uint8>(b)) &&
           ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(b));
}

bool PublicKeyHashEquals(const KemPublicKey& key, const char* hex) {
    return Equals(Sha3_256(key.View()).View(), hex);
}

constexpr const char* MlKemPublicKeySha3 =
    "a24e16d8f8f9383a95b77050f4d9fd2f5733eec1d63ef3c23ebf9918173669a7";
constexpr const char* MlKemCiphertext =
    "04c1e43bd82139e4600aa87fddc0793f7c147e0eb7dccc377f019d22c16ecdcbd22e05bb45e2fd38ca184e7bcc5b"
    "b504cdc13fc6d915cc029057311814bd916923de25330a22a53b6061dc17d892c437276108f1cb12bbbb37dce17e"
    "b4fecff68859850dbac4e0b6fc218a24caa82a4434b0886e63e824632aa32b0dce207636cbeb40a22b8432c094f0"
    "7f54af3793d6449322066890576e7d191347e34578f264c5406379e7c0c46dbc1857dcd07ced02dd14a5e882cb80"
    "73a0906ae293e7a639499321751b7d9b3de1e38eebf20fc0957fe72643106eb8a87b3862e680c235d99a7e432f7b"
    "ce65ccbed16973ed46f23c207e4096a563a945a867feaadb2a01d570d3bff7585c5f276a23e8f8995076904f89d0"
    "d044acf66a93550309326731ef4c0509047015e0068862de943e84cc9fab33850c86e295dca82d44b3ce236c186a"
    "ad8120d9134d4f6597e7278534f42339286987c06c4ba0d52c82e451041106ceddb3c6154d72a8214de6ab4e7af9"
    "6623684a36130d96498698724e4b5fe02f98e602e5d89f35335412bb7d3d9350bd61b99dd8f529b8f61e5d4f6cb7"
    "8733d3048e491f3296fb9972ca8af433870287f2ee52ccf487166d7edbbafe475bd30c1aa831e5c34975e9b227c5"
    "005cce483d9feac2b392efbe5e7c1b81d72d61ef6a9c55f37170c694b40bae5e6b89274b25613db3de07d426bfb2"
    "1db9a8f8cd2beb8756fdc9af863c904c2ff9453d21ce147c96c56e2e052dfaea6ad0ec6dc7eba35af185d2df99ee"
    "3d0ae22886d3135da04d90dc384857d128d38ea128f794d37d7c2dc74d5406455f04be7d75b5bcffc2b20fa34a5c"
    "b633c53874a3930abf387f16f5b0da53320dc5747956015f28a6ab54519d0528e4fe56515ace0109cbea117fefae"
    "7846e4c167734880ea6adb062abb7a530f5855133017fcdf4b25617881eb16538bf69f363d6f4bf9800b3687e0b1"
    "8f63489a5da21acb78258f36c30e76b6d268c77131df64e8625aaf8fd1d43de92a1fa8578fb22ccde5b97e840fe9"
    "7503c612199e7e99d7b4849035b3bd274228de0ad486fb788017e2124e34ebfb0885cfa1c5445d12f64da3694ca0"
    "eee205506950a08495f1be86ba0d756ce35c599e9507a3dbeea8403f78e71e1f0972a4b95519d34ab20b302a9390"
    "2139666a6000f4549ed2db07ae211733c9050a10153dd55f3b02e9c345ffb4a58c0bd2ca34e23c3d3dc45ecd8753"
    "21420f921527e7d1de27bf9525ae4c014ab52d147771be029d1dc18352d93f47c3ac7a2d2d6004d1a70da2207cb7"
    "de664166e2be537fdee44d9697f9a5ad80934cf189cd34c6160f05852de601efc8e745543369ca94d7d6f661de02"
    "0c32b63fffa0e5f85da87494f8286b49f03a3e3006df6a8b9cbc4dd7846809d24d23d0a1ae191cb8fd1946d71376"
    "2a14a2c4e68927774570c1f6e472047550ed8b83e5a77cc150d700f80433502daf12c33da4409e0f776add4b990d"
    "c6ed9a4202ea3eca5dec9fbe0281ca2e9e62c218d3dc5cd92ffc040a9ce0";
constexpr const char* MlKemShared =
    "ef91db44b6cd5b2c50f483481a3d6e2a08cc149764fcb8dc568851332da45ed9";
// 암호문 첫 byte를 바꿨을 때의 암묵적 거부 값이다.
constexpr const char* MlKemRejected =
    "016b585c8abc901fa45387c496d0ed74332aa06501ace2bb65ab9f8458a35bfc";

constexpr const char* XWingPublicKeySha3 =
    "11da78b4461862f89e2c545304ac972ac55ff353c82fc0a63e980212caf65e8e";
constexpr const char* XWingCiphertext =
    "d81018a94f8078e02105beaa814e003390befa4589bb614f77397af42d8e8150796f2c88a4efca81b8cf93c0ae37"
    "16c54ec1b045e3875f38c2dd12d7f717bd7fb701a9fecda5ed8b764c9a35d4a5c1d8930f6071f653eebb2d1afa77"
    "debb8302d16f17e0f5f3920a71a4d49beafa0e1c7e443f8abca64a65a9e81a97e7357bf902573363c0e1a12e5228"
    "036828e3f759121fada92441fe334e85d79347e470d2fed945541d832c54baaa3cb7526c3853954db4f73547cc7c"
    "27fd38398bfa7704952cb841e38b270e4db7435f0ee22f57d7ad3270bd0c88e71b4b864cf2277c65daa10a6dad4c"
    "7abecd95cc4ebec39c08404b522e4ecc1545713f76bebd3b5a0f2feb3461936065dbd13f6a1f61e1b142a2af2e5a"
    "482ba2c50cf0317049c0b3bfd6d5e9240eba9111d2030fdea17e33b6524020d30b0c4f8069285f3a6ca267d287d0"
    "1e827d8422bf5426e11688bfc73756af1841b1c87e126cb50c914b5b2b8673488ad3b074cad77a3840eb12dd688f"
    "313ee1e9ff8c479a678f276356fc9d65e1d5b4c1e9855b4175db144f7767c12061769190fe6b5e51563b91f94d13"
    "1a2b796bd2980ed0dab4ae7a7110e920007a757158a5eb8662cbf89ddffe9d8196821313cdc00108853fc4746b11"
    "1d5b56da638d8ed2973918960f5dfe93ead3ae521e957cec3c8d843e8fce234c70ad055177f235439d6098bdd771"
    "b1cfcfadaab4f50a7378185c62409f383c8ff658c2a2af66498cfd81e962766ac6b774e88424fb4f331837d0a285"
    "02708477caf8780a156d723f68fca791e1cd2397bfc2b24c77c765d9b2af36f732d52107517efd8157b283b440a6"
    "13f756c364ca108971a8878199a93f260baec3e850033cc032c2e53f823576affb4d3b116e2d16049152c35aaa26"
    "3ab376f0ad5ede6a749607a283e3016e62191c0e8fde33e718cd989591c9a205d608d99fcb8a7471603d716cb01b"
    "56328d7d880aec2851f4e6d8b5016c25647e9026ebb441543e8012dbfcf078d4012b8c39184dd64f3821b4774ae4"
    "e36365f8baf2bd1f6667c017a1e65ff8a1554458fb3f367c02721752bfa56fc7fd566ae95ffb208f919ef12f4cf8"
    "a2fdd141a8df559bddb7b8d1f04ee6d4cf7805d142989caf216dfae985faaab9974f6d9f8aa1129084db8db912b1"
    "655f595ffbaa66491ab4655fd734cfd4bb0c0289d4bcc8fc5e9943b351cb147c8db059a24004d1c3e3bb4c14a881"
    "e5101acb736c65c5d579acb67ee85a560277b43338fe79d34b772c5da001da3b5a3383dd81319a0b4542e6d7e46e"
    "ed5314cc70eb231de27b6e760db598ba19995cf69be0e4458e35f3f274aca2455d43fe3344e183c6dc47c857dbe9"
    "907b41e41006d91b25adcafc098fe66f7554be8dad493c4f4b1dbf7a51464139db474afab5572f92a2232b59be56"
    "a72c0505149dae5cde1e602877037de7802b5f6fa47a4c9a3e52d6ca15339920254e9ffb53c7b834cc0288ed9905"
    "a1841e9390ea94a8898bd4c6b6d6027e4d43c7867242515bbeefe12340fc6b3d57762f8badb69433f9c6d060f85f"
    "5e5c6b6803a816d141c075f63541ad10";
constexpr const char* XWingShared =
    "e5ba94031ea6efd69c09c254f6d9783136ba6037e2d4c43bcccf19d6f3f4343a";

// draft-11 부록 C 벡터 1(seed 7f9c…, eseed 3cb1…)이다.
constexpr const char* DraftPublicKeySha3 =
    "5121745904643ad9dfacca7869292c19a8a69533b53e60666b7db910b4ad6367";
constexpr const char* DraftCiphertext =
    "b83aa828d4d62b9a83ceffe1d3d3bb1ef31264643c070c5798927e41fb07914a273f8f96e7826cd5375a283d7da8"
    "85304c5de0516a0f0654243dc5b97f8bfeb831f68251219aabdd723bc6512041acbaef8af44265524942b902e68f"
    "fd23221cda70b1b55d776a92d1143ea3a0c475f63ee6890157c7116dae3f62bf72f60acd2bb8cc31ce2ba0de364f"
    "52b8ed38c79d719715963a5dd3842d8e8b43ab704e4759b5327bf027c63c8fa857c4908d5a8a7b88ac7f2be394d9"
    "3c3706ddd4e698cc6ce370101f4d0213254238b4a2e8821b6e414a1cf20f6c1244b699046f5a01caa0a1a5551630"
    "0b40d2048c77cc73afba79afeea9d2c0118bdf2adb8870dc328c5516cc45b1a2058141039e2c90a110a9e16b318d"
    "fb53bd49a126d6b73f215787517b8917cc01cabd107d06859854ee8b4f9861c226d3764c87339ab16c3667d2f493"
    "84e55456dd40414b70a6af841585f4c90c68725d57704ee8ee7ce6e2f9be582dbee985e038ffc346ebfb4e22158b"
    "6c84374a9ab4a44e1f91de5aac5197f89bc5e5442f51f9a5937b102ba3beaebf6e1c58380a4a5fedce4a4e5026f8"
    "8f528f59ffd2db41752b3a3d90efabe463899b7d40870c530c8841e8712b733668ed033adbfafb2d49d37a44d406"
    "4e5863eb0af0a08d47b3cc888373bc05f7a33b841bc2587c57eb69554e8a3767b7506917b6b70498727f16eac1a3"
    "6ec8d8cfaf751549f2277db277e8a55a9a5106b23a0206b4721fa9b3048552c5bd5b594d6e247f38c18c591aea7f"
    "56249c72ce7b117afcc3a8621582f9cf71787e183dee09367976e98409ad9217a497df888042384d7707a6b78f5f"
    "7fb8409e3b535175373461b776002d799cbad62860be70573ecbe13b246e0da7e93a52168e0fb6a9756b895ef7f0"
    "147a0dc81bfa644b088a9228160c0f9acf1379a2941cd28c06ebc80e44e17aa2f8177010afd78a97ce0868d1629e"
    "bb294c5151812c583daeb88685220f4da9118112e07041fcc24d5564a99fdbde28869fe0722387d7a9a4d16e1cc8"
    "555917e09944aa5ebaaaec2cf62693afad42a3f518fce67d273cc6c9fb5472b380e8573ec7de06a3ba2fd5f931d7"
    "25b493026cb0acbd3fe62d00e4c790d965d7a03a3c0b4222ba8c2a9a16e2ac658f572ae0e746eafc4feba023576f"
    "08942278a041fb82a70a595d5bacbf297ce2029898a71e5c3b0d1c6228b485b1ade509b35fbca7eca97b2132e7cb"
    "6bc465375146b7dceac969308ac0c2ac89e7863eb8943015b24314cafb9c7c0e85fe543d56658c213632599efabf"
    "c1ec49dd8c88547bb2cc40c9d38cbd3099b4547840560531d0188cd1e9c23a0ebee0a03d5577d66b1d2bcb4baaf2"
    "1cc7fef1e03806ca96299df0dfbc56e1b2b43e4fc20c37f834c4af62127e7dae86c3c25a2f696ac8b589dec71d59"
    "5bfbe94b5ed4bc07d800b330796fda89edb77be0294136139354eb8cd37591578f9c600dd9be8ec6219fdd507adf"
    "3397ed4d68707b8d13b24ce4cd8fb22851bfe9d632407f31ed6f7cb1600de56f17576740ce2a32fc5145030145cf"
    "b97e63e0e41d354274a079d3e6fb2e15";
constexpr const char* DraftShared =
    "d2df0522128f09dd8e2c92b1e905c793d8f57a54c3da25861f10bf4ca613e384";

// X25519 small-order u 값이다: 0, 1(위수 4), 위수 8 점, 최상위 bit만 켠 0.
constexpr const char* SmallOrderX25519[] = {
    "0000000000000000000000000000000000000000000000000000000000000000",
    "0100000000000000000000000000000000000000000000000000000000000000",
    "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
    "0000000000000000000000000000000000000000000000000000000000000080",
};

constexpr KdfContext TestContext("KEM test");

} // namespace

TEST(CryptoKemInfo) {
    auto xwing = TryGetKemInfo(KemAlgorithm::XWing);
    auto mlkem = TryGetKemInfo(KemAlgorithm::MlKem768);
    REQUIRE(xwing && mlkem);
    CHECK(xwing.Value().publicKeyBytes == 1216 && xwing.Value().ciphertextBytes == 1120 &&
          xwing.Value().seedBytes == 32);
    CHECK(mlkem.Value().publicKeyBytes == 1184 && mlkem.Value().ciphertextBytes == 1088 &&
          mlkem.Value().seedBytes == 64);
    CHECK(GetPreferredKem() == KemAlgorithm::XWing);
    CHECK(TryGetKemInfo(static_cast<KemAlgorithm>(0)).Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetKemInfo(static_cast<KemAlgorithm>(3)).Error() == ErrorCode::InvalidArgument);
}

TEST(CryptoMlKem768KnownAnswers) {
    uint8 seed[64];
    for (usize i = 0; i < sizeof(seed); ++i) {
        seed[i] = static_cast<uint8>(i);
    }

    auto key = KemKeyPair::TryFromSeed(KemAlgorithm::MlKem768, Span<const uint8>(seed));
    REQUIRE(key);
    CHECK(key.Value().Algorithm() == KemAlgorithm::MlKem768);
    CHECK(key.Value().PublicKey().View().Size() == 1184);
    CHECK(PublicKeyHashEquals(key.Value().PublicKey(), MlKemPublicKeySha3));

    const HexBytes ciphertext(MlKemCiphertext);
    REQUIRE(ciphertext.size == 1088);
    auto shared = TryDecapsulate(key.Value(), ciphertext.View());
    REQUIRE(shared);
    CHECK(KdfKeyEquals(shared.Value(), MlKemShared));

    // 암호문이 바뀌면 오류 없이 다른(정해진) 공유 비밀이 나온다(암묵적 거부).
    uint8 tampered[1088];
    memcpy(tampered, ciphertext.bytes, sizeof(tampered));
    tampered[0] ^= 0x01;
    auto rejected = TryDecapsulate(key.Value(), Span<const uint8>(tampered));
    REQUIRE(rejected);
    CHECK(KdfKeyEquals(rejected.Value(), MlKemRejected));

    uint8 exported[64];
    REQUIRE(hazmat::TryExportSeed(key.Value(), Span<uint8>(exported)));
    CHECK(ConstantTimeEquals(Span<const uint8>(exported), Span<const uint8>(seed)));
}

TEST(CryptoXWingKnownAnswers) {
    const uint8 seed[32] = {};
    auto key = KemKeyPair::TryFromSeed(KemAlgorithm::XWing, Span<const uint8>(seed));
    REQUIRE(key);
    CHECK(key.Value().Algorithm() == KemAlgorithm::XWing);
    CHECK(PublicKeyHashEquals(key.Value().PublicKey(), XWingPublicKeySha3));
    CHECK(Equals(key.Value().PublicKey().View().Left(32),
                 "3d209f716752f6408e7f89bceef97ac388530045377927644ef046c0a7cae978"));

    const HexBytes ciphertext(XWingCiphertext);
    REQUIRE(ciphertext.size == 1120);
    auto shared = TryDecapsulate(key.Value(), ciphertext.View());
    REQUIRE(shared);
    CHECK(KdfKeyEquals(shared.Value(), XWingShared));

    // ML-KEM 부분이 바뀌면 정해진 다른 공유 비밀(암묵적 거부)이고, X25519 부분이 small-order 점이면
    // 거부다.
    uint8 tampered[1120];
    memcpy(tampered, ciphertext.bytes, sizeof(tampered));
    tampered[0] ^= 0x01;
    auto other = TryDecapsulate(key.Value(), Span<const uint8>(tampered));
    REQUIRE(other);
    CHECK(KdfKeyEquals(other.Value(),
                       "cdaeaabde37f873928d46a6ce18af3ea8e2cb1a675650a684ff33506cea7e75b"));
    for (const char* hex : SmallOrderX25519) {
        const HexBytes point(hex);
        REQUIRE(point.size == 32);
        memcpy(tampered, ciphertext.bytes, sizeof(tampered));
        memcpy(tampered + 1088, point.bytes, 32);
        CHECK(TryDecapsulate(key.Value(), Span<const uint8>(tampered)).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    // draft-11 부록 C 벡터 1이다.
    const HexBytes draftSeed("7f9c2ba4e88f827d616045507605853ed73b8093f6efbc88eb1a6eacfa66ef26");
    auto draft = KemKeyPair::TryFromSeed(KemAlgorithm::XWing, draftSeed.View());
    REQUIRE(draft);
    CHECK(PublicKeyHashEquals(draft.Value().PublicKey(), DraftPublicKeySha3));
    const HexBytes draftCiphertext(DraftCiphertext);
    REQUIRE(draftCiphertext.size == 1120);
    auto draftShared = TryDecapsulate(draft.Value(), draftCiphertext.View());
    REQUIRE(draftShared);
    CHECK(KdfKeyEquals(draftShared.Value(), DraftShared));
}

TEST(CryptoKemRoundTrip) {
    const KemAlgorithm algorithms[] = {KemAlgorithm::XWing, KemAlgorithm::MlKem768};
    for (KemAlgorithm algorithm : algorithms) {
        const KemInfo info = TryGetKemInfo(algorithm).Value();
        auto recipient = KemKeyPair::TryGenerate(algorithm);
        REQUIRE(recipient);
        CHECK(recipient.Value().PublicKey().Algorithm() == algorithm);

        // 공개키는 값으로 옮겨 다닌다(직렬화 뒤 다시 만든다).
        auto publicKey =
            KemPublicKey::TryFromBytes(algorithm, recipient.Value().PublicKey().View());
        REQUIRE(publicKey);
        CHECK(publicKey.Value() == recipient.Value().PublicKey());

        uint8 ciphertext[KemMaxCiphertextBytes];
        const Span<uint8> ciphertextView = Span<uint8>(ciphertext).Left(info.ciphertextBytes);
        auto sent = TryEncapsulate(publicKey.Value(), ciphertextView);
        REQUIRE(sent);
        auto received = TryDecapsulate(recipient.Value(), ciphertextView);
        REQUIRE(received);
        CHECK(SameKdfKey(sent.Value(), received.Value()));

        // 같은 공개키라도 캡슐화마다 암호문·공유 비밀이 다르다.
        uint8 again[KemMaxCiphertextBytes];
        auto second =
            TryEncapsulate(publicKey.Value(), Span<uint8>(again).Left(info.ciphertextBytes));
        REQUIRE(second);
        CHECK(!ConstantTimeEquals(Span<const uint8>(again).Left(info.ciphertextBytes),
                                  Span<const uint8>(ciphertext).Left(info.ciphertextBytes)));
        CHECK(!SameKdfKey(second.Value(), sent.Value()));

        // 공유 비밀에서 뽑은 AEAD 키가 양쪽에서 같다.
        auto sendKey = TryDeriveAeadKey(sent.Value(), 1, TestContext, GetPreferredAead());
        auto receiveKey = TryDeriveAeadKey(received.Value(), 1, TestContext, GetPreferredAead());
        REQUIRE(sendKey && receiveKey);
        uint8 sealed[64];
        auto written = TrySeal(sendKey.Value(), Span<const uint8>(),
                               Span<const uint8>(reinterpret_cast<const uint8*>("kem"), 3),
                               Span<uint8>(sealed));
        REQUIRE(written);
        uint8 opened[16];
        CHECK(TryOpen(receiveKey.Value(), Span<const uint8>(),
                      Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened)));

        // seed를 내보내 다시 만든 키 쌍은 같은 공개키이고 같은 암호문을 연다.
        uint8 seed[KemMaxSeedBytes];
        const Span<uint8> seedView = Span<uint8>(seed).Left(info.seedBytes);
        REQUIRE(hazmat::TryExportSeed(recipient.Value(), seedView));
        auto restored = KemKeyPair::TryFromSeed(algorithm, seedView);
        SecureZero(seedView);
        REQUIRE(restored);
        CHECK(restored.Value().PublicKey() == recipient.Value().PublicKey());
        auto restoredShared = TryDecapsulate(restored.Value(), ciphertextView);
        REQUIRE(restoredShared);
        CHECK(SameKdfKey(restoredShared.Value(), sent.Value()));
    }
}

TEST(CryptoKemRejectsMisuse) {
    // 알고리즘·길이 검사다.
    uint8 bytes[KemMaxPublicKeyBytes + 1] = {};
    CHECK(KemPublicKey::TryFromBytes(static_cast<KemAlgorithm>(0),
                                     Span<const uint8>(bytes).Left(1216))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(KemPublicKey::TryFromBytes(KemAlgorithm::XWing, Span<const uint8>(bytes).Left(1184))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(KemPublicKey::TryFromBytes(KemAlgorithm::MlKem768, Span<const uint8>(bytes).Left(1216))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(KemKeyPair::TryGenerate(static_cast<KemAlgorithm>(9)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(KemKeyPair::TryFromSeed(KemAlgorithm::XWing, Span<const uint8>(bytes).Left(64)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(KemKeyPair::TryFromSeed(KemAlgorithm::MlKem768, Span<const uint8>(bytes).Left(32))
              .Error() == ErrorCode::InvalidArgument);

    // 빈 공개키·키 쌍과 암호문 길이다.
    uint8 ciphertext[KemMaxCiphertextBytes + 1] = {};
    CHECK(TryEncapsulate(KemPublicKey(), Span<uint8>(ciphertext).Left(1120)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryDecapsulate(KemKeyPair(), Span<const uint8>(ciphertext).Left(1120)).Error() ==
          ErrorCode::StaleHandle);
    auto key = KemKeyPair::TryGenerate(KemAlgorithm::XWing);
    REQUIRE(key);
    CHECK(TryEncapsulate(key.Value().PublicKey(), Span<uint8>(ciphertext).Left(1119)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryEncapsulate(key.Value().PublicKey(), Span<uint8>(ciphertext)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryDecapsulate(key.Value(), Span<const uint8>(ciphertext).Left(1119)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryDecapsulate(key.Value(), Span<const uint8>(ciphertext)).Error() ==
          ErrorCode::AuthenticationFailed);
    uint8 seed[KemMaxSeedBytes];
    CHECK(hazmat::TryExportSeed(KemKeyPair(), Span<uint8>(seed).Left(32)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(hazmat::TryExportSeed(key.Value(), Span<uint8>(seed)).Error() ==
          ErrorCode::InvalidArgument);

    // 올바르지 않은 공개키는 캡슐화하지 않고 암호문을 지운다.
    const auto expectRejectedKey = [&](KemAlgorithm algorithm, Span<const uint8> publicKey) {
        auto bad = KemPublicKey::TryFromBytes(algorithm, publicKey);
        REQUIRE(bad);
        const usize ciphertextBytes = TryGetKemInfo(algorithm).Value().ciphertextBytes;
        memset(ciphertext, 0x5A, sizeof(ciphertext));
        CHECK(TryEncapsulate(bad.Value(), Span<uint8>(ciphertext).Left(ciphertextBytes)).Error() ==
              ErrorCode::AuthenticationFailed);
        bool cleared = true;
        for (usize i = 0; i < ciphertextBytes; ++i) {
            cleared = cleared && ciphertext[i] == 0;
        }

        CHECK(cleared);
    };
    const auto expectAcceptedKey = [&](KemAlgorithm algorithm, Span<const uint8> publicKey) {
        auto good = KemPublicKey::TryFromBytes(algorithm, publicKey);
        REQUIRE(good);
        const usize ciphertextBytes = TryGetKemInfo(algorithm).Value().ciphertextBytes;
        CHECK(TryEncapsulate(good.Value(), Span<uint8>(ciphertext).Left(ciphertextBytes)));
    };

    // X-Wing의 X25519 부분이 small-order 점이다.
    for (const char* hex : SmallOrderX25519) {
        const HexBytes point(hex);
        REQUIRE(point.size == 32);
        memcpy(bytes, key.Value().PublicKey().View().Data(), 1216);
        memcpy(bytes + 1184, point.bytes, 32);
        expectRejectedKey(KemAlgorithm::XWing, Span<const uint8>(bytes).Left(1216));
    }

    // ML-KEM 계수 경계(12 bit little-endian 묶음): 첫 계수 3328은 받고 3329(q)는 거부하며, 둘째
    // 계수 3329도 거부한다. X-Wing의 ML-KEM 부분에도 같은 검사가 걸린다.
    auto mlkem = KemKeyPair::TryGenerate(KemAlgorithm::MlKem768);
    REQUIRE(mlkem);
    struct Coefficients {
        uint8 bytes[3];
        bool accepted;
    };

    const Coefficients boundaries[] = {
        {{0x00, 0x8d, 0xa1}, true},  // 계수0 = 3328, 계수1 = 2584
        {{0x01, 0x8d, 0xa1}, false}, // 계수0 = 3329
        {{0x29, 0x1a, 0xd0}, false}, // 계수0 = 2601, 계수1 = 3329
    };
    for (const Coefficients& item : boundaries) {
        memcpy(bytes, mlkem.Value().PublicKey().View().Data(), 1184);
        memcpy(bytes, item.bytes, 3);
        if (item.accepted) {
            expectAcceptedKey(KemAlgorithm::MlKem768, Span<const uint8>(bytes).Left(1184));
        } else {
            expectRejectedKey(KemAlgorithm::MlKem768, Span<const uint8>(bytes).Left(1184));
        }
    }

    memcpy(bytes, key.Value().PublicKey().View().Data(), 1216);
    memcpy(bytes, boundaries[1].bytes, 3);
    expectRejectedKey(KemAlgorithm::XWing, Span<const uint8>(bytes).Left(1216));

    // 키 쌍은 키 저장소에 seed만 둔다(ML-KEM도 작은 슬롯 하나, 확장 비밀키는 캡슐 해제 때만 잠깐
    // 쓴다). 소멸·이동 때 돌려준다.
    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        auto generated = KemKeyPair::TryGenerate(KemAlgorithm::MlKem768);
        REQUIRE(generated);
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
        CHECK(GetSecretMemoryStatus().arenaUsedBytes - used <= 64);
        KemKeyPair moved = Move(generated.Value());
        CHECK(generated.Value().IsEmpty());
        CHECK(TryDecapsulate(generated.Value(), Span<const uint8>(ciphertext).Left(1088)).Error() ==
              ErrorCode::StaleHandle);
        CHECK(!moved.IsEmpty());
    }

    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoKdfBoundDerivation) {
    // transcript 묶기 파생은 BLAKE2b(key = master, salt = LE64(id) ‖ 0⁸, personal = context ‖
    // "RkKdfBnd", 입력 = 조각마다 LE64(길이) ‖ byte)다. 조각 경계가 다르면 다른 키다.
    uint8 masterBytes[32];
    for (usize i = 0; i < sizeof(masterBytes); ++i) {
        masterBytes[i] = static_cast<uint8>(i);
    }

    auto master = KdfKey::TryImport(Span<const uint8>(masterBytes));
    REQUIRE(master);
    const auto text = [](const char* value) {
        return Span<const uint8>(reinterpret_cast<const uint8*>(value), strlen(value));
    };
    const Span<const uint8> pieces[] = {text("pk-bytes"), Span<const uint8>(), text("ciphertext")};
    auto bound =
        TryDeriveBoundKdfKey(master.Value(), 7, TestContext, Span<const Span<const uint8>>(pieces));
    REQUIRE(bound);
    CHECK(KdfKeyEquals(bound.Value(),
                       "d8f3d6f77c97842289acd88c3439fdf4f3b57db1af7fc4660ae4820dfbbc06f7"));

    const Span<const uint8> shifted[] = {text("pk-byte"), text("s"), text("ciphertext")};
    auto other = TryDeriveBoundKdfKey(master.Value(), 7, TestContext,
                                      Span<const Span<const uint8>>(shifted));
    REQUIRE(other);
    CHECK(KdfKeyEquals(other.Value(),
                       "c639b18621acc9e6086943cbaa3f454df42de60d6d4aae57351f8acaf7225940"));

    auto empty =
        TryDeriveBoundKdfKey(master.Value(), 7, TestContext, Span<const Span<const uint8>>());
    REQUIRE(empty);
    CHECK(KdfKeyEquals(empty.Value(),
                       "0d5d2e69766bb9837c8f4c1f0b1586590974c08647e3990cc703f96c5548b659"));

    // 같은 (id, context)의 TryDeriveKdfKey와 다르다(타입 tag가 다르다).
    auto plain = TryDeriveKdfKey(master.Value(), 7, TestContext);
    REQUIRE(plain);
    CHECK(!SameKdfKey(plain.Value(), empty.Value()));
    CHECK(TryDeriveBoundKdfKey(KdfKey(), 7, TestContext, Span<const Span<const uint8>>()).Error() ==
          ErrorCode::StaleHandle);
}
