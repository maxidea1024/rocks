#include <Crypto/Bytes.h>
#include <Crypto/KeyBlob.h>
#include <Crypto/Labels.h>
#include <Crypto/Private/KdfInternal.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: Key·Password 방식 KdfKey blob을 Python에서 따로 조립했다(BLAKE2b 파생은
// hashlib, XChaCha20-Poly1305는 pycryptodome, Argon2id는 argon2-cffi). 고정 값: KEK 00..1f, 비밀
// 40..5f, nonce a0..b7, keyId 1112…1f20, wrapKeyId 0102…0f10, 비밀번호 "correct horse", salt
// c0..cf, ops 1, memKiB 8.
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

Span<const uint8> Text(const char* text) {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), strlen(text));
}

constexpr const char* KeyMethodBlob =
    "524b4559010300011112131415161718191a1b1c1d1e1f2020000102030405060708090a0b0c0d0e0f10a0a1a2a3"
    "a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b79e7fe077d398bf518c5a7d9fec8a548739dbddd9f72e02753822"
    "f32ccb46ed542240d533ce6577e6f11a7b66a842a5ff2edbc02b4eb8e5ea79491859557dc6531133120ee26f75f5"
    "d6d2d8112b535025";
constexpr const char* PasswordMethodBlob =
    "524b4559010300021112131415161718191a1b1c1d1e1f2020000100000008000000c0c1c2c3c4c5c6c7c8c9cacb"
    "cccdcecfa0a1a2a3a4a5a6a7a8a9aaabacadaeafb0b1b2b3b4b5b6b793a89ed27498eacadb2ca616f87a3114ac99"
    "a8545905d34f846dc99cca99912e194a5992bc52718fefd2a6e6219c64f5e925ea70b6e18fc4e255775d5bdb51f4"
    "615d7bcd50f7ac83077fc6befef1c25d";

constexpr KeyId FixtureKeyId{{0x1112131415161718ull, 0x191A1B1C1D1E1F20ull}};
constexpr KeyId FixtureWrapKeyId{{0x0102030405060708ull, 0x090A0B0C0D0E0F10ull}};
constexpr PasswordCost SmallCost{1, 8 * 1024};

KdfKey FixtureKek() {
    uint8 bytes[32];
    for (usize i = 0; i < sizeof(bytes); ++i) {
        bytes[i] = static_cast<uint8>(i);
    }

    auto key = KdfKey::TryImport(Span<const uint8>(bytes));
    return key ? Move(key.Value()) : KdfKey();
}

bool FixtureSecretEquals(const KdfKey& key) {
    uint8 exported[32];
    if (!hazmat::TryExportKey(key, Span<uint8>(exported))) {
        return false;
    }

    for (usize i = 0; i < sizeof(exported); ++i) {
        if (exported[i] != static_cast<uint8>(0x40 + i)) {
            return false;
        }
    }

    return true;
}

// 키 타입마다 비밀 byte를 꺼낸다(같은 키인지 비교한다).
usize ExportSecret(const AeadKey& key, Span<uint8> out) {
    const usize bytes = TryGetAeadInfo(key.Algorithm()).Value().keyBytes;
    return hazmat::TryExportKey(key, out.Left(bytes)) ? bytes : 0;
}

usize ExportSecret(const MacKey& key, Span<uint8> out) {
    return hazmat::TryExportKey(key, out.Left(key.KeyBytes())) ? key.KeyBytes() : 0;
}

usize ExportSecret(const KdfKey& key, Span<uint8> out) {
    return hazmat::TryExportKey(key, out.Left(KdfKeyBytes)) ? KdfKeyBytes : 0;
}

usize ExportSecret(const Ed25519SigningKey& key, Span<uint8> out) {
    return hazmat::TryExportSeed(key, out.Left(Ed25519SeedBytes)) ? Ed25519SeedBytes : 0;
}

usize ExportSecret(const KxKeyPair& key, Span<uint8> out) {
    return hazmat::TryExportKey(key, out.Left(X25519SecretKeyBytes)) ? X25519SecretKeyBytes : 0;
}

usize ExportSecret(const BoxKeyPair& key, Span<uint8> out) {
    return hazmat::TryExportKey(key, out.Left(X25519SecretKeyBytes)) ? X25519SecretKeyBytes : 0;
}

usize ExportSecret(const KemKeyPair& key, Span<uint8> out) {
    const usize bytes = TryGetKemInfo(key.Algorithm()).Value().seedBytes;
    return hazmat::TryExportSeed(key, out.Left(bytes)) ? bytes : 0;
}

