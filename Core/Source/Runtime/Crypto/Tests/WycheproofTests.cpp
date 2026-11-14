#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/Hash.h>
#include <Crypto/Kdf.h>
#include <Crypto/Kem.h>
#include <Crypto/KeyExchange.h>
#include <Crypto/Mac.h>
#include <Crypto/Signature.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>

#include <stdio.h>
#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// Wycheproof(C2SP, Apache-2.0, 데이터 전용 ThirdParty) 시험 벡터를 사상표대로 돌린다
// (Docs/Design/Crypto.ko.md 12절). 모든 사례를 세고 갈래별 개수를 고정해 조용히 건너뛰는 사례가
// 없게 한다. 사상:
// - AEAD: valid는 봉인 byte와 열기 결과가 같고, invalid는 열기가 AuthenticationFailed이고 평문
//   출력을 0으로 지운다. AES-GCM은 256 bit 키·96 bit nonce만 검증한다. 128 bit 키는 AES 경량
//   계층(C2) 전까지 Unsupported, 96 bit가 아닌 nonce는 Unsupported, 192 bit 키는 열거값이 없어
//   세기만 한다. AES HW가 없는 기기에서는 256 bit 키도 Unsupported로 센다. ChaCha20-Poly1305·
//   XChaCha20-Poly1305의 nonce 길이 오류는 InvalidArgument다.
// - HMAC: 잘린 tag는 전체 tag의 앞부분과 비교하고, 검증 함수는 잘린 tag를 AuthenticationFailed로
//   거부한다.
// - HKDF: valid는 출력이 같고 invalid(출력 길이 상한 초과)는 InvalidArgument다.
// - Ed25519: valid는 수용, invalid는 AuthenticationFailed다. acceptable은 libsodium 판정(혼합 차수
//   공개키는 수용, small-order 공개키는 거부)을 기대값으로 둔다.
// - X25519: crypto_kx로 클라이언트·서버 양쪽을 돌린다. 공유 비밀이 0이면(small-order 점)
//   AuthenticationFailed이고, 그 밖에는(twist 점·비정규 u 포함) 세션 키가 BLAKE2b-512(공유 비밀 ‖
//   client_pk ‖ server_pk)와 같다.
// - ML-KEM-768: seed로 만든 공개키가 ek와 같고 캡슐 해제 결과가 K와 같다(암묵적 거부 포함). seed
//   길이 오류는 InvalidArgument, 암호문 길이 오류는 AuthenticationFailed다.
// - AEGIS는 들이지 않았다. Wycheproof는 128 bit tag라 libsodium(256 bit tag)으로는 tag와 invalid
//   사례를 검증할 수 없다(암호문 부분은 tag 길이와 무관하지만 tag256 draft 벡터로 대신한다).
namespace {

constexpr const char* VectorDirectory =
    "Core/Source/ThirdParty/Wycheproof/wycheproof-65ca494727a5/testvectors_v1/";

bool ReadFile(const char* name, Array<char>& out) noexcept {
    char path[256];
    snprintf(path, sizeof(path), "%s%s", VectorDirectory, name);
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

// 시험 파일 하나다. JsonValue가 문서를 가리키므로 옮기지 않고 시험 범위에 둔다.
struct VectorFile {
    SystemAllocator allocator;
    Array<char> text{allocator};
    JsonDocument document{allocator};

    bool Load(const char* name) noexcept {
        return ReadFile(name, text) &&
               document.Parse(StringView(text.Data(), text.Size())).Succeeded();
    }
};

enum class Field : uint8 {
    Required,
    Optional, // 없으면 빈 byte다
};

// hex 문자열 필드를 byte로 푼다. 필수 필드가 없거나(필드 이름 오타·schema 변경을 invalid 갈래가
// 우연히 통과하지 않게), 문자열이 아니거나, hex가 아니면 실패다.
bool Decode(JsonValue object, const char* field, Array<uint8>& out,
            Field kind = Field::Required) noexcept {
    const JsonValue value = object.Find(field);
    if (!value.IsValid()) {
        out.Clear();
        return kind == Field::Optional;
    }

    if (!value.IsString()) {
        return false;
    }

    const StringView hex = value.GetString();
    if (!out.TryResize(hex.ByteCount() / 2)) {
        return false;
    }

    auto decoded = TryFromHex(hex, out.View());
    return decoded && decoded.Value() == out.Size();
}

// 정수 필드를 읽는다. 없거나 정수가 아니면 실패다.
bool IntegerField(JsonValue object, const char* field, usize& out) noexcept {
    const JsonValue value = object.Find(field);
    if (!value.IsNumber()) {
        return false;
    }

    auto number = value.TryGetUInt64();
    if (!number) {
        return false;
    }

    out = static_cast<usize>(number.Value());
    return true;
}

bool ResultIs(JsonValue test, const char* expected) noexcept {
    return test.Find("result").GetString() == StringView(expected);
}

bool HasFlag(JsonValue test, const char* flag) noexcept {
    const JsonValue flags = test.Find("flags");
    for (usize i = 0; i < flags.Size(); ++i) {
        if (flags.At(i).GetString() == StringView(flag)) {
            return true;
        }
    }

    return false;
}

// 모든 그룹의 모든 시험에 body(group, test)를 부르고 시험 수를 돌려준다.
template <typename Body>
usize ForEachTest(const VectorFile& file, Body&& body) noexcept {
    usize count = 0;
    const JsonValue groups = file.document.Root().Find("testGroups");
    for (usize g = 0; g < groups.Size(); ++g) {
        const JsonValue group = groups.At(g);
        const JsonValue tests = group.Find("tests");
        for (usize t = 0; t < tests.Size(); ++t) {
            body(group, tests.At(t));
            ++count;
        }
    }

    return count;
}

struct AeadCounts {
    usize total = 0;
    usize verified = 0;
    usize unsupported = 0;
    usize badNonce = 0;
    usize skipped = 0;
};

// AEAD 파일 하나를 algorithm으로 돌린다(키 길이가 algorithm과 맞는 그룹만 검증한다).
void RunAead(const char* name, AeadAlgorithm algorithm, AeadCounts& counts) {
    VectorFile file;
    REQUIRE(file.Load(name));
    const AeadInfo info = TryGetAeadInfo(algorithm).Value();
    const bool gcm = algorithm == AeadAlgorithm::Aes256Gcm;
    Array<uint8> key(file.allocator);
    Array<uint8> iv(file.allocator);
    Array<uint8> aad(file.allocator);
    Array<uint8> msg(file.allocator);
    Array<uint8> ct(file.allocator);
    Array<uint8> tag(file.allocator);
    Array<uint8> out(file.allocator);
    Array<uint8> outTag(file.allocator);
    counts.total = ForEachTest(file, [&](JsonValue group, JsonValue test) {
        REQUIRE(Decode(test, "key", key) && Decode(test, "iv", iv) && Decode(test, "aad", aad) &&
                Decode(test, "msg", msg) && Decode(test, "ct", ct) && Decode(test, "tag", tag));
        usize keyBits = 0;
        REQUIRE(IntegerField(group, "keySize", keyBits));
        if (gcm && keyBits == 128) {
            CHECK(AeadKey::TryImport(AeadAlgorithm::Aes128Gcm, key.View()).Error() ==
                  ErrorCode::Unsupported);
            ++counts.unsupported;
            return;
        }

        if (keyBits != info.keyBytes * 8) {
            ++counts.skipped;
            return;
        }

        auto aeadKey = AeadKey::TryImport(algorithm, key.View());
        if (!info.available) {
            CHECK(aeadKey.Error() == ErrorCode::Unsupported);
            ++counts.unsupported;
            return;
        }

        REQUIRE(aeadKey);
        REQUIRE(out.TryResize(ct.Size()) && outTag.TryResize(info.tagBytes));
        if (iv.Size() != info.nonceBytes) {
            const ErrorCode expected = gcm ? ErrorCode::Unsupported : ErrorCode::InvalidArgument;
            CHECK(hazmat::TryOpenDetached(aeadKey.Value(), iv.View(), aad.View(), ct.View(),
                                          tag.View(), out.View())
                      .Error() == expected);
            ++counts.badNonce;
            return;
        }

        if (ResultIs(test, "valid")) {
            CHECK(hazmat::TrySealDetached(aeadKey.Value(), iv.View(), aad.View(), msg.View(),
                                          out.View(), outTag.View()));
            CHECK(ConstantTimeEquals(out.View(), ct.View()));
            CHECK(ConstantTimeEquals(outTag.View(), tag.View()));
            CHECK(hazmat::TryOpenDetached(aeadKey.Value(), iv.View(), aad.View(), ct.View(),
                                          tag.View(), out.View()));
            CHECK(ConstantTimeEquals(out.View(), msg.View()));
        } else {
            // 실패한 열기는 평문 출력 전체를 0으로 지운다(Aead.h 계약).
            CHECK(ResultIs(test, "invalid"));
            memset(out.Data(), 0xA5, out.Size());
            CHECK(hazmat::TryOpenDetached(aeadKey.Value(), iv.View(), aad.View(), ct.View(),
                                          tag.View(), out.View())
                      .Error() == ErrorCode::AuthenticationFailed);
            CHECK(IsAllZero(out.View()));
        }

        ++counts.verified;
    });
}

} // namespace

TEST(CryptoWycheproofAesGcm) {
    AeadCounts counts;
    RunAead("aes_gcm_test.json", AeadAlgorithm::Aes256Gcm, counts);
    CHECK(counts.total == 316);
    CHECK(counts.skipped == 103); // AES-192
    if (TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available) {
        CHECK(counts.verified == 66 && counts.badNonce == 39 && counts.unsupported == 108);
    } else {
        CHECK(counts.unsupported == 316 - 103);
    }
}

TEST(CryptoWycheproofChaCha20Poly1305) {
    AeadCounts ietf;
    RunAead("chacha20_poly1305_test.json", AeadAlgorithm::ChaCha20Poly1305Ietf, ietf);
    CHECK(ietf.total == 325 && ietf.verified == 316 && ietf.badNonce == 9);
    AeadCounts xchacha;
    RunAead("xchacha20_poly1305_test.json", AeadAlgorithm::XChaCha20Poly1305, xchacha);
    CHECK(xchacha.total == 315 && xchacha.verified == 306 && xchacha.badNonce == 9);
}

TEST(CryptoWycheproofHmac) {
    struct File {
        const char* name;
        MacAlgorithm algorithm;
        usize tagBytes;
    };

    const File files[] = {{"hmac_sha256_test.json", MacAlgorithm::HmacSha256, HmacSha256TagBytes},
                          {"hmac_sha512_test.json", MacAlgorithm::HmacSha512, HmacSha512TagBytes}};
    for (const File& item : files) {
        VectorFile file;
        REQUIRE(file.Load(item.name));
        Array<uint8> key(file.allocator);
        Array<uint8> msg(file.allocator);
        Array<uint8> tag(file.allocator);
        usize truncated = 0;
        const usize total = ForEachTest(file, [&](JsonValue, JsonValue test) {
            REQUIRE(Decode(test, "key", key) && Decode(test, "msg", msg) &&
                    Decode(test, "tag", tag));
            auto macKey = MacKey::TryImport(item.algorithm, key.View());
            REQUIRE(macKey);
            REQUIRE(tag.Size() <= item.tagBytes);
            uint8 full[MaxMacTagBytes];
            REQUIRE(TryMac(macKey.Value(), msg.View(), Span<uint8>(full).Left(item.tagBytes)));
            const bool same =
                ConstantTimeEquals(Span<const uint8>(full).Left(tag.Size()), tag.View());
            CHECK(same == ResultIs(test, "valid"));
            const auto verified = TryVerifyMac(macKey.Value(), msg.View(), tag.View());
            if (tag.Size() == item.tagBytes) {
                CHECK(static_cast<bool>(verified) == same);
            } else {
                // 검증 함수는 잘린 tag를 받지 않는다(맞는 앞부분이어도 거부).
                CHECK(verified.Error() == ErrorCode::AuthenticationFailed);
                ++truncated;
            }
        });
        CHECK(total == 174 && truncated == 87);
    }
}

TEST(CryptoWycheproofHkdf) {
    struct File {
        const char* name;
        HkdfAlgorithm algorithm;
        usize tests;
    };

    const File files[] = {{"hkdf_sha256_test.json", HkdfAlgorithm::Sha256, 86},
                          {"hkdf_sha512_test.json", HkdfAlgorithm::Sha512, 83}};
    for (const File& item : files) {
        VectorFile file;
        REQUIRE(file.Load(item.name));
        Array<uint8> ikm(file.allocator);
        Array<uint8> salt(file.allocator);
        Array<uint8> info(file.allocator);
        Array<uint8> okm(file.allocator);
        Array<uint8> out(file.allocator);
        usize invalid = 0;
        const usize total = ForEachTest(file, [&](JsonValue, JsonValue test) {
            REQUIRE(Decode(test, "ikm", ikm) && Decode(test, "salt", salt) &&
                    Decode(test, "info", info) && Decode(test, "okm", okm));
            usize size = 0;
            REQUIRE(IntegerField(test, "size", size) && out.TryResize(size));
            const auto derived =
                TryHkdf(item.algorithm, ikm.View(), salt.View(), info.View(), out.View());
            if (ResultIs(test, "valid")) {
                CHECK(derived);
                CHECK(ConstantTimeEquals(out.View(), okm.View()));
            } else {
                CHECK(derived.Error() == ErrorCode::InvalidArgument);
                ++invalid;
            }
        });
        CHECK(total == item.tests && invalid == 3);
    }
}

TEST(CryptoWycheproofEd25519) {
    VectorFile file;
    REQUIRE(file.Load("ed25519_test.json"));
    Array<uint8> pk(file.allocator);
    Array<uint8> msg(file.allocator);
    Array<uint8> sig(file.allocator);
    usize accepted = 0;
    usize acceptable = 0;
    const usize total = ForEachTest(file, [&](JsonValue group, JsonValue test) {
        REQUIRE(Decode(group.Find("publicKey"), "pk", pk) && Decode(test, "msg", msg) &&
                Decode(test, "sig", sig));
        auto key = Ed25519PublicKey::TryFromBytes(pk.View());
        REQUIRE(key);
        bool expected = ResultIs(test, "valid");
        if (ResultIs(test, "acceptable")) {
            expected = HasFlag(test, "MixedOrderPublicKey");
            ++acceptable;
        }

        const auto verified = TryVerify(key.Value(), msg.View(), sig.View());
        CHECK(static_cast<bool>(verified) == expected);
        if (!verified) {
            CHECK(verified.Error() == ErrorCode::AuthenticationFailed);
        } else {
            ++accepted;
        }
    });
    CHECK(total == 210 && acceptable == 29);
    CHECK(accepted == 117 + 21); // valid + 혼합 차수 공개키
}

namespace {

// kx 세션 키(32 byte 두 개)가 BLAKE2b-512(q ‖ client_pk ‖ server_pk)의 앞·뒤 절반과 같은지 본다.
// receiveFirst면 receive가 앞 절반이다(클라이언트), 아니면 transmit이 앞 절반이다(서버).
constexpr usize SessionKeyBytes = 32;

bool SessionKeysMatch(const KxSessionKeys& keys, Span<const uint8> shared,
                      Span<const uint8> clientPublic, Span<const uint8> serverPublic,
                      bool receiveFirst) {
    const Span<const uint8> pieces[] = {shared, clientPublic, serverPublic};
    uint8 expected[2 * SessionKeyBytes];
    static_assert(sizeof(expected) == Blake2bMaxDigestBytes);
    uint8 receive[SessionKeyBytes];
    uint8 transmit[SessionKeyBytes];
    if (!TryBlake2b(Span<const Span<const uint8>>(pieces), Span<uint8>(expected)) ||
        !hazmat::TryExportKey(keys.receive, Span<uint8>(receive)) ||
        !hazmat::TryExportKey(keys.transmit, Span<uint8>(transmit))) {
        return false;
    }

    const Span<const uint8> first = Span<const uint8>(expected).Left(SessionKeyBytes);
    const Span<const uint8> second = Span<const uint8>(expected).Right(SessionKeyBytes);
    return ConstantTimeEquals(Span<const uint8>(receive), receiveFirst ? first : second) &&
           ConstantTimeEquals(Span<const uint8>(transmit), receiveFirst ? second : first);
}

} // namespace

TEST(CryptoWycheproofX25519) {
    VectorFile file;
    REQUIRE(file.Load("x25519_test.json"));
    Array<uint8> secret(file.allocator);
    Array<uint8> peer(file.allocator);
    Array<uint8> shared(file.allocator);
    usize rejected = 0;
    const usize total = ForEachTest(file, [&](JsonValue, JsonValue test) {
        REQUIRE(Decode(test, "private", secret) && Decode(test, "public", peer) &&
                Decode(test, "shared", shared));
        // 이 파일에는 invalid가 없다(valid·acceptable뿐). 데이터가 바뀌면 사상을 다시 정한다.
        CHECK(!ResultIs(test, "invalid"));
        auto pair = KxKeyPair::TryImport(secret.View());
        auto peerKey = KxPublicKey::TryFromBytes(peer.View());
        REQUIRE(pair && peerKey);
        const AeadAlgorithm algorithm = AeadAlgorithm::XChaCha20Poly1305;
        auto client = TryClientSessionKeys(pair.Value(), peerKey.Value(), algorithm);
        auto server = TryServerSessionKeys(pair.Value(), peerKey.Value(), algorithm);
        if (IsAllZero(shared.View())) {
            CHECK(client.Error() == ErrorCode::AuthenticationFailed);
            CHECK(server.Error() == ErrorCode::AuthenticationFailed);
            ++rejected;
            return;
        }

        // 자기 키 쌍을 클라이언트로 쓰면 (own, peer), 서버로 쓰면 (peer, own) 순서로 해시한다.
        REQUIRE(client && server);
        const KxPublicKey own = pair.Value().PublicKey();
        CHECK(SessionKeysMatch(client.Value(), shared.View(), own.View(), peer.View(), true));
        CHECK(SessionKeysMatch(server.Value(), shared.View(), peer.View(), own.View(), false));
    });
    CHECK(total == 518 && rejected == 31);
}

TEST(CryptoWycheproofMlKem768) {
    VectorFile file;
    REQUIRE(file.Load("mlkem_768_test.json"));
    Array<uint8> seed(file.allocator);
    Array<uint8> ek(file.allocator);
    Array<uint8> c(file.allocator);
    Array<uint8> k(file.allocator);
    usize valid = 0;
    usize badSeed = 0;
    usize badCiphertext = 0;
    const KemInfo info = TryGetKemInfo(KemAlgorithm::MlKem768).Value();
    const usize total = ForEachTest(file, [&](JsonValue, JsonValue test) {
        // seed 길이가 틀린 사례에는 ek가 없다.
        REQUIRE(Decode(test, "seed", seed) && Decode(test, "ek", ek, Field::Optional) &&
                Decode(test, "c", c) && Decode(test, "K", k));
        auto key = KemKeyPair::TryFromSeed(KemAlgorithm::MlKem768, seed.View());
        if (seed.Size() != info.seedBytes) {
            CHECK(key.Error() == ErrorCode::InvalidArgument);
            CHECK(ResultIs(test, "invalid"));
            ++badSeed;
            return;
        }

        REQUIRE(key);
        REQUIRE(ek.Size() == key.Value().PublicKey().View().Size());
        CHECK(ConstantTimeEquals(key.Value().PublicKey().View(), ek.View()));
        const auto shared = TryDecapsulate(key.Value(), c.View());
        if (ResultIs(test, "valid")) {
            REQUIRE(shared);
            uint8 exported[KdfKeyBytes];
            REQUIRE(hazmat::TryExportKey(shared.Value(), Span<uint8>(exported)));
            CHECK(ConstantTimeEquals(Span<const uint8>(exported), k.View()));
            ++valid;
        } else {
            CHECK(shared.Error() == ErrorCode::AuthenticationFailed);
            ++badCiphertext;
        }
    });
    CHECK(total == 201 && valid == 161 && badSeed == 20 && badCiphertext == 20);
}
