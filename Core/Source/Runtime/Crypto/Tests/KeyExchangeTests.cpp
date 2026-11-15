#include <Crypto/Aead.h>
#include <Crypto/Bytes.h>
#include <Crypto/KeyExchange.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::crypto;

// 알려진 답의 출처: X25519 키 쌍과 공유 비밀은 RFC 7748 6.1(Alice·Bob)이다. kx 세션 키는
// BLAKE2b-512(공유 비밀 ‖ client_pk ‖ server_pk)를 Python hashlib로 계산했다. sealed box 열기 값은
// 임시 비밀키 40..5f로 Python에서 조립했다(X25519·Poly1305는 cryptography, XSalsa20은 pycryptodome
// Salsa20과 순수 Python HSalsa20, HSalsa20은 NaCl 문서의 firstkey 값으로 확인). libsodium과
// 독립이다.
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

constexpr const char* AliceSecret =
    "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a";
constexpr const char* AlicePublic =
    "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a";
constexpr const char* BobSecret =
    "5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb";
constexpr const char* BobPublic =
    "de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f";

// 클라이언트(Alice) rx = BLAKE2b-512 앞 32 byte, tx = 뒤 32 byte다.
constexpr const char* ClientReceive =
    "322b7be3b9bce4a84fe6e2dea61e8e6d0a98f3e4c60b58bad722b1c855c9db22";
constexpr const char* ClientTransmit =
    "284901a611708379d0b5b0e40d77ea207624eaab8dd0c95e693fc3ee76c73ccb";

// Bob에게 "sealed for bob"을 봉인한 것과 빈 메시지를 봉인한 것이다(임시 비밀키 40..5f).
constexpr const char* SealedForBob =
    "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a"
    "a9db71872a97eb6a77d206330455506785a02ce0f6085eea19bd08c47c7e";
constexpr const char* SealedEmptyForBob =
    "79a631eede1bf9c98f12032cdeadd0e7a079398fc786b88cc846ec89af85a51a"
    "98db126da9f34abcd7d52e2237632060";

template <typename KeyPair>
KeyPair Import(const char* secretHex) {
    const HexBytes secret(secretHex);
    auto key = KeyPair::TryImport(secret.View());
    return key ? Move(key.Value()) : KeyPair();
}

bool KeyEquals(const AeadKey& key, const char* hex) {
    uint8 exported[32];
    return hazmat::TryExportKey(key, Span<uint8>(exported)) &&
           Equals(Span<const uint8>(exported), hex);
}

// sealed box는 BoxKeyPair로만 열고 kx는 KxKeyPair로만 한다(용도별 키 타입).
template <typename KeyPair>
concept CanOpenSealed = requires(const KeyPair& key, Span<const uint8> sealed,
                                 Span<uint8> plaintext) { TryOpenSealed(key, sealed, plaintext); };

template <typename KeyPair>
concept CanDeriveSessionKeys = requires(const KeyPair& key, const KxPublicKey& peer) {
    TryClientSessionKeys(key, peer, AeadAlgorithm::XChaCha20Poly1305);
};

static_assert(CanOpenSealed<BoxKeyPair> && !CanOpenSealed<KxKeyPair>);
static_assert(CanDeriveSessionKeys<KxKeyPair> && !CanDeriveSessionKeys<BoxKeyPair>);

} // namespace