template <typename TKey>
bool SameSecret(const TKey& left, const TKey& right) {
    uint8 a[4096];
    uint8 b[4096];
    const usize leftBytes = ExportSecret(left, Span<uint8>(a));
    const usize rightBytes = ExportSecret(right, Span<uint8>(b));
    return leftBytes != 0 && leftBytes == rightBytes &&
           ConstantTimeEquals(Span<const uint8>(a).Left(leftBytes),
                              Span<const uint8>(b).Left(rightBytes));
}

// key를 두 방식으로 wrap·unwrap해 같은 비밀·ID가 돌아오는지 본다.
template <typename TKey>
void CheckRoundTrip(const KdfKey& kek, const TKey& key) {
    auto keyId = TryNewKeyId();
    auto kekId = TryNewKeyId();
    REQUIRE(keyId && kekId);
    auto expected = TryGetKeyBlobBytes(key, KeyBlobWrapMethod::Key);
    REQUIRE(expected);
    static uint8 blob[KeyBlobMaxBytes];
    auto written = TryWrapKey(kek, kekId.Value(), key, keyId.Value(), Span<uint8>(blob));
    REQUIRE(written);
    CHECK(written.Value() == expected.Value());
    const Span<const uint8> stored = Span<const uint8>(blob).Left(written.Value());

    auto info = TryPeekKeyBlob(stored);
    REQUIRE(info);
    CHECK(info.Value().type == KeyBlobTraits<TKey>::Type);
    CHECK(info.Value().method == KeyBlobWrapMethod::Key);
    CHECK(info.Value().keyId == keyId.Value());
    CHECK(info.Value().wrapKeyId == kekId.Value());
    CHECK(info.Value().blobBytes == written.Value());
    // 담긴 비밀 길이는 키 타입의 정본 길이다(Ed25519는 sk ‖ pk가 아니라 seed 32 byte).
    uint8 exported[4096];
    const usize secretBytes = ExportSecret(key, Span<uint8>(exported));
    CHECK(secretBytes != 0 && info.Value().secretBytes == secretBytes);

    auto restored = TryUnwrapKey<TKey>(kek, stored);
    REQUIRE(restored);
    CHECK(restored.Value().id == keyId.Value());
    CHECK(SameSecret(restored.Value().key, key));

    auto passwordBytes = TryGetKeyBlobBytes(key, KeyBlobWrapMethod::Password);
    REQUIRE(passwordBytes);
    auto passwordWritten = TryWrapKeyWithPassword(Text("pass phrase"), SmallCost, key,
                                                  keyId.Value(), Span<uint8>(blob));
    REQUIRE(passwordWritten);
    CHECK(passwordWritten.Value() == passwordBytes.Value());
    auto passwordInfo = TryPeekKeyBlob(Span<const uint8>(blob).Left(passwordWritten.Value()));
    REQUIRE(passwordInfo);
    CHECK(passwordInfo.Value().method == KeyBlobWrapMethod::Password);
    CHECK(passwordInfo.Value().keyId == keyId.Value());
    CHECK(passwordInfo.Value().wrapKeyId.IsNil());
    CHECK(passwordInfo.Value().passwordCost.operations == SmallCost.operations);
    CHECK(passwordInfo.Value().passwordCost.memoryBytes == SmallCost.memoryBytes);
    CHECK(passwordInfo.Value().secretBytes == secretBytes);
    CHECK(passwordInfo.Value().blobBytes == passwordWritten.Value());
    auto fromPassword = TryUnwrapKeyWithPassword<TKey>(
        Text("pass phrase"), Span<const uint8>(blob).Left(passwordWritten.Value()));
    REQUIRE(fromPassword);
    CHECK(fromPassword.Value().id == keyId.Value());
    CHECK(SameSecret(fromPassword.Value().key, key));
}

// 헤더만 맞춘 blob을 out에 만들고 길이를 돌려준다(nonce·commitment·암호문은 0). 거부가 KDF·Argon2id
// 전에 끝나는 경로를 보는 데 쓴다. keyId는 고정 값, Key 방식 wrapKeyId는 0x01 반복이다.
usize BuildHeaderOnlyBlob(uint8 type, uint8 algorithm, KeyBlobWrapMethod method, uint16 secretBytes,
                          uint32 operations, uint32 memoryKiB, Span<uint8> out) {
    const bool keyMethod = method == KeyBlobWrapMethod::Key;
    const usize params = keyMethod ? 16 : 24;
    const usize total = 98 + params + secretBytes;
    memset(out.Data(), 0, total);
    memcpy(out.Data(), "RKEY", 4);
    out[4] = 1;
    out[5] = type;
    out[6] = algorithm;
    out[7] = static_cast<uint8>(method);
    for (usize i = 0; i < 16; ++i) {
        out[8 + i] = static_cast<uint8>(0x11 + i);
    }

    out[24] = static_cast<uint8>(secretBytes);
    out[25] = static_cast<uint8>(secretBytes >> 8);
    if (keyMethod) {
        memset(out.Data() + 26, 0x01, 16);
    } else {
        for (usize i = 0; i < 4; ++i) {
            out[26 + i] = static_cast<uint8>(operations >> (8 * i));
            out[30 + i] = static_cast<uint8>(memoryKiB >> (8 * i));
        }
    }

    return total;
}

} // namespace

