#include <Core/Memory/SystemAllocator.h>
#include <Crypto/Bytes.h>
#include <Crypto/Hash.h>
#include <Crypto/Secret.h>
#include <Crypto/Signature.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace signature_tests_file {

using namespace rocks::crypto;

// 알려진 답의 출처: RFC 8032 7.1 TEST 1–3(Ed25519, Python cryptography·pycryptodome으로 재계산)과
// 7.3 TEST abc(Ed25519ph, pycryptodome으로 재계산)다. 경계·음성 벡터는 RFC 8032 순수 Python
// 구현으로 만들고 OpenSSL·pycryptodome·실제 libsodium(PyNaCl) 결과로 어느 검사가 거부하는지
// 확인했다.
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

    // Ed25519ph와 순수 Ed25519는 서로의 서명을 받지 않는다. ph 서명은 dom2 접두를 붙이므로
    // SHA-512(m)에 대한 순수 서명으로도 확인되지 않는다.
    CHECK(TryVerify(publicKey, Text("abc"), signature.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryVerify(publicKey, Sha512(Text("abc")).View(), signature.Value().View()).Error() ==
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

    // R·S의 bit를 37 간격으로 훑어 바꾸고, S의 최상위 bit(바꾸면 S >= L이라 비정규 S 경로)와 R의
    // 부호 bit도 바꾼다. 메시지·키가 달라도 실패한다.
    const auto expectRejected = [&](usize bit) {
        uint8 tampered[64];
        memcpy(tampered, signature.bytes, sizeof(tampered));
        tampered[bit / 8] ^= static_cast<uint8>(1u << (bit % 8));
        CHECK(TryVerify(publicKey, message.View(), Span<const uint8>(tampered)).Error() ==
              ErrorCode::AuthenticationFailed);
    };
    for (usize bit = 0; bit < 512; bit += 37) {
        expectRejected(bit);
    }

    expectRejected(31 * 8 + 7);
    expectRejected(63 * 8 + 7);
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

    // 아래 음성 벡터는 각각 다른 검사가 모두 통과하는 상태에서 libsodium 검사 하나만 거부한다.
    // - 비정규 S(TEST 1의 R ‖ (S + L)): S + L은 mod L로 같은 scalar라 [S]B가 같다. S < L을 보지
    //   않는 검증(libsodium ED25519_COMPAT 빌드, S를 줄이는 구현)은 받는다. 최상위 3 bit가 0이라
    //   COMPAT의 상위 bit 검사도 통과하므로 ED25519_COMPAT 미정의를 고정한다.
    const HexBytes noncanonical("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e06522490155"
                                "4c8c7872aa064e049dbb3013fbf29380d25bf5f0595bbe24655141438e7a101b");
    REQUIRE(noncanonical.size == 64);
    CHECK(TryVerify(PublicKeyFromHex(Rfc8032Cases[0].publicKey), Span<const uint8>(),
                    noncanonical.View())
              .Error() == ErrorCode::AuthenticationFailed);

    // - small-order 공개키(항등원)와 R = B, S = 1: 허용적 검증(OpenSSL·pycryptodome 포함)은 어떤
    //   메시지든 받는다. libsodium은 small-order 공개키 검사 하나로 거부한다.
    auto identity = Ed25519PublicKey::TryFromBytes(
        HexBytes("0100000000000000000000000000000000000000000000000000000000000000").View());
    REQUIRE(identity);
    const HexBytes forged("5866666666666666666666666666666666666666666666666666666666666666"
                          "0100000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(forged.size == 64);
    CHECK(TryVerify(identity.Value(), Text("any message"), forged.View()).Error() ==
          ErrorCode::AuthenticationFailed);

    // - small-order R(항등원)과 S = k·a(TEST 2 키): cofactorless 식이 성립해 OpenSSL·pycryptodome은
    //   받지만 libsodium은 small-order R 검사로 거부한다.
    const HexBytes smallOrderR("0100000000000000000000000000000000000000000000000000000000000000"
                               "675d054595fa17cb6a44e3ada6ca385e55667e41a63fdab0ac22d10000354002");
    REQUIRE(smallOrderR.size == 64);
    CHECK(TryVerify(publicKey, message.View(), smallOrderR.View()).Error() ==
          ErrorCode::AuthenticationFailed);

    // - 혼합 차수 공개키(a·B + 차수 8 점): cofactored 식만 성립하는 서명은 거부하고(검증식이
    //   cofactorless), 두 식이 모두 성립하는 서명은 받는다(혼합 차수 키 자체는 거부하지 않는다).
    auto mixed = Ed25519PublicKey::TryFromBytes(
        HexBytes("0169d3c02f3edd121cafbe1dfd45c4cf4aac216a79e36e6326419f2a654d2d46").View());
    REQUIRE(mixed);
    const HexBytes cofactoredOnly(
        "fc9a6fd4133c19c2757d8c2fe80131f4a290ed892eefff11d5280879fdb5d7fd"
        "249af2277de10d5d5ed23293bcc60fbd1cdbdd357961a6f49f42151823fcb401");
    const HexBytes bothEquations(
        "7c9854c4f604c5fdc5d11ccc1b7d6a3fe999098a3f1ee2b39d4a77541eed44b8"
        "91a9780c1197743860edf821c3e8b830152e9e70a23e3a288e824294e6795e01");
    REQUIRE(cofactoredOnly.size == 64 && bothEquations.size == 64);
    CHECK(TryVerify(mixed.Value(), Text("mixed-order key 0"), cofactoredOnly.View()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(TryVerify(mixed.Value(), Text("mixed-order key 1"), bothEquations.View()));
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
    CHECK(signer.Value().TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    Ed25519phSigner defaulted;
    CHECK(defaulted.TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    CHECK(defaulted.TryFinish(key).Error() == ErrorCode::StaleHandle);
    moved.Reset();
    REQUIRE(moved.TryUpdate(Text("abc")));
    auto movedSignature = moved.TryFinish(key);
    REQUIRE(movedSignature);
    CHECK(Equals(movedSignature.Value().View(), PhSignature));

    // 검증은 결과와 관계없이 끝난다. 실패(길이가 틀린 서명, 64 byte 불일치)는 Reset 전까지
    // AuthenticationFailed로 남고(sticky), 성공 뒤에는 InvalidArgument다.
    auto verifier = Ed25519phVerifier::TryCreate();
    REQUIRE(verifier);
    REQUIRE(verifier.Value().TryUpdate(Text("abc")));
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View().Left(63)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(verifier.Value().TryUpdate(Text("x")).Error() == ErrorCode::AuthenticationFailed);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("abd")));
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View()).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(verifier.Value().TryUpdate(Text("x")).Error() == ErrorCode::AuthenticationFailed);
    const Span<const uint8> verifierPieces[] = {Text("x")};
    CHECK(verifier.Value().TryUpdate(Span<const Span<const uint8>>(verifierPieces)).Error() ==
          ErrorCode::AuthenticationFailed);
    Ed25519phVerifier failedMoved = Move(verifier.Value());
    CHECK(failedMoved.TryUpdate(Text("x")).Error() == ErrorCode::AuthenticationFailed);
    verifier.Value() = Move(failedMoved);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("abc")));
    CHECK(verifier.Value().TryFinish(key.PublicKey(), signature.Value().View()));
    Ed25519phVerifier movedVerifier = Move(verifier.Value());
    CHECK(verifier.Value().TryUpdate(Text("x")).Error() == ErrorCode::StaleHandle);
    CHECK(movedVerifier.TryUpdate(Text("x")).Error() == ErrorCode::InvalidArgument);

    // 기본은 버퍼 할당기이고, 다른 allocator를 주면 그 allocator에서 할당하고
    // 돌려준다(SystemAllocator는 소멸 때 남은 할당이 있으면 Fatal). 이동 대입은 대상의 이전 상태를
    // 그 allocator로 돌려준다.
    const usize allocations = GetSecretMemoryStatus().bufferAllocations;
    {
        auto scoped = Ed25519phSigner::TryCreate();
        auto scopedVerifier = Ed25519phVerifier::TryCreate();
        REQUIRE(scoped && scopedVerifier);
        CHECK(GetSecretMemoryStatus().bufferAllocations == allocations + 2);
    }

    CHECK(GetSecretMemoryStatus().bufferAllocations == allocations);
    SystemAllocator allocator;
    {
        auto first = Ed25519phSigner::TryCreate(allocator);
        auto second = Ed25519phSigner::TryCreate(allocator);
        auto checker = Ed25519phVerifier::TryCreate(allocator);
        auto other = Ed25519phVerifier::TryCreate(allocator);
        REQUIRE(first && second && checker && other);
        CHECK(allocator.Stats().liveAllocations == 4);
        first.Value() = Move(second.Value());
        checker.Value() = Move(other.Value());
        CHECK(allocator.Stats().liveAllocations == 2);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CryptoEd25519phBoundaries) {
    const Ed25519SigningKey key = KeyFromSeed(PhSeed);
    REQUIRE(!key.IsEmpty());
    const Ed25519PublicKey publicKey = key.PublicKey();

    // 빈 메시지(E1): 넣은 뒤 Reset하면 쌓인 입력을 버리므로 빈 메시지 서명과 같다.
    constexpr const char* EmptyPhSignature =
        "5b88ce227bf28bdef4c66ae9608c8bc15e81148386849110d2ad76abab768949"
        "342afe5a477ac0c41fd211a6dfc36dd5e57b6d26c1fb7a92187ed72e6997d304";
    auto signer = Ed25519phSigner::TryCreate();
    REQUIRE(signer);
    REQUIRE(signer.Value().TryUpdate(Text("x")));
    signer.Value().Reset();
    auto empty = signer.Value().TryFinish(key);
    REQUIRE(empty);
    CHECK(Equals(empty.Value().View(), EmptyPhSignature));

    // 여러 블록 조각(E2): 777 byte(byte i = (7i + 3) mod 256)를 5·0·128·644 byte로 나눠 넣는다.
    // 같은 메시지의 순수 Ed25519(F)도 고정한다.
    uint8 message[777];
    for (usize i = 0; i < sizeof(message); ++i) {
        message[i] = static_cast<uint8>(i * 7 + 3);
    }

    const Span<const uint8> whole = Span<const uint8>(message);
    const Span<const uint8> pieces[] = {whole.Left(5), whole.Mid(5, 0), whole.Mid(5, 128),
                                        whole.Mid(133, 644)};
    signer.Value().Reset();
    REQUIRE(signer.Value().TryUpdate(Span<const Span<const uint8>>(pieces)));
    auto chunked = signer.Value().TryFinish(key);
    REQUIRE(chunked);
    CHECK(Equals(chunked.Value().View(),
                 "6a19f770e2336068f0a04a0e1988ccc1f78f932a124e0ff44ad358827493c4b6"
                 "f1c2e9a295da96cc3dbf62cc9d0f455d274a8da64bc0c9c1d29e992e0ef22a0f"));
    auto pure = TrySign(key, whole);
    REQUIRE(pure);
    CHECK(Equals(pure.Value().View(),
                 "cd7c52d2821864171026454f438042669aa331360da751f33c8ed064c77cba34"
                 "0d57f1f9f11587c3b27da538c2666064b970b7d7988a9a2de69cd980940f0208"));
    auto verifier = Ed25519phVerifier::TryCreate();
    REQUIRE(verifier);
    REQUIRE(verifier.Value().TryUpdate(Span<const Span<const uint8>>(pieces)));
    CHECK(verifier.Value().TryFinish(publicKey, chunked.Value().View()));

    // 신뢰 공개키 여러 개(키 회전): 스트림을 다시 해시하지 않고 맞는 키의 순번을 돌려준다.
    const Ed25519PublicKey other = PublicKeyFromHex(Rfc8032Cases[0].publicKey);
    const Ed25519PublicKey trusted[] = {other, publicKey};
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(whole));
    auto index = verifier.Value().TryFinishAny(Span<const Ed25519PublicKey>(trusted),
                                               chunked.Value().View());
    REQUIRE(index);
    CHECK(index.Value() == 1);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(whole));
    CHECK(verifier.Value()
              .TryFinishAny(Span<const Ed25519PublicKey>(), chunked.Value().View())
              .Error() == ErrorCode::InvalidArgument);
    CHECK(verifier.Value()
              .TryFinishAny(Span<const Ed25519PublicKey>(trusted).Left(1), chunked.Value().View())
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(verifier.Value().TryUpdate(whole).Error() == ErrorCode::AuthenticationFailed);

    // ph 경로에서도 small-order 공개키 위조(R = B, S = 1)를 거부한다(허용적 구현은 받는다).
    auto identity = Ed25519PublicKey::TryFromBytes(
        HexBytes("0100000000000000000000000000000000000000000000000000000000000000").View());
    REQUIRE(identity);
    const HexBytes forged("5866666666666666666666666666666666666666666666666666666666666666"
                          "0100000000000000000000000000000000000000000000000000000000000000");
    REQUIRE(forged.size == 64);
    verifier.Value().Reset();
    REQUIRE(verifier.Value().TryUpdate(Text("any message")));
    CHECK(verifier.Value().TryFinish(identity.Value(), forged.View()).Error() ==
          ErrorCode::AuthenticationFailed);
}
} // namespace signature_tests_file