TEST(CryptoKxRfc7748) {
    const KxKeyPair alice = Import<KxKeyPair>(AliceSecret);
    const KxKeyPair bob = Import<KxKeyPair>(BobSecret);
    REQUIRE(!alice.IsEmpty() && !bob.IsEmpty());
    CHECK(Equals(alice.PublicKey().View(), AlicePublic));
    CHECK(Equals(bob.PublicKey().View(), BobPublic));
    CHECK(alice.PublicKey() == KxPublicKey::TryFromBytes(HexBytes(AlicePublic).View()).Value());

    auto client = TryClientSessionKeys(alice, bob.PublicKey(), AeadAlgorithm::ChaCha20Poly1305Ietf);
    auto server = TryServerSessionKeys(bob, alice.PublicKey(), AeadAlgorithm::ChaCha20Poly1305Ietf);
    REQUIRE(client && server);
    CHECK(client.Value().receive.Algorithm() == AeadAlgorithm::ChaCha20Poly1305Ietf);
    CHECK(KeyEquals(client.Value().receive, ClientReceive));
    CHECK(KeyEquals(client.Value().transmit, ClientTransmit));
    // 역할이 묶인다: 서버 rx = 클라이언트 tx, 서버 tx = 클라이언트 rx.
    CHECK(KeyEquals(server.Value().receive, ClientTransmit));
    CHECK(KeyEquals(server.Value().transmit, ClientReceive));

    // 클라이언트가 보낸 것을 서버가 연다.
    const uint8 nonce[12] = {};
    uint8 sealed[64];
    auto written =
        hazmat::TrySealWithNonce(client.Value().transmit, Span<const uint8>(nonce),
                                 Span<const uint8>(), Text("hello server"), Span<uint8>(sealed));
    REQUIRE(written);
    uint8 opened[64];
    auto openedBytes = hazmat::TryOpenWithNonce(
        server.Value().receive, Span<const uint8>(nonce), Span<const uint8>(),
        Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened));
    REQUIRE(openedBytes);
    CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(openedBytes.Value()),
                             Text("hello server")));

    // 양쪽이 같은 역할(둘 다 클라이언트)로 착각하면 키가 맞지 않는다(fail-closed).
    auto confused =
        TryClientSessionKeys(bob, alice.PublicKey(), AeadAlgorithm::ChaCha20Poly1305Ietf);
    REQUIRE(confused);
    CHECK(hazmat::TryOpenWithNonce(
              confused.Value().receive, Span<const uint8>(nonce), Span<const uint8>(),
              Span<const uint8>(sealed).Left(written.Value()), Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);

    // 양쪽이 모두 서버로 착각해도 키가 맞지 않는다.
    auto aliceServer =
        TryServerSessionKeys(alice, bob.PublicKey(), AeadAlgorithm::ChaCha20Poly1305Ietf);
    REQUIRE(aliceServer);
    uint8 serverSealed[64];
    auto serverWritten = hazmat::TrySealWithNonce(aliceServer.Value().transmit,
                                                  Span<const uint8>(nonce), Span<const uint8>(),
                                                  Text("both servers"), Span<uint8>(serverSealed));
    REQUIRE(serverWritten);
    CHECK(hazmat::TryOpenWithNonce(
              server.Value().receive, Span<const uint8>(nonce), Span<const uint8>(),
              Span<const uint8>(serverSealed).Left(serverWritten.Value()), Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);

    // 32 byte 키 알고리즘이면 키 byte는 알고리즘과 관계없이 crypto_kx 출력과 같다.
    const AeadAlgorithm algorithms[] = {AeadAlgorithm::XChaCha20Poly1305, AeadAlgorithm::Aes256Gcm,
                                        AeadAlgorithm::Aegis256};
    for (AeadAlgorithm algorithm : algorithms) {
        auto keys = TryClientSessionKeys(alice, bob.PublicKey(), algorithm);
        if (!IsAvailable(algorithm)) {
            CHECK(keys.Error() == ErrorCode::Unsupported);
            continue;
        }

        REQUIRE(keys);
        CHECK(keys.Value().transmit.Algorithm() == algorithm);
        CHECK(KeyEquals(keys.Value().receive, ClientReceive));
        CHECK(KeyEquals(keys.Value().transmit, ClientTransmit));

        // 같은 byte로 가져온 키와 같은 결과를 낸다(AES-256-GCM 키 확장 상태 포함).
        auto imported = AeadKey::TryImport(algorithm, HexBytes(ClientTransmit).View());
        REQUIRE(imported);
        const usize nonceBytes = TryGetAeadInfo(algorithm).Value().nonceBytes;
        const uint8 zeroNonce[32] = {};
        uint8 fromKx[64];
        uint8 fromImport[64];
        auto a = hazmat::TrySealWithNonce(
            keys.Value().transmit, Span<const uint8>(zeroNonce).Left(nonceBytes),
            Span<const uint8>(), Text("same key"), Span<uint8>(fromKx));
        auto b = hazmat::TrySealWithNonce(
            imported.Value(), Span<const uint8>(zeroNonce).Left(nonceBytes), Span<const uint8>(),
            Text("same key"), Span<uint8>(fromImport));
        REQUIRE(a && b);
        CHECK(ConstantTimeEquals(Span<const uint8>(fromKx).Left(a.Value()),
                                 Span<const uint8>(fromImport).Left(b.Value())));
    }
}

TEST(CryptoKxRejectsMisuse) {
    const KxKeyPair alice = Import<KxKeyPair>(AliceSecret);
    const KxPublicKey bob = KxPublicKey::TryFromBytes(HexBytes(BobPublic).View()).Value();

    // 빈 키 쌍과 알고리즘 검사다. 키가 16 byte인 알고리즘(AES-128-GCM·AEGIS-128L)은 HW 유무와
    // 관계없이 InvalidArgument다.
    CHECK(TryClientSessionKeys(KxKeyPair(), bob, AeadAlgorithm::XChaCha20Poly1305).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryServerSessionKeys(KxKeyPair(), bob, AeadAlgorithm::XChaCha20Poly1305).Error() ==
          ErrorCode::StaleHandle);
    CHECK(TryClientSessionKeys(alice, bob, static_cast<AeadAlgorithm>(0)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryClientSessionKeys(alice, bob, AeadAlgorithm::Aes128Gcm).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(TryClientSessionKeys(alice, bob, AeadAlgorithm::Aegis128L).Error() ==
          ErrorCode::InvalidArgument);

    // 상대 공개키가 small-order 점이면 공유 비밀이 0이라 AuthenticationFailed다: u = 0, u = 1(위수
    // 4), 위수 8 점, 최상위 bit만 켠 0(X25519는 최상위 bit를 무시한다).
    const char* smallOrder[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
        "0000000000000000000000000000000000000000000000000000000000000080",
    };
    for (const char* hex : smallOrder) {
        const KxPublicKey peer = KxPublicKey::TryFromBytes(HexBytes(hex).View()).Value();
        CHECK(TryClientSessionKeys(alice, peer, AeadAlgorithm::XChaCha20Poly1305).Error() ==
              ErrorCode::AuthenticationFailed);
        CHECK(TryServerSessionKeys(alice, peer, AeadAlgorithm::XChaCha20Poly1305).Error() ==
              ErrorCode::AuthenticationFailed);
    }

    // 길이 검사다.
    uint8 bytes[33] = {};
    CHECK(KxKeyPair::TryImport(Span<const uint8>(bytes).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(KxKeyPair::TryImport(Span<const uint8>(bytes)).Error() == ErrorCode::InvalidArgument);
    CHECK(KxPublicKey::TryFromBytes(Span<const uint8>(bytes)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(BoxPublicKey::TryFromBytes(Span<const uint8>(bytes).Left(31)).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(CryptoKxKeyPairLifetime) {
    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        auto first = KxKeyPair::TryGenerate();
        auto second = KxKeyPair::TryGenerate();
        REQUIRE(first && second);
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
        CHECK(!(first.Value().PublicKey() == second.Value().PublicKey()));

        // 내보낸 비밀키로 다시 만든 키 쌍은 같은 공개키다.
        uint8 secret[33];
        REQUIRE(hazmat::TryExportKey(first.Value(), Span<uint8>(secret).Left(32)));
        CHECK(hazmat::TryExportKey(first.Value(), Span<uint8>(secret)).Error() ==
              ErrorCode::InvalidArgument);
        auto restored = KxKeyPair::TryImport(Span<const uint8>(secret).Left(32));
        SecureZero(Span<uint8>(secret));
        REQUIRE(restored);
        CHECK(restored.Value().PublicKey() == first.Value().PublicKey());

        // 이동한 키 쌍은 원본이 비어 StaleHandle이다.
        KxKeyPair moved = Move(first.Value());
        CHECK(first.Value().IsEmpty());
        CHECK(first.Value().PublicKey() == KxPublicKey());
        CHECK(!(moved.PublicKey() == KxPublicKey()));
        CHECK(hazmat::TryExportKey(first.Value(), Span<uint8>(secret).Left(32)).Error() ==
              ErrorCode::StaleHandle);
        auto keys = TryClientSessionKeys(moved, second.Value().PublicKey(),
                                         AeadAlgorithm::XChaCha20Poly1305);
        auto peer = TryServerSessionKeys(second.Value(), moved.PublicKey(),
                                         AeadAlgorithm::XChaCha20Poly1305);
        REQUIRE(keys && peer);
        uint8 a[32];
        uint8 b[32];
        REQUIRE(hazmat::TryExportKey(keys.Value().transmit, Span<uint8>(a)));
        REQUIRE(hazmat::TryExportKey(peer.Value().receive, Span<uint8>(b)));
        CHECK(ConstantTimeEquals(Span<const uint8>(a), Span<const uint8>(b)));
    }

    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoSealedBoxKnownAnswer) {
    // libsodium과 독립으로 조립한 sealed box를 연다(형식 호환).
    const BoxKeyPair bob = Import<BoxKeyPair>(BobSecret);
    REQUIRE(!bob.IsEmpty());
    CHECK(Equals(bob.PublicKey().View(), BobPublic));

    const HexBytes sealed(SealedForBob);
    REQUIRE(sealed.size == 14 + SealedBoxOverheadBytes);
    uint8 opened[32];
    auto openedBytes = TryOpenSealed(bob, sealed.View(), Span<uint8>(opened));
    REQUIRE(openedBytes);
    CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(openedBytes.Value()),
                             Text("sealed for bob")));

    const HexBytes empty(SealedEmptyForBob);
    REQUIRE(empty.size == SealedBoxOverheadBytes);
    auto emptyOpened = TryOpenSealed(bob, empty.View(), Span<uint8>());
    REQUIRE(emptyOpened);
    CHECK(emptyOpened.Value() == 0);

    // 다른 수신자는 열지 못한다. kx 키 쌍으로는 sealed box를 열 수 없다(타입이 다르다, 아래
    // static_assert).
    const BoxKeyPair alice = Import<BoxKeyPair>(AliceSecret);
    memset(opened, 0xAA, sizeof(opened));
    CHECK(TryOpenSealed(alice, sealed.View(), Span<uint8>(opened)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(opened)));
}

TEST(CryptoSealedBoxRoundTripAndTampering) {
    auto recipient = BoxKeyPair::TryGenerate();
    REQUIRE(recipient);
    const BoxPublicKey publicKey = recipient.Value().PublicKey();
    const Span<const uint8> message = Text("anonymous report");
    REQUIRE(TryGetSealedBoxBytes(message.Size()).Value() == message.Size() + 48);
    CHECK(TryGetSealedBoxBytes(MaxSize - 47).Error() == ErrorCode::Overflow);
    CHECK(TryGetSealedBoxBytes(MaxSize - 48).Value() == MaxSize);

    uint8 sealed[128] = {};
    auto written = TrySealToRecipient(publicKey, message, Span<uint8>(sealed));
    REQUIRE(written);
    REQUIRE(written.Value() == message.Size() + SealedBoxOverheadBytes);
    const Span<const uint8> good = Span<const uint8>(sealed).Left(written.Value());

    uint8 opened[128];
    auto openedBytes = TryOpenSealed(recipient.Value(), good, Span<uint8>(opened));
    REQUIRE(openedBytes);
    CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(openedBytes.Value()), message));

    // 임시 키 쌍이 매번 새것이라 같은 메시지도 결과가 다르다.
    uint8 again[128];
    REQUIRE(TrySealToRecipient(publicKey, message, Span<uint8>(again)));
    CHECK(!ConstantTimeEquals(Span<const uint8>(again).Left(32), good.Left(32)));

    // 임시 공개키·tag·암호문 어느 byte를 바꿔도, 잘라도 실패하고 plaintext 전체를 지운다.
    const auto expectRejected = [&](Span<const uint8> input) {
        memset(opened, 0xAA, sizeof(opened));
        CHECK(TryOpenSealed(recipient.Value(), input, Span<uint8>(opened)).Error() ==
              ErrorCode::AuthenticationFailed);
        CHECK(AllZero(Span<const uint8>(opened)));
    };
    const usize positions[] = {0, 31, 32, 47, 48, written.Value() - 1};
    for (usize position : positions) {
        uint8 tampered[128];
        memcpy(tampered, sealed, written.Value());
        tampered[position] ^= 0x01;
        expectRejected(Span<const uint8>(tampered).Left(written.Value()));
    }

    expectRejected(good.Left(written.Value() - 1));
    expectRejected(good.Left(47));
    expectRejected(Span<const uint8>());

    // 제자리: 평문을 출력의 임시 공개키 자리 뒤에 두고 봉인하고, 같은 자리로 연다.
    uint8 buffer[128];
    memcpy(buffer + 32, message.Data(), message.Size());
    auto inPlace = TrySealToRecipient(publicKey, Span<const uint8>(buffer).Mid(32, message.Size()),
                                      Span<uint8>(buffer));
    REQUIRE(inPlace);
    auto inPlaceOpened =
        TryOpenSealed(recipient.Value(), Span<const uint8>(buffer).Left(inPlace.Value()),
                      Span<uint8>(buffer).RightChop(32));
    REQUIRE(inPlaceOpened);
    CHECK(ConstantTimeEquals(Span<const uint8>(buffer).Mid(32, message.Size()), message));

    // 규칙 밖 겹침은 쓰기 전에 InvalidArgument다. 겹침은 길이 검사보다 먼저다.
    uint8 overlap[128];
    memcpy(overlap, message.Data(), message.Size());
    CHECK(TrySealToRecipient(publicKey, Span<const uint8>(overlap).Left(message.Size()),
                             Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    memcpy(overlap, sealed, written.Value());
    CHECK(TryOpenSealed(recipient.Value(), Span<const uint8>(overlap).Left(written.Value()),
                        Span<uint8>(overlap))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(TryOpenSealed(recipient.Value(), Span<const uint8>(overlap).Left(20),
                        Span<uint8>(overlap).Mid(5, 10))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ConstantTimeEquals(Span<const uint8>(overlap).Left(written.Value()), good));

    // 출력이 작으면 CapacityExceeded이고 쓰지 않는다.
    uint8 small[128];
    memset(small, 0x5A, sizeof(small));
    CHECK(TrySealToRecipient(publicKey, message, Span<uint8>(small).Left(written.Value() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(small[0] == 0x5A && small[written.Value() - 2] == 0x5A);
    CHECK(TryOpenSealed(recipient.Value(), good, Span<uint8>(small).Left(message.Size() - 1))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(small[0] == 0x5A);

    // small-order 수신자 공개키(u = 1, 위수 4)로는 봉인하지 않는다. libsodium이 쓴 임시 공개키
    // 자리만 지우고 나머지는 쓰지 않는다.
    const BoxPublicKey orderFour =
        BoxPublicKey::TryFromBytes(
            HexBytes("0100000000000000000000000000000000000000000000000000000000000000").View())
            .Value();
    memset(small, 0x5A, sizeof(small));
    CHECK(TrySealToRecipient(orderFour, message, Span<uint8>(small)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(small).Left(32)));
    bool untouched = true;
    for (usize i = 32; i < sizeof(small); ++i) {
        untouched = untouched && small[i] == 0x5A;
    }

    CHECK(untouched);

    // 빈 키 쌍과 내보내기다.
    CHECK(TryOpenSealed(BoxKeyPair(), good, Span<uint8>(opened)).Error() == ErrorCode::StaleHandle);
    uint8 secret[32];
    CHECK(hazmat::TryExportKey(BoxKeyPair(), Span<uint8>(secret)).Error() ==
          ErrorCode::StaleHandle);
    REQUIRE(hazmat::TryExportKey(recipient.Value(), Span<uint8>(secret)));
    auto restored = BoxKeyPair::TryImport(Span<const uint8>(secret));
    SecureZero(Span<uint8>(secret));
    REQUIRE(restored);
    CHECK(restored.Value().PublicKey() == publicKey);
    CHECK(TryOpenSealed(restored.Value(), good, Span<uint8>(opened)));
}

TEST(CryptoSealedBoxInPlaceAndEdgeCases) {
    auto recipient = BoxKeyPair::TryGenerate();
    REQUIRE(recipient);
    const BoxPublicKey publicKey = recipient.Value().PublicKey();

    // 제자리 봉인·열기를 libsodium secretbox의 겹침 처리 경계(16·17 byte, block0 32·33 byte)를
    // 지나는 길이로 확인한다. 빈 메시지도 포함한다.
    const usize lengths[] = {0, 1, 16, 17, 32, 33, 100};
    for (usize length : lengths) {
        uint8 message[100];
        for (usize i = 0; i < length; ++i) {
            message[i] = static_cast<uint8>(i * 13 + 5);
        }

        uint8 buffer[160] = {};
        memcpy(buffer + 32, message, length);
        auto sealed = TrySealToRecipient(publicKey, Span<const uint8>(buffer).Mid(32, length),
                                         Span<uint8>(buffer));
        REQUIRE(sealed);
        CHECK(sealed.Value() == length + SealedBoxOverheadBytes);
        auto opened =
            TryOpenSealed(recipient.Value(), Span<const uint8>(buffer).Left(sealed.Value()),
                          Span<uint8>(buffer).RightChop(32));
        REQUIRE(opened);
        CHECK(opened.Value() == length);
        CHECK(ConstantTimeEquals(Span<const uint8>(buffer).Mid(32, length),
                                 Span<const uint8>(message).Left(length)));
    }

    // 제자리 시작 주소가 한 byte라도 어긋나면 겹침이다.
    uint8 shifted[96] = {};
    CHECK(
        TrySealToRecipient(publicKey, Span<const uint8>(shifted).Mid(31, 10), Span<uint8>(shifted))
            .Error() == ErrorCode::InvalidArgument);
    CHECK(
        TrySealToRecipient(publicKey, Span<const uint8>(shifted).Mid(33, 10), Span<uint8>(shifted))
            .Error() == ErrorCode::InvalidArgument);

    // 제자리 열기가 실패하면(변조) 평문 자리 전체를 지운다.
    uint8 buffer[96] = {};
    memcpy(buffer + 32, "in place", 8);
    auto sealed =
        TrySealToRecipient(publicKey, Span<const uint8>(buffer).Mid(32, 8), Span<uint8>(buffer));
    REQUIRE(sealed);
    buffer[sealed.Value() - 1] ^= 0x01;
    CHECK(TryOpenSealed(recipient.Value(), Span<const uint8>(buffer).Left(sealed.Value()),
                        Span<uint8>(buffer).RightChop(32))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(buffer).RightChop(32)));

    // 열기: 임시 공개키가 small-order(0, 1)면 거부한다. 임시 공개키의 최상위 bit는 X25519가
    // 무시하지만 nonce(BLAKE2b(epk ‖ pk))에 들어가므로 바꾸면 거부된다.
    uint8 good[64];
    auto written = TrySealToRecipient(publicKey, Text("edge"), Span<uint8>(good));
    REQUIRE(written);
    uint8 opened[16];
    const uint8 smallOrder[2][32] = {{0}, {1}};
    for (const auto& epk : smallOrder) {
        uint8 forged[64];
        memcpy(forged, good, written.Value());
        memcpy(forged, epk, 32);
        CHECK(TryOpenSealed(recipient.Value(), Span<const uint8>(forged).Left(written.Value()),
                            Span<uint8>(opened))
                  .Error() == ErrorCode::AuthenticationFailed);
    }

    uint8 highBit[64];
    memcpy(highBit, good, written.Value());
    highBit[31] ^= 0x80;
    CHECK(TryOpenSealed(recipient.Value(), Span<const uint8>(highBit).Left(written.Value()),
                        Span<uint8>(opened))
              .Error() == ErrorCode::AuthenticationFailed);

    // small-order 수신자로의 제자리 봉인이 실패해도 평문 자리는 그대로이고 임시 공개키 자리만
    // 지운다.
    const BoxPublicKey zero = BoxPublicKey();
    uint8 keep[64];
    memset(keep, 0x5A, sizeof(keep));
    memcpy(keep + 32, "keep me", 7);
    CHECK(TrySealToRecipient(zero, Span<const uint8>(keep).Mid(32, 7), Span<uint8>(keep)).Error() ==
          ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(keep).Left(32)));
    CHECK(ConstantTimeEquals(Span<const uint8>(keep).Mid(32, 7), Text("keep me")));
}
