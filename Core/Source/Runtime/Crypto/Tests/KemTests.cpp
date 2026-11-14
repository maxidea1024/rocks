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
// - X-Wing: libsodium 1.0.22-stable test/default/kem_xwing.c tv0(seed 0, 난수 64…)이다. 같은 구성을
//   Python에서 조립해(SHAKE256 키 확장, ML-KEM은 kyber-py, X25519는 cryptography, SHA3-256
//   combiner) 표의 공개키·암호문 앞부분과 공유 비밀을 재현했다. 공개키는 SHA3-256으로 비교한다.
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

    // ML-KEM 부분이 바뀌면 다른 공유 비밀(암묵적 거부), X25519 부분이 small-order 점이면 거부다.
    uint8 tampered[1120];
    memcpy(tampered, ciphertext.bytes, sizeof(tampered));
    tampered[0] ^= 0x01;
    auto other = TryDecapsulate(key.Value(), Span<const uint8>(tampered));
    REQUIRE(other);
    CHECK(!KdfKeyEquals(other.Value(), XWingShared));
    memcpy(tampered, ciphertext.bytes, sizeof(tampered));
    memset(tampered + 1088, 0, 32);
    CHECK(TryDecapsulate(key.Value(), Span<const uint8>(tampered)).Error() ==
          ErrorCode::AuthenticationFailed);
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

    // 올바르지 않은 공개키: X-Wing의 X25519 부분이 0(small-order), ML-KEM 계수가 q 이상(첫 계수
    // 0xfff). 캡슐화하지 않고 암호문을 지운다.
    memcpy(bytes, key.Value().PublicKey().View().Data(), 1216);
    memset(bytes + 1184, 0, 32);
    auto badXWing =
        KemPublicKey::TryFromBytes(KemAlgorithm::XWing, Span<const uint8>(bytes).Left(1216));
    REQUIRE(badXWing);
    memset(ciphertext, 0x5A, sizeof(ciphertext));
    CHECK(TryEncapsulate(badXWing.Value(), Span<uint8>(ciphertext).Left(1120)).Error() ==
          ErrorCode::AuthenticationFailed);
    bool cleared = true;
    for (usize i = 0; i < 1120; ++i) {
        cleared = cleared && ciphertext[i] == 0;
    }

    CHECK(cleared);
    auto mlkem = KemKeyPair::TryGenerate(KemAlgorithm::MlKem768);
    REQUIRE(mlkem);
    memcpy(bytes, mlkem.Value().PublicKey().View().Data(), 1184);
    bytes[0] = 0xFF;
    bytes[1] |= 0x0F;
    auto badMlKem =
        KemPublicKey::TryFromBytes(KemAlgorithm::MlKem768, Span<const uint8>(bytes).Left(1184));
    REQUIRE(badMlKem);
    CHECK(TryEncapsulate(badMlKem.Value(), Span<uint8>(ciphertext).Left(1088)).Error() ==
          ErrorCode::AuthenticationFailed);

    // 키 쌍은 키 저장소를 쓰고 소멸·이동 때 돌려준다.
    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        auto generated = KemKeyPair::TryGenerate(KemAlgorithm::MlKem768);
        REQUIRE(generated);
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
        KemKeyPair moved = Move(generated.Value());
        CHECK(generated.Value().IsEmpty());
        CHECK(TryDecapsulate(generated.Value(), Span<const uint8>(ciphertext).Left(1088)).Error() ==
              ErrorCode::StaleHandle);
        CHECK(!moved.IsEmpty());
    }

    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}
