#include <Crypto/Bytes.h>
#include <Crypto/Secret.h>
#include <Crypto/Signature.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: RFC 8032 7.1 TEST 1–3(Ed25519)과 7.3 TEST abc(Ed25519ph)다. Python
// cryptography(OpenSSL)와 pycryptodome으로 다시 계산해 같음을 확인했다.
namespace {

struct HexBytes {
    uint8 bytes[128];
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

Ed25519PublicKey PublicKeyFromHex(const char* hex) {
    const HexBytes bytes(hex);
    auto key = Ed25519PublicKey::TryFromBytes(bytes.View());
    return key ? key.Value() : Ed25519PublicKey();
}

struct Rfc8032Case {
    const char* seed;
    const char* publicKey;
    const char* message;
    const char* signature;
};

const Rfc8032Case Rfc8032Cases[] = {
    {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
     "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
     "5fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
    {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
     "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
     "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
    {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
     "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
     "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac"
     "18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
};

constexpr const char* PhSeed = "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42";
constexpr const char* PhPublicKey =
    "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf";
constexpr const char* PhSignature =
    "98a70222f0b8121aa9d30f813d683f809e462b469c7ff87639499bb94e6dae41"
    "31f85042463c2a355a2003d062adf5aaa10b8c61e636062aaad11c2a26083406";

Ed25519SigningKey KeyFromSeed(const char* hex) {
    const HexBytes seed(hex);
    auto key = Ed25519SigningKey::TryFromSeed(seed.View());
    return key ? Move(key.Value()) : Ed25519SigningKey();
}

} // namespace

TEST(CryptoEd25519Rfc8032) {
    for (const Rfc8032Case& item : Rfc8032Cases) {
        const HexBytes seed(item.seed);
        const HexBytes message(item.message);
        REQUIRE(seed.size == 32 && message.size != MaxSize);
        auto key = Ed25519SigningKey::TryFromSeed(seed.View());
        REQUIRE(key);
        CHECK(!key.Value().IsEmpty());
        const Ed25519PublicKey publicKey = key.Value().PublicKey();
        CHECK(Equals(publicKey.View(), item.publicKey));
        CHECK(publicKey == PublicKeyFromHex(item.publicKey));

        auto signature = TrySign(key.Value(), message.View());
        REQUIRE(signature);
        CHECK(Equals(signature.Value().View(), item.signature));
        CHECK(TryVerify(publicKey, message.View(), signature.Value().View()));

        // 같은 키·메시지는 늘 같은 서명이다(결정적).
        auto again = TrySign(key.Value(), message.View());
        REQUIRE(again);
        CHECK(ConstantTimeEquals(again.Value().View(), signature.Value().View()));

        uint8 exported[32];
        REQUIRE(hazmat::TryExportSeed(key.Value(), Span<uint8>(exported)));
        CHECK(ConstantTimeEquals(Span<const uint8>(exported), seed.View()));
    }
}

TEST(CryptoEd25519phRfc8032) {
    const Ed25519SigningKey key = KeyFromSeed(PhSeed);
    REQUIRE(!key.IsEmpty());
    const Ed25519PublicKey publicKey = key.PublicKey();
    CHECK(Equals(publicKey.View(), PhPublicKey));

    // 한 번에 넣든 조각으로 넣든 같은 서명이다.
    auto signer = Ed25519phSigner::TryCreate();
    REQUIRE(signer);
    REQUIRE(signer.Value().TryUpdate(Text("abc")));
    auto signature = signer.Value().TryFinish(key);
    REQUIRE(signature);
    CHECK(Equals(signature.Value().View(), PhSignature));

    signer.Value().Reset();
    const Span<const uint8> pieces[] = {Text("a"), Span<const uint8>(), Text("bc")};
    REQUIRE(signer.Value().TryUpdate(Span<const Span<const uint8>>(pieces)));
    auto split = signer.Value().TryFinish(key);
    REQUIRE(split);
    CHECK(Equals(split.Value().View(), PhSignature));

    auto verifier = Ed25519phVerifier::TryCreate();
    REQUIRE(verifier);
    REQUIRE(verifier.Value().TryUpdate(Text("ab")));
    REQUIRE(verifier.Value().TryUpdate(Text("c")));
    CHECK(verifier.Value().TryFinish(publicKey, signature.Value().View()));

    // Ed25519ph와 순수 Ed25519는 서로의 서명을 받지 않는다.
    CHECK(TryVerify(publicKey, Text("abc"), signature.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);
    auto pure = TrySign(key, Text("abc"));
    REQUIRE(pure);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("abc")));
    CHECK(verifier.Value().TryFinish(publicKey, pure.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);

    // 다른 메시지는 받지 않는다.
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("abd")));
    CHECK(verifier.Value().TryFinish(publicKey, signature.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoEd25519RejectsForgeries) {
    const Rfc8032Case& item = Rfc8032Cases[1];
    const Ed25519PublicKey publicKey = PublicKeyFromHex(item.publicKey);
    const HexBytes message(item.message);
    const HexBytes signature(item.signature);
    REQUIRE(signature.size == 64);
    CHECK(TryVerify(publicKey, message.View(), signature.View()));

    // R·S 어느 bit를 바꿔도, 메시지·키가 달라도 실패한다.
    const usize positions[] = {0, 31, 32, 63};
    for (usize position : positions) {
        uint8 tampered[64];
        memcpy(tampered, signature.bytes, sizeof(tampered));
        tampered[position] ^= 0x01;
        CHECK(TryVerify(publicKey, message.View(), Span<const uint8>(tampered)).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    CHECK(TryVerify(publicKey, Text("s"), signature.View()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryVerify(PublicKeyFromHex(Rfc8032Cases[0].publicKey), message.View(), signature.View())
              .Error() == ErrorCode::AuthenticationFailed);

    // 서명 길이가 틀리면 잘린 서명으로 본다.
    CHECK(TryVerify(publicKey, message.View(), signature.View().Left(63)).Error() ==
          ErrorCode::AuthenticationFailed);
    uint8 longer[65] = {};
    memcpy(longer, signature.bytes, 64);
    CHECK(TryVerify(publicKey, message.View(), Span<const uint8>(longer)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryVerify(publicKey, message.View(), Span<const uint8>()).Error() ==
          ErrorCode::AuthenticationFailed);

    // 비정규 S(RFC 8032 TEST 1 서명의 S + L): 같은 점을 뜻하지만 libsodium은 거부한다(서명 가변성).
    const Rfc8032Case& empty = Rfc8032Cases[0];
    CHECK(TryVerify(PublicKeyFromHex(empty.publicKey), Span<const uint8>(),
                    HexBytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                             "4c8c7872aa064e049dbb3013fbf29380d25bf5f0595bbe24655141438e7a101b")
                        .View())
              .Error() == ErrorCode::AuthenticationFailed);

    // small-order 공개키(항등원)와 R = B, S = 1: 허용적인 cofactorless 검증은 어떤 메시지든
    // 받아들이지만 libsodium은 small-order 공개키를 거부한다.
    const Ed25519PublicKey identity =
        PublicKeyFromHex("0100000000000000000000000000000000000000000000000000000000000000");
    const HexBytes forged("5866666666666666666666666666666666666666666666666666666666666666"
                          "0100000000000000000000000000000000000000000000000000000000000000");
    CHECK(TryVerify(identity, Text("any message"), forged.View()).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoEd25519KeyLifetimeAndMisuse) {
    // 빈 키다.
    Ed25519SigningKey empty;
    CHECK(empty.IsEmpty());
    CHECK(empty.PublicKey() == Ed25519PublicKey());
    CHECK(TrySign(empty, Text("x")).Error() == ErrorCode::StaleHandle);
    uint8 seed[33] = {};
    CHECK(hazmat::TryExportSeed(empty, Span<uint8>(seed).Left(32)).Error() ==
          ErrorCode::StaleHandle);

    // 길이 검사다.
    CHECK(Ed25519SigningKey::TryFromSeed(Span<const uint8>(seed).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Ed25519SigningKey::TryFromSeed(Span<const uint8>(seed)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Ed25519PublicKey::TryFromBytes(Span<const uint8>(seed).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(Ed25519PublicKey::TryFromBytes(Span<const uint8>(seed)).Error() ==
          ErrorCode::InvalidArgument);

    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        auto generated = Ed25519SigningKey::TryGenerate();
        auto other = Ed25519SigningKey::TryGenerate();
        REQUIRE(generated && other);
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
        CHECK(!(generated.Value().PublicKey() == other.Value().PublicKey()));

        auto signature = TrySign(generated.Value(), Text("payload"));
        REQUIRE(signature);
        CHECK(TryVerify(generated.Value().PublicKey(), Text("payload"), signature.Value().View()));
        CHECK(TryVerify(other.Value().PublicKey(), Text("payload"), signature.Value().View())
                  .Error() == ErrorCode::AuthenticationFailed);

        // 내보낸 seed로 다시 만든 키는 같은 공개키·서명이다.
        REQUIRE(hazmat::TryExportSeed(generated.Value(), Span<uint8>(seed).Left(32)));
        CHECK(hazmat::TryExportSeed(generated.Value(), Span<uint8>(seed)).Error() ==
              ErrorCode::InvalidArgument);
        auto restored = Ed25519SigningKey::TryFromSeed(Span<const uint8>(seed).Left(32));
        SecureZero(Span<uint8>(seed));
        REQUIRE(restored);
        CHECK(restored.Value().PublicKey() == generated.Value().PublicKey());
        auto restoredSignature = TrySign(restored.Value(), Text("payload"));
        REQUIRE(restoredSignature);
        CHECK(ConstantTimeEquals(restoredSignature.Value().View(), signature.Value().View()));

        // 이동한 키는 같은 서명을 내고 원본은 비어 StaleHandle이다.
        Ed25519SigningKey moved = Move(generated.Value());
        CHECK(generated.Value().IsEmpty());
        CHECK(TrySign(generated.Value(), Text("payload")).Error() == ErrorCode::StaleHandle);
        auto movedSignature = TrySign(moved, Text("payload"));
        REQUIRE(movedSignature);
        CHECK(ConstantTimeEquals(movedSignature.Value().View(), signature.Value().View()));
    }

    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoEd25519phStateRules) {
    const Ed25519SigningKey key = KeyFromSeed(PhSeed);
    REQUIRE(!key.IsEmpty());
    auto signer = Ed25519phSigner::TryCreate();
    REQUIRE(signer);
    REQUIRE(signer.Value().TryUpdate(Text("abc")));

    // 빈 키로 끝내면 StaleHandle이고 상태를 바꾸지 않는다: 이어서 올바른 키로 끝낼 수 있다.
    CHECK(signer.Value().TryFinish(Ed25519SigningKey()).Error() == ErrorCode::StaleHandle);
    auto signature = signer.Value().TryFinish(key);
    REQUIRE(signature);
    CHECK(Equals(signature.Value().View(), PhSignature));

    // 끝난 뒤에는 Reset 전까지 InvalidArgument다.
    CHECK(signer.Value().TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);
    const Span<const uint8> pieces[] = {Text("x")};
    CHECK(signer.Value().TryUpdate(Span<const Span<const uint8>>(pieces)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(signer.Value().TryFinish(key).Error() == ErrorCode::InvalidArgument);

    // 이동한 객체는 StaleHandle이다.
    Ed25519phSigner moved = Move(signer.Value());
    CHECK(signer.Value().TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    CHECK(signer.Value().TryFinish(key).Error() == ErrorCode::StaleHandle);
    signer.Value().Reset();
    moved.Reset();
    REQUIRE(moved.TryUpdate(Text("abc")));
    auto movedSignature = moved.TryFinish(key);
    REQUIRE(movedSignature);
    CHECK(Equals(movedSignature.Value().View(), PhSignature));

    // 검증은 결과와 관계없이 끝난다. 길이가 틀린 서명도 AuthenticationFailed다.
    auto verifier = Ed25519phVerifier::TryCreate();
    REQUIRE(verifier);
    REQUIRE(verifier.Value().TryUpdate(Text("abc")));
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View().Left(63)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(verifier.Value().TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("abc")));
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View()));
    Ed25519phVerifier movedVerifier = Move(verifier.Value());
    CHECK(verifier.Value().TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    CHECK(movedVerifier.TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);

    // 버퍼 할당기를 쓰고 소멸 때 돌려준다.
    const usize allocations = GetSecretMemoryStatus().bufferAllocations;
    {
        auto scoped = Ed25519phSigner::TryCreate();
        REQUIRE(scoped);
        CHECK(GetSecretMemoryStatus().bufferAllocations == allocations + 1);
    }

    CHECK(GetSecretMemoryStatus().bufferAllocations == allocations);
}