TEST(CryptoKeyBlobKnownAnswers) {
    const KdfKey kek = FixtureKek();
    REQUIRE(!kek.IsEmpty());

    const HexBytes keyBlob(KeyMethodBlob);
    REQUIRE(keyBlob.size == 146);
    auto info = TryPeekKeyBlob(keyBlob.View());
    REQUIRE(info);
    CHECK(info.Value().type == KeyBlobType::Kdf);
    CHECK(info.Value().algorithm == 0);
    CHECK(info.Value().method == KeyBlobWrapMethod::Key);
    CHECK(info.Value().keyId == FixtureKeyId);
    CHECK(info.Value().wrapKeyId == FixtureWrapKeyId);
    CHECK(info.Value().secretBytes == 32);
    CHECK(info.Value().blobBytes == 146);
    auto restored = TryUnwrapKey<KdfKey>(kek, keyBlob.View());
    REQUIRE(restored);
    CHECK(restored.Value().id == FixtureKeyId);
    CHECK(FixtureSecretEquals(restored.Value().key));

    const HexBytes passwordBlob(PasswordMethodBlob);
    REQUIRE(passwordBlob.size == 154);
    auto passwordInfo = TryPeekKeyBlob(passwordBlob.View());
    REQUIRE(passwordInfo);
    CHECK(passwordInfo.Value().method == KeyBlobWrapMethod::Password);
    CHECK(passwordInfo.Value().passwordCost.operations == 1);
    CHECK(passwordInfo.Value().passwordCost.memoryBytes == 8 * 1024);
    CHECK(passwordInfo.Value().wrapKeyId.IsNil());
    auto fromPassword =
        TryUnwrapKeyWithPassword<KdfKey>(Text("correct horse"), passwordBlob.View());
    REQUIRE(fromPassword);
    CHECK(fromPassword.Value().id == FixtureKeyId);
    CHECK(FixtureSecretEquals(fromPassword.Value().key));

    // 다른 비밀번호·KEK는 commitment에서 거부된다.
    CHECK(TryUnwrapKeyWithPassword<KdfKey>(Text("correct horsf"), passwordBlob.View()).Error() ==
          ErrorCode::AuthenticationFailed);
    auto otherKek = KdfKey::TryGenerate();
    REQUIRE(otherKek);
    CHECK(TryUnwrapKey<KdfKey>(otherKek.Value(), keyBlob.View()).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobRoundTrips) {
    auto kek = KdfKey::TryGenerate();
    REQUIRE(kek);

    const AeadAlgorithm aeadAlgorithms[] = {
        AeadAlgorithm::XChaCha20Poly1305, AeadAlgorithm::ChaCha20Poly1305Ietf,
        AeadAlgorithm::Aes256Gcm, AeadAlgorithm::Aegis256, AeadAlgorithm::Aegis128L};
    for (AeadAlgorithm algorithm : aeadAlgorithms) {
        auto key = AeadKey::TryGenerate(algorithm);
        if (key) {
            CheckRoundTrip(kek.Value(), key.Value());
        }
    }

    const MacAlgorithm macAlgorithms[] = {MacAlgorithm::HmacSha256, MacAlgorithm::HmacSha512,
                                          MacAlgorithm::Blake2bKeyed};
    for (MacAlgorithm algorithm : macAlgorithms) {
        auto key = MacKey::TryGenerate(algorithm);
        REQUIRE(key);
        CheckRoundTrip(kek.Value(), key.Value());
    }

    // HMAC 긴 키(상한 4096 byte)와 BLAKE2b keyed 64 byte 키다.
    static uint8 longKey[4096];
    for (usize i = 0; i < sizeof(longKey); ++i) {
        longKey[i] = static_cast<uint8>(i * 31 + 7);
    }

    auto longHmac = MacKey::TryImport(MacAlgorithm::HmacSha512, Span<const uint8>(longKey));
    auto wideBlake =
        MacKey::TryImport(MacAlgorithm::Blake2bKeyed, Span<const uint8>(longKey).Left(64));
    REQUIRE(longHmac && wideBlake);
    CheckRoundTrip(kek.Value(), longHmac.Value());
    CheckRoundTrip(kek.Value(), wideBlake.Value());

    auto kdf = KdfKey::TryGenerate();
    auto signing = Ed25519SigningKey::TryGenerate();
    auto kx = KxKeyPair::TryGenerate();
    auto box = BoxKeyPair::TryGenerate();
    auto xwing = KemKeyPair::TryGenerate(KemAlgorithm::XWing);
    auto mlkem = KemKeyPair::TryGenerate(KemAlgorithm::MlKem768);
    REQUIRE(kdf && signing && kx && box && xwing && mlkem);
    CheckRoundTrip(kek.Value(), kdf.Value());
    CheckRoundTrip(kek.Value(), signing.Value());
    CheckRoundTrip(kek.Value(), kx.Value());
    CheckRoundTrip(kek.Value(), box.Value());
    CheckRoundTrip(kek.Value(), xwing.Value());
    CheckRoundTrip(kek.Value(), mlkem.Value());

    // 푼 서명 키는 같은 서명을 낸다(공개키를 seed에서 다시 계산한다).
    uint8 blob[KeyBlobMaxBytes];
    const KeyId id = TryNewKeyId().Value();
    auto written =
        TryWrapKey(kek.Value(), TryNewKeyId().Value(), signing.Value(), id, Span<uint8>(blob));
    REQUIRE(written);
    auto restored =
        TryUnwrapKey<Ed25519SigningKey>(kek.Value(), Span<const uint8>(blob).Left(written.Value()));
    REQUIRE(restored);
    CHECK(restored.Value().key.PublicKey() == signing.Value().PublicKey());

    // nonce가 무작위라 같은 키도 blob이 다르다.
    uint8 again[KeyBlobMaxBytes];
    auto second =
        TryWrapKey(kek.Value(), TryNewKeyId().Value(), signing.Value(), id, Span<uint8>(again));
    REQUIRE(second);
    CHECK(!ConstantTimeEquals(Span<const uint8>(blob).Left(written.Value()),
                              Span<const uint8>(again).Left(second.Value())));
}

TEST(CryptoKeyBlobRejectsTampering) {
    const KdfKey kek = FixtureKek();
    const HexBytes source(KeyMethodBlob);
    REQUIRE(source.size == 146);

    // 모든 byte의 bit 하나를 바꾸면 거부된다. 식별 byte(버전·타입·알고리즘·방식)는 Unsupported(버전
    // 0, Mac 타입의 알고리즘 0, Kdf의 알고리즘 1, 방식 3이 모두 표 밖)이고 나머지는
    // AuthenticationFailed다.
    for (usize offset = 0; offset < source.size; ++offset) {
        uint8 tampered[146];
        memcpy(tampered, source.bytes, sizeof(tampered));
        tampered[offset] ^= 0x01;
        const ErrorCode error = TryUnwrapKey<KdfKey>(kek, Span<const uint8>(tampered)).Error();
        if (offset >= 4 && offset <= 7) {
            CHECK(error == ErrorCode::Unsupported);
        } else {
            CHECK(error == ErrorCode::AuthenticationFailed);
        }
    }

    // 모든 길이로 자르거나 꼬리 byte를 붙여도 거부된다.
    for (usize length = 0; length < source.size; ++length) {
        CHECK(TryUnwrapKey<KdfKey>(kek, source.View().Left(length)).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    uint8 longer[147] = {};
    memcpy(longer, source.bytes, source.size);
    CHECK(TryUnwrapKey<KdfKey>(kek, Span<const uint8>(longer)).Error() ==
          ErrorCode::AuthenticationFailed);

    // 기대한 타입·방식과 다르면 AuthenticationFailed다.
    CHECK(TryUnwrapKey<MacKey>(kek, source.View()).Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryUnwrapKeyWithPassword<KdfKey>(Text("x"), source.View()).Error() ==
          ErrorCode::AuthenticationFailed);
    const HexBytes passwordBlob(PasswordMethodBlob);
    CHECK(TryUnwrapKey<KdfKey>(kek, passwordBlob.View()).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobPeekAndHeaderRules) {
    const HexBytes source(KeyMethodBlob);
    // 헤더(Key 방식 42 byte)만 있어도 읽는다. 한 byte 모자라면 AuthenticationFailed다.
    auto header = TryPeekKeyBlob(source.View().Left(42));
    REQUIRE(header);
    CHECK(header.Value().blobBytes == 146);
    CHECK(TryPeekKeyBlob(source.View().Left(41)).Error() == ErrorCode::AuthenticationFailed);
    CHECK(TryPeekKeyBlob(source.View().Left(25)).Error() == ErrorCode::AuthenticationFailed);

    const auto peekWith = [&](usize offset, uint8 value) {
        uint8 bytes[146];
        memcpy(bytes, source.bytes, sizeof(bytes));
        bytes[offset] = value;
        return TryPeekKeyBlob(Span<const uint8>(bytes)).Error();
    };
    // 모르는 버전·타입·방식, 표에 없는 (타입, 알고리즘)은 Unsupported다.
    CHECK(peekWith(4, 2) == ErrorCode::Unsupported);
    CHECK(peekWith(5, 0) == ErrorCode::Unsupported);
    CHECK(peekWith(5, 8) == ErrorCode::Unsupported);
    CHECK(peekWith(6, 1) == ErrorCode::Unsupported);
    CHECK(peekWith(7, 3) == ErrorCode::Unsupported);
    // magic, 길이 범위 밖은 AuthenticationFailed다.
    CHECK(peekWith(0, 'X') == ErrorCode::AuthenticationFailed);
    CHECK(peekWith(24, 31) == ErrorCode::AuthenticationFailed);

    // Nil keyId·wrapKeyId는 AuthenticationFailed다.
    uint8 nil[146];
    memcpy(nil, source.bytes, sizeof(nil));
    memset(nil + 8, 0, 16);
    CHECK(TryPeekKeyBlob(Span<const uint8>(nil)).Error() == ErrorCode::AuthenticationFailed);
    memcpy(nil, source.bytes, sizeof(nil));
    memset(nil + 26, 0, 16);
    CHECK(TryPeekKeyBlob(Span<const uint8>(nil)).Error() == ErrorCode::AuthenticationFailed);

    // 비밀번호 비용: 하한 위반은 형식 오류(AuthenticationFailed), 상한 초과는 Argon2id를 돌리지
    // 않고 CapacityExceeded다(Peek는 비용을 그대로 알려 준다).
    const HexBytes passwordBlob(PasswordMethodBlob);
    REQUIRE(passwordBlob.size == 154);
    uint8 costly[154];
    memcpy(costly, passwordBlob.bytes, sizeof(costly));
    costly[26] = 0;
    CHECK(TryPeekKeyBlob(Span<const uint8>(costly)).Error() == ErrorCode::AuthenticationFailed);
    costly[26] = 33;
    auto costInfo = TryPeekKeyBlob(Span<const uint8>(costly));
    REQUIRE(costInfo);
    CHECK(costInfo.Value().passwordCost.operations == 33);
    CHECK(TryUnwrapKeyWithPassword<KdfKey>(Text("correct horse"), Span<const uint8>(costly))
              .Error() == ErrorCode::CapacityExceeded);
    memcpy(costly, passwordBlob.bytes, sizeof(costly));
    costly[30] = 7; // memKiB 7 < 8 KiB
    CHECK(TryPeekKeyBlob(Span<const uint8>(costly)).Error() == ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobRejectsPasswordTampering) {
    const HexBytes source(PasswordMethodBlob);
    REQUIRE(source.size == 154);
    const Span<const uint8> password = Text("correct horse");

    // 모든 byte의 bit 하나를 바꾼다. 식별 byte(4–7)는 Unsupported, 비용 상한을 넘기는 byte(ops
    // 27–29, memKiB 최상위 33)는 Argon2id 전에 CapacityExceeded, 나머지는
    // AuthenticationFailed다(ops 0은 하한 위반, memKiB 30–32는 Argon2id를 돌린 뒤 commitment에서
    // 걸린다. 32는 64 MiB를 한 번 쓴다).
    for (usize offset = 0; offset < source.size; ++offset) {
        uint8 tampered[154];
        memcpy(tampered, source.bytes, sizeof(tampered));
        tampered[offset] ^= 0x01;
        const ErrorCode error =
            TryUnwrapKeyWithPassword<KdfKey>(password, Span<const uint8>(tampered)).Error();
        if (offset >= 4 && offset <= 7) {
            CHECK(error == ErrorCode::Unsupported);
        } else if (offset == 27 || offset == 28 || offset == 29 || offset == 33) {
            CHECK(error == ErrorCode::CapacityExceeded);
        } else {
            CHECK(error == ErrorCode::AuthenticationFailed);
        }
    }

    // 모든 길이로 자르거나 꼬리 byte를 붙여도 거부된다.
    for (usize length = 0; length < source.size; ++length) {
        CHECK(TryUnwrapKeyWithPassword<KdfKey>(password, source.View().Left(length)).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    uint8 longer[155] = {};
    memcpy(longer, source.bytes, source.size);
    CHECK(TryUnwrapKeyWithPassword<KdfKey>(password, Span<const uint8>(longer)).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobAuthenticatesTypeAndAlgorithm) {
    // 헤더의 타입·알고리즘을 길이가 같은 다른 값으로 바꾸고 그 타입으로 열면 commitment에서
    // 거부된다(헤더가 파생 transcript에 묶여 있다).
    auto kek = KdfKey::TryGenerate();
    REQUIRE(kek);
    const KeyId id = TryNewKeyId().Value();
    const KeyId kekId = TryNewKeyId().Value();
    uint8 blob[KeyBlobMaxBytes];

    auto aead = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(aead);
    auto aeadWritten = TryWrapKey(kek.Value(), kekId, aead.Value(), id, Span<uint8>(blob));
    REQUIRE(aeadWritten);
    const AeadAlgorithm others[] = {AeadAlgorithm::ChaCha20Poly1305Ietf, AeadAlgorithm::Aes256Gcm,
                                    AeadAlgorithm::Aegis256};
    for (AeadAlgorithm other : others) {
        uint8 changed[KeyBlobMaxBytes];
        memcpy(changed, blob, aeadWritten.Value());
        changed[6] = static_cast<uint8>(other);
        const ErrorCode expected = TryGetAeadInfo(other).Value().available
                                       ? ErrorCode::AuthenticationFailed
                                       : ErrorCode::Unsupported;
        CHECK(
            TryUnwrapKey<AeadKey>(kek.Value(), Span<const uint8>(changed).Left(aeadWritten.Value()))
                .Error() == expected);
    }

    auto kx = KxKeyPair::TryGenerate();
    REQUIRE(kx);
    auto kxWritten = TryWrapKey(kek.Value(), kekId, kx.Value(), id, Span<uint8>(blob));
    REQUIRE(kxWritten);
    blob[5] = static_cast<uint8>(KeyBlobType::Box);
    CHECK(TryUnwrapKey<BoxKeyPair>(kek.Value(), Span<const uint8>(blob).Left(kxWritten.Value()))
              .Error() == ErrorCode::AuthenticationFailed);

    // Kdf blob(KAT)을 32 byte 비밀의 다른 타입으로 바꾼다.
    const KdfKey fixture = FixtureKek();
    const HexBytes source(KeyMethodBlob);
    uint8 retyped[146];
    memcpy(retyped, source.bytes, sizeof(retyped));
    retyped[5] = static_cast<uint8>(KeyBlobType::Ed25519);
    CHECK(TryUnwrapKey<Ed25519SigningKey>(fixture, Span<const uint8>(retyped)).Error() ==
          ErrorCode::AuthenticationFailed);
    retyped[5] = static_cast<uint8>(KeyBlobType::Kx);
    CHECK(TryUnwrapKey<KxKeyPair>(fixture, Span<const uint8>(retyped)).Error() ==
          ErrorCode::AuthenticationFailed);
    retyped[5] = static_cast<uint8>(KeyBlobType::Box);
    CHECK(TryUnwrapKey<BoxKeyPair>(fixture, Span<const uint8>(retyped)).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobRejectsBeforeKeyDerivation) {
    const KdfKey kek = FixtureKek();
    uint8 blob[KeyBlobMaxBytes];
    const auto aead = static_cast<uint8>(KeyBlobType::Aead);
    const auto aes128 = static_cast<uint8>(AeadAlgorithm::Aes128Gcm);
    const bool aes128Available = TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available;
    const ErrorCode aes128Error =
        aes128Available ? ErrorCode::AuthenticationFailed : ErrorCode::Unsupported;

    // 이 기기에서 쓸 수 없는 AEAD 알고리즘: Peek는 되고 unwrap은 KDF 전에 Unsupported다.
    const usize keyBytes =
        BuildHeaderOnlyBlob(aead, aes128, KeyBlobWrapMethod::Key, 16, 0, 0, Span<uint8>(blob));
    REQUIRE(keyBytes == 130);
    auto info = TryPeekKeyBlob(Span<const uint8>(blob).Left(keyBytes));
    REQUIRE(info);
    CHECK(info.Value().secretBytes == 16 && info.Value().blobBytes == 130);
    CHECK(TryUnwrapKey<AeadKey>(kek, Span<const uint8>(blob).Left(keyBytes)).Error() ==
          aes128Error);

    // 비용 상한은 알고리즘 판정보다 먼저다(상한 검사가 빠지면 Unsupported가 나온다).
    const Span<const uint8> password = Text("pw");
    usize passwordBytes = BuildHeaderOnlyBlob(aead, aes128, KeyBlobWrapMethod::Password, 16, 33, 8,
                                              Span<uint8>(blob));
    REQUIRE(passwordBytes == 138);
    CHECK(TryUnwrapKeyWithPassword<AeadKey>(password, Span<const uint8>(blob).Left(passwordBytes))
              .Error() == ErrorCode::CapacityExceeded);
    if (!aes128Available) {
        passwordBytes = BuildHeaderOnlyBlob(aead, aes128, KeyBlobWrapMethod::Password, 16, 1, 8,
                                            Span<uint8>(blob));
        CHECK(
            TryUnwrapKeyWithPassword<AeadKey>(password, Span<const uint8>(blob).Left(passwordBytes))
                .Error() == ErrorCode::Unsupported);
    }

    // blob에서 온 비용이 상한(메모리 1 GiB, 반복 × 메모리 4 GiB)을 넘으면 CapacityExceeded다.
    const auto kdf = static_cast<uint8>(KeyBlobType::Kdf);
    const struct {
        uint32 operations;
        uint32 memoryKiB;
    } costs[] = {{1, 0x00100001}, {5, 0x00100000}, {1, 0xFFFFFFFF}};
    for (const auto& cost : costs) {
        passwordBytes = BuildHeaderOnlyBlob(kdf, 0, KeyBlobWrapMethod::Password, 32,
                                            cost.operations, cost.memoryKiB, Span<uint8>(blob));
        auto costInfo = TryPeekKeyBlob(Span<const uint8>(blob).Left(passwordBytes));
        REQUIRE(costInfo);
        CHECK(costInfo.Value().passwordCost.operations == cost.operations);
        CHECK(costInfo.Value().passwordCost.memoryBytes == usize{cost.memoryKiB} * 1024);
        CHECK(
            TryUnwrapKeyWithPassword<KdfKey>(password, Span<const uint8>(blob).Left(passwordBytes))
                .Error() == ErrorCode::CapacityExceeded);
    }

    // Password 방식 헤더는 50 byte다. 모르는 버전은 magic 다음 byte만으로 Unsupported다.
    const HexBytes passwordBlob(PasswordMethodBlob);
    CHECK(TryPeekKeyBlob(passwordBlob.View().Left(50)));
    CHECK(TryPeekKeyBlob(passwordBlob.View().Left(49)).Error() == ErrorCode::AuthenticationFailed);
    const uint8 futureVersion[] = {'R', 'K', 'E', 'Y', 2};
    CHECK(TryPeekKeyBlob(Span<const uint8>(futureVersion)).Error() == ErrorCode::Unsupported);
    CHECK(TryUnwrapKey<KdfKey>(kek, Span<const uint8>(futureVersion)).Error() ==
          ErrorCode::Unsupported);
    CHECK(TryPeekKeyBlob(Span<const uint8>(futureVersion).Left(4)).Error() ==
          ErrorCode::AuthenticationFailed);
}

TEST(CryptoKeyBlobRejectsMisuse) {
    const KdfKey kek = FixtureKek();
    auto key = AeadKey::TryGenerate(AeadAlgorithm::XChaCha20Poly1305);
    REQUIRE(key);
    const KeyId id = TryNewKeyId().Value();
    const KeyId kekId = TryNewKeyId().Value();
    uint8 blob[KeyBlobMaxBytes];
    // 아래 거부(StaleHandle·InvalidArgument·CapacityExceeded)는 모두 쓰기 전이다. 끝에서 blob이
    // 그대로인지 본다.
    memset(blob, 0x5A, sizeof(blob));
    const auto untouched = [&]() {
        for (uint8 value : blob) {
            if (value != 0x5A) {
                return false;
            }
        }

        return true;
    };

    // 빈 키·KEK는 StaleHandle, Nil ID는 InvalidArgument다.
    CHECK(TryWrapKey(KdfKey(), kekId, key.Value(), id, Span<uint8>(blob)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryWrapKey(kek, kekId, AeadKey(), id, Span<uint8>(blob)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryWrapKey(kek, KeyId(), key.Value(), id, Span<uint8>(blob)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryWrapKey(kek, kekId, key.Value(), KeyId(), Span<uint8>(blob)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryUnwrapKey<AeadKey>(KdfKey(), Span<const uint8>(blob)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryGetKeyBlobBytes(AeadKey(), KeyBlobWrapMethod::Key).Error() == ErrorCode::StaleHandle);
    CHECK(TryGetKeyBlobBytes(key.Value(), static_cast<KeyBlobWrapMethod>(9)).Error() ==
          ErrorCode::InvalidArgument);

    // hazmat 짧은 MAC 키는 일반 저장 경로로 내보내지 않는다(크기 질의도 같은 규칙).
    auto shortKey = hazmat::TryImportShortMacKey(MacAlgorithm::HmacSha256, Text("short"));
    REQUIRE(shortKey);
    CHECK(TryWrapKey(kek, kekId, shortKey.Value(), id, Span<uint8>(blob)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryWrapKeyWithPassword(Text("pw"), SmallCost, shortKey.Value(), id, Span<uint8>(blob))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryGetKeyBlobBytes(shortKey.Value(), KeyBlobWrapMethod::Key).Error() ==
          ErrorCode::InvalidArgument);

    // 출력이 작으면 CapacityExceeded다.
    const usize needed = TryGetKeyBlobBytes(key.Value(), KeyBlobWrapMethod::Key).Value();
    CHECK(TryWrapKey(kek, kekId, key.Value(), id, Span<uint8>(blob).Left(needed - 1)).Error() ==
          ErrorCode::CapacityExceeded);

    // 비밀번호 인자: 빈 비밀번호, blob과 겹친 비밀번호, 범위 밖 cost는 InvalidArgument, 상한 초과는
    // CapacityExceeded다.
    CHECK(TryWrapKeyWithPassword(Span<const uint8>(), SmallCost, key.Value(), id, Span<uint8>(blob))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryWrapKeyWithPassword(Span<const uint8>(blob).Left(8), SmallCost, key.Value(), id,
                                 Span<uint8>(blob))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryWrapKeyWithPassword(Text("pw"), PasswordCost{0, 8 * 1024}, key.Value(), id,
                                 Span<uint8>(blob))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryWrapKeyWithPassword(Text("pw"), PasswordCost{MaxPasswordOperations + 1, 8 * 1024},
                                 key.Value(), id, Span<uint8>(blob))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(untouched());

    // 메모리 비용은 1 KiB 단위로 내려 기록하고 계산도 그 값으로 한다(같은 비용에 인코딩이 하나).
    auto rounded = TryWrapKeyWithPassword(Text("pw"), PasswordCost{1, 8 * 1024 + 1023}, key.Value(),
                                          id, Span<uint8>(blob));
    REQUIRE(rounded);
    const Span<const uint8> roundedBlob = Span<const uint8>(blob).Left(rounded.Value());
    auto roundedInfo = TryPeekKeyBlob(roundedBlob);
    REQUIRE(roundedInfo);
    CHECK(roundedInfo.Value().passwordCost.memoryBytes == 8 * 1024);
    auto roundedKey = TryUnwrapKeyWithPassword<AeadKey>(Text("pw"), roundedBlob);
    REQUIRE(roundedKey);
    CHECK(SameSecret(roundedKey.Value().key, key.Value()));
}

TEST(CryptoLabelsAndTypeTags) {
    // 엔진 KDF context는 모두 다르고 "Rk"로 시작한다.
    const usize count = sizeof(labels::All) / sizeof(labels::All[0]);
    for (usize i = 0; i < count; ++i) {
        CHECK(labels::All[i].Data()[0] == 'R' && labels::All[i].Data()[1] == 'k');
        for (usize j = i + 1; j < count; ++j) {
            CHECK(!(labels::All[i] == labels::All[j]));
        }
    }

    // 타입 파생 tag는 모두 다르다.
    detail::KdfTypeTag tags[32];
    usize tagCount = 0;
    tags[tagCount++] = detail::KdfKeyTypeTag;
    tags[tagCount++] = detail::KdfBoundTypeTag;
    for (uint8 algorithm = 1; algorithm <= 3; ++algorithm) {
        tags[tagCount++] = detail::MacKeyTypeTag(algorithm);
    }

    for (uint8 algorithm = 1; algorithm <= 6; ++algorithm) {
        tags[tagCount++] = detail::AeadKeyTypeTag(algorithm);
        tags[tagCount++] = detail::AeadSequenceKeyTypeTag(algorithm);
        tags[tagCount++] = detail::AeadSequenceIvTypeTag(algorithm);
    }

    for (usize i = 0; i < tagCount; ++i) {
        for (usize j = i + 1; j < tagCount; ++j) {
            CHECK(memcmp(tags[i].bytes, tags[j].bytes, sizeof(tags[i].bytes)) != 0);
        }
    }

    // KeyId는 Nil이 아니고 매번 다르다.
    auto first = TryNewKeyId();
    auto second = TryNewKeyId();
    REQUIRE(first && second);
    CHECK(!first.Value().IsNil() && !(first.Value() == second.Value()));
    CHECK(KeyId().IsNil());
}
