#include <Base/Endian.h>
#include <Crypto/Bytes.h>
#include <Crypto/Channel.h>
#include <Crypto/KeyId.h>
#include <Crypto/Private/ChannelInternal.h>
#include <Crypto/Private/ChannelTestAccess.h>
#include <Crypto/Secret.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
namespace channel_tests_file {

using namespace rocks::crypto;

// 보안 채널 부품 시험이다(Docs/Design/SecureChannel.ko.md 3절). 알려진 답의 출처:
// - Noise 공식 형식(cacophony) 벡터의 Noise_NK_25519_ChaChaPoly_SHA256 항목 두 개:
//   https://raw.githubusercontent.com/haskell-cryptography/cacophony/master/vectors/cacophony.txt
//   (commit 18b7348c54fd61fcd0c220298883de0d09c8364d, 파일 SHA-256 3bde7c09a6f349ee…9386cae)와
//   https://raw.githubusercontent.com/mcginty/snow/main/tests/vectors/snow.txt
//   (commit d00b360cc61a7fe519ce7539974dca4f36c4654a, 파일 SHA-256 69da433305fd045f…cd402fd3).
//   두 항목 모두 Python noiseprotocol 0.3.1로 다시 만들어 같은 byte임을 확인했다. 메시지 3번부터는
//   transport(빈 AD, n = 0, 1, …)라 빈 header의 datagram 봉인과 같다.
// - 상호운용 transcript A·B: Python noiseprotocol 0.3.1(cryptography 50.0.2 backend)이 고정 키로
// 만든
//   handshake와 transport다. header(AD)가 있는 datagram은 noiseprotocol CipherState(set_nonce,
//   encrypt_with_ad)로 만들었고 PyNaCl 1.6.2 crypto_aead_chacha20poly1305_ietf_encrypt로 같은
//   byte임을 확인했다. A의 prologue는 SecureChannel 4.1절 꼴("rocks.channel.v1" ‖ type ‖ version ‖
//   keyId), header는 transport 꼴(type 4 ‖ receiverIndex ‖ counter)이다.
namespace {

struct HexBytes {
    uint8 bytes[1024];
    usize size = 0;

    explicit HexBytes(const char* hex) {
        auto decoded = TryFromHex(StringView(hex), Span<uint8>(bytes));
        size = decoded ? decoded.Value() : MaxSize;
    }

    Span<const uint8> View() const {
        return Span<const uint8>(bytes, size == MaxSize ? 0 : size);
    }
};

bool Equals(Span<const uint8> actual, const char* hex) {
    const HexBytes expected(hex);
    return expected.size != MaxSize && actual.Size() == expected.size &&
           ConstantTimeEquals(actual, expected.View());
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

KeyId FixedKeyId() {
    KeyId id;
    id.value.high = 0x0102030405060708ull;
    id.value.low = 0x090a0b0c0d0e0f10ull;
    return id;
}

// transport 한 건이다. fromInitiator면 클라이언트 send → 서버 receive다.
struct TransportCase {
    bool fromInitiator;
    uint64 counter;
    const char* header;
    const char* payload;
    const char* sealed;
};

struct NoiseVector {
    const char* responderStatic;
    const char* responderPublic;
    const char* initiatorEphemeral;
    const char* responderEphemeral;
    const char* prologue;
    const char* payload1;
    const char* message1;
    const char* payload2;
    const char* message2;
    // 빈 문자열이면 출처에 값이 없다(snow).
    const char* handshakeHash;
    const TransportCase* transport;
    usize transportCount;
};

constexpr const char* CacophonyResponderStatic =
    "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893";
constexpr const char* CacophonyResponderPublic =
    "31e0303fd6418d2f8c0e78b91f22e8caed0fbe48656dcf4767e4834f701b8f62";
constexpr const char* CacophonyInitiatorEphemeral =
    "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a";
constexpr const char* CacophonyResponderEphemeral =
    "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
constexpr const char* CacophonyPrologue = "4a6f686e2047616c74";
constexpr const char* CacophonyPayload1 = "4c756477696720766f6e204d69736573";
constexpr const char* CacophonyMessage1 =
    "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79448134d00711fdb390"
    "a0d178fa008f6d47d2891e5ea18ae136c3b4c23ac384efb0";
constexpr const char* CacophonyPayload2 = "4d757272617920526f746862617264";
constexpr const char* CacophonyMessage2 =
    "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f1448088438ea16e3701bc0d77"
    "744f117bee22451c9afa7f4cdbbcff00c04a8ee0913c88";
constexpr const char* CacophonyHandshakeHash =
    "2efa38a9c7c93ac98f3a097af25c2f58b9e7673787717bc27e98827118c2c1a5";

const TransportCase CacophonyTransport[] = {
    {true, 0u, "", "462e20412e20486179656b",
     "a62de29ce27cb80245d440d986ed816c156e9d757d7008df2198b0"},
    {false, 0u, "", "4361726c204d656e676572",
     "174a35f11c689f4530d7208618e0564ae12f2f50ba8eb4df5382ff"},
    {true, 1u, "", "4a65616e2d426170746973746520536179",
     "337e475ebb8eae60f91974c4e455a5af38d1d8628d1803b160d60442874b0a1777"},
    {false, 1u, "", "457567656e2042f6686d20766f6e2042617765726b",
     "047e80e060b7bb08b53c5a23dfe9920cae135b9d1dc6302fc475003062723700366346ac9d"},
};

constexpr const char* SnowResponderStatic =
    "eb80faf5ebe8437d8edacb3bd95d0718a4eb3e5de6f835bca4d7e161301a563e";
constexpr const char* SnowResponderPublic =
    "a1685fa10096ad890ba2427ef08d85c44ffe7b1990ebb6ed7555f57b248b9319";
constexpr const char* SnowInitiatorEphemeral =
    "1b0c2affa2a0ba57c3d96a2b5def1671aa00f66a46d306618a2546a6757f5f65";
constexpr const char* SnowResponderEphemeral =
    "908f7161432df9036325367c1592ca275fea9a44cdf70d4f3135e16d0034dedc";
constexpr const char* SnowPrologue =
    "5468657265206973206e6f20726967687420616e642077726f6e672e2054686572652773206f6e6c"
    "792066756e20616e6420626f72696e672e";
constexpr const char* SnowPayload1 =
    "6b2da7554f3d4b8a27f371a0b2fb0d1b9c4830ea29e804b05d698f3f302740f4";
constexpr const char* SnowMessage1 =
    "1c83e1721fadb9fc168e67da9d25503aa286e42d7d35b80c73096d29cce503096ee5516c7bd399fb"
    "1f8b0f2affa71e262626c7eb957069a46177f86530217aac9d18bbb78c297d3de7b651995c6783cc";
constexpr const char* SnowPayload2 =
    "0216bdac2fb32a7c53d9c4711e3ed995c2e856d19fb1ed465cc88efd4f347e6f";
constexpr const char* SnowMessage2 =
    "d514c7509ba675f32aaaaef759846609383fb36f6081a3aed84737c800a3127c66ebfa61ba8ee2a5"
    "8b5074e40e3c0ce02ac02c30f3b6afd9ace9fcc4494485e7e3e545d05f5e9f1336ab5cd9f0d8eb54";
constexpr const char* SnowHandshakeHash = "";

const TransportCase SnowTransport[] = {
    {true, 0u, "", "0497dfc21ec4d70f04ffc7094b9467808c043d027c87d410b62e1af3dc791dce",
     "5bd6fb7c4fd75dd4d15b6600c8df4a59509709a97affb19f4e4a72e62dc8459f89361ae710a9fd58"
     "545eee4ceb37ec01"},
    {false, 0u, "", "874ba1f2539061f556a212f83c7a96faf33a451aa75099ef8a9b2aeb54c48f36",
     "1f7b7dd2a4992441bc91b6996b0079c3b0747e5aa8dc2443b1fe01331d57644647bbef13cda29f0a"
     "bc9836739df52fff"},
};

constexpr const char* PythonAResponderStatic =
    "0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20";
constexpr const char* PythonAResponderPublic =
    "07a37cbc142093c8b755dc1b10e86cb426374ad16aa853ed0bdfc0b2b86d1c7c";
constexpr const char* PythonAInitiatorEphemeral =
    "2122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f40";
constexpr const char* PythonAResponderEphemeral =
    "4142434445464748494a4b4c4d4e4f505152535455565758595a5b5c5d5e5f60";
constexpr const char* PythonAPrologue =
    "726f636b732e6368616e6e656c2e76310101000000a0a1a2a3a4a5a6a7a8a9aaabacadaeaf";
constexpr const char* PythonAPayload1 = "";
constexpr const char* PythonAMessage1 =
    "5869aff450549732cbaaed5e5df9b30a6da31cb0e5742bad5ad4a1a768f1a67b396274689612cb89"
    "84beb2353faa02fe";
constexpr const char* PythonAPayload2 = "07000000";
constexpr const char* PythonAMessage2 =
    "64b101b1d0be5a8704bd078f9895001fc03e8e9f9522f188dd128d9846d48466a3799f72c96587e0"
    "f3ff3d1a666bb6bcf57b9b71";
constexpr const char* PythonAHandshakeHash =
    "6d4185ae196503f75d03cee753a60f1a313b0242026942807523d6d92dcbdb49";

const TransportCase PythonATransport[] = {
    {true, 0u, "04070000000000000000000000", "68656c6c6f20736572766572",
     "b6c2417a17e63f67bf874adc3eba1a002d8182b1bd674c26e6d6ab17"},
    {true, 1u, "04070000000100000000000000",
     "010e1b2835424f5c697683909daab7c4d1deebf805121f2c394653606d7a8794a1aebbc8d5e2effc"
     "091623303d4a5764717e8b98a5b2bfccd9e6f3000d1a2734414e5b6875828f9ca9b6c3d0ddeaf704"
     "111e2b3845525f6c798693a0adbac7d4e1eefb0815222f3c495663707d8a97a4b1becbd8e5f2ff0c"
     "192633404d5a6774818e9ba8b5c2cfdce9f603101d2a3744515e6b7885929facb9c6d3e0edfa0714"
     "212e3b4855626f7c8996a3b0bdcad7e4f1fe0b1825323f4c596673808d9aa7b4c1cedbe8f5020f1c",
     "93c62b07df560ac83f1dff80e402020bb36fde5de23f1fb40c3b65c9822637a3ef19d4c36e712fd5"
     "5a6358a78fac11837b7cf1932b7de354fd9644a1557bf8e9dbb4ea02383d6332871cb28b908de08e"
     "b20f92b80f2d532a3913eab1edd1e39a87470508897bc901c6d3f36c500b933a6b7779c78da000c3"
     "94f02bb138aab76dcd5ba6d6f09976373d0d79cf7cd84d939d90458809eb26e71496247233aae2e6"
     "67d9bf0f645a6418234e310dcf90d487c80889f622aa8d620bf289867167afefca806a9dc79a07bc"
     "f31e6f8d243233986cb4bd221645bbdd"},
    {false, 0u, "04090000000000000000000000", "68656c6c6f20636c69656e74",
     "8f5d28875af52e3228624ad21ec5e7d1db7d5e6e5dd2dc06b0d35fb4"},
    {true, 4294967301u, "04070000000500000001000000", "65646765",
     "a8b9d34e57e297e529cd5dc1ba3166c093e67107"},
    {false, 3u, "", "", "cb1f32b4feff2f448f4399381a0ed0e3"},
    {false, 9223372036854775807u, "0409000000ffffffffffffff7f", "6c617374",
     "f4478296573a449059849738df55cab7d4e3a696"},
};

constexpr const char* PythonBResponderStatic =
    "c0acbe26605e6a7c773e2d309771959ee2ee607b9a6b7884e12854b4f18fe2b8";
constexpr const char* PythonBResponderPublic =
    "c3609fedc9632bd9a57a095581d437575e1c5c56e64cf31308505d5fc1a1604c";
constexpr const char* PythonBInitiatorEphemeral =
    "409fd4e92b57c6fcc796d82d6691ba43278f0de5e8767c126e1d47cfe8294f2e";
constexpr const char* PythonBResponderEphemeral =
    "27cc162b3374aa6e544fc759576bb61a16e1aedae0a3be2c126e2a31c6771230";
constexpr const char* PythonBPrologue = "";
constexpr const char* PythonBPayload1 =
    "00070e151c232a31383f464d545b626970777e858c939aa1a8afb6bdc4cbd2d9e0e7eef5fc030a11"
    "181f262d343b424950575e656c737a81888f969da4abb2b9c0c7ced5dce3eaf1f8ff060d141b2229"
    "30373e454c535a61686f767d848b9299a0a7aeb5";
constexpr const char* PythonBMessage1 =
    "301f4d515888d0432cfe352d1fb385ba3445a9421a448d37915bdd53bd56f1231a5ae2b0fe6c3478"
    "adc124ee8b1a0a6432f34dc1049742c3cd844e68771b84e4a162b9b9de924871192cbfcc949b74a8"
    "981c550a917a2ca6e9e2d345084c2957bc17784fec857ddc42cdbbd960b095522146fd6b64cd85fd"
    "0a230f8d168f459f6eff75d1562cbb95afbccce45a74c64a40676c7c";
constexpr const char* PythonBPayload2 =
    "030e19242f3a45505b66717c87929da8b3bec9d4dfeaf5000b16212c37424d58636e79848f9aa5b0"
    "bbc6d1dce7f2fd08131e29343f4a55606b76818c97a2adb8c3ced9e4effa05101b26313c47525d68"
    "737e89949faab5c0cbd6e1ecf7020d18232e39444f5a65707b86919ca7b2bdc8d3dee9f4ff0a1520"
    "2b36414c57626d78838e99a4afbac5d0dbe6f1fc07121d28333e49545f6a75808b96a1acb7c2cdd8"
    "e3eef9040f1a25303b46515c67727d88939ea9b4bfcad5e0ebf6010c17222d38434e59646f7a8590"
    "9ba6b1bcc7d2dde8f3fe09141f2a35404b56616c77828d98a3aeb9c4cfdae5f0fb06111c27323d48"
    "535e69747f8a95a0abb6c1ccd7e2edf8030e19242f3a45505b66717c87929da8b3bec9d4dfeaf500"
    "0b16212c37424d58636e79848f9aa5b0bbc6d1dc";
constexpr const char* PythonBMessage2 =
    "2da22ab24052be1c7581caa45cf3cb313563ab12411ea4bcb8333bce2e88c932e2bd9a1707695caf"
    "801bef4e94e1fdf1fcb98afbe7b68ff10f95e382635f95cdb435566adafdcbb9c4dd4d3cf1339a50"
    "be4767e5b62231a61697b2852206a9a308e33830bd4a62109918f1eba72131c9ae341ba913e4f90b"
    "5ef19e8ce3513a403a1943eb7ce19591afe8b3794c884c17adb9438d7e5644af7373ee09d08b945d"
    "97b223bf66ba4ffb8bd4ea55f82e08f2f107809a5345d0c6c77a1ef88214653e219a4bfa4e06aee6"
    "5b4844e9d25a63473cd0d6e59731ac141ec74057c3b7ea77abcd4a72e5c8044d50c8ce2ec49d28d6"
    "ca78b83e038b4c668ac0df6142e468ebc1393dc9cefbb03290cbe12f19e6ec7aa447a657347d3b9e"
    "0ad74a9a9accb4af3cc37c41376c952cd7fe9e126f506645e78e0fb6e9b94c693978347b77e8d20c"
    "70fb748116de8f79eb61c1fc9a76571116f01293e50f3c739a936a39";
constexpr const char* PythonBHandshakeHash =
    "c61e489b73bf9ed0f8cefd58b17379a3071518bdb4b4defc701298e9d0a54654";

const TransportCase PythonBTransport[] = {
    {true, 0u, "", "6669727374", "904272ed1221dcaa6f21e70bee28ca5699cf95329b"},
    {false, 0u, "", "7365636f6e64", "d2404738fba6b55c8862cc6ed07a92f79bf17b73267c"},
    {true, 1u, "", "7468697264", "65123d4df08e5330b01a3d5c6906d5b3b44eeb2fed"},
};

const NoiseVector Vectors[] = {
    {CacophonyResponderStatic, CacophonyResponderPublic, CacophonyInitiatorEphemeral,
     CacophonyResponderEphemeral, CacophonyPrologue, CacophonyPayload1, CacophonyMessage1,
     CacophonyPayload2, CacophonyMessage2, CacophonyHandshakeHash, CacophonyTransport,
     sizeof(CacophonyTransport) / sizeof(CacophonyTransport[0])},
    {SnowResponderStatic, SnowResponderPublic, SnowInitiatorEphemeral, SnowResponderEphemeral,
     SnowPrologue, SnowPayload1, SnowMessage1, SnowPayload2, SnowMessage2, SnowHandshakeHash,
     SnowTransport, sizeof(SnowTransport) / sizeof(SnowTransport[0])},
    {PythonAResponderStatic, PythonAResponderPublic, PythonAInitiatorEphemeral,
     PythonAResponderEphemeral, PythonAPrologue, PythonAPayload1, PythonAMessage1, PythonAPayload2,
     PythonAMessage2, PythonAHandshakeHash, PythonATransport,
     sizeof(PythonATransport) / sizeof(PythonATransport[0])},
    {PythonBResponderStatic, PythonBResponderPublic, PythonBInitiatorEphemeral,
     PythonBResponderEphemeral, PythonBPrologue, PythonBPayload1, PythonBMessage1, PythonBPayload2,
     PythonBMessage2, PythonBHandshakeHash, PythonBTransport,
     sizeof(PythonBTransport) / sizeof(PythonBTransport[0])},
};

// 고정 ephemeral 키로 handshake를 끝까지 돌려 메시지·payload·hash·transport가 벡터와 같은지 본다.
void CheckVector(const NoiseVector& vector) {
    auto serverKey =
        ChannelServerKey::TryImport(FixedKeyId(), HexBytes(vector.responderStatic).View());
    REQUIRE(serverKey);
    CHECK(Equals(serverKey.Value().PublicKey().View(), vector.responderPublic));
    const HexBytes prologue(vector.prologue);
    auto initiator =
        NoiseHandshake::TryCreateInitiator(serverKey.Value().PublicKey(), prologue.View());
    auto responder = NoiseHandshake::TryCreateResponder(serverKey.Value(), prologue.View());
    REQUIRE(initiator && responder);
    CHECK(initiator.Value().IsInitiator() && !responder.Value().IsInitiator());

    static uint8 message[1024];
    static uint8 payload[1024];
    const HexBytes payload1(vector.payload1);
    auto first = crypto::detail::ChannelFactory::TryWriteMessage(
        initiator.Value(), HexBytes(vector.initiatorEphemeral).View(), payload1.View(),
        Span<uint8>(message));
    REQUIRE(first);
    CHECK(Equals(Span<const uint8>(message).Left(first.Value()), vector.message1));
    CHECK(initiator.Value().NextStep() == NoiseStep::ReadMessage);
    auto firstRead = responder.Value().TryReadMessage(
        Span<const uint8>(message).Left(first.Value()), Span<uint8>(payload));
    REQUIRE(firstRead);
    CHECK(ConstantTimeEquals(Span<const uint8>(payload).Left(firstRead.Value()), payload1.View()));
    CHECK(responder.Value().NextStep() == NoiseStep::WriteMessage);

    const HexBytes payload2(vector.payload2);
    auto second = crypto::detail::ChannelFactory::TryWriteMessage(
        responder.Value(), HexBytes(vector.responderEphemeral).View(), payload2.View(),
        Span<uint8>(message));
    REQUIRE(second);
    CHECK(Equals(Span<const uint8>(message).Left(second.Value()), vector.message2));
    auto secondRead = initiator.Value().TryReadMessage(
        Span<const uint8>(message).Left(second.Value()), Span<uint8>(payload));
    REQUIRE(secondRead);
    CHECK(ConstantTimeEquals(Span<const uint8>(payload).Left(secondRead.Value()), payload2.View()));
    CHECK(initiator.Value().NextStep() == NoiseStep::Split);
    CHECK(responder.Value().NextStep() == NoiseStep::Split);

    auto client = initiator.Value().TrySplit();
    auto server = responder.Value().TrySplit();
    REQUIRE(client && server);
    CHECK(initiator.Value().NextStep() == NoiseStep::Done);
    CHECK(ConstantTimeEquals(Span<const uint8>(client.Value().handshakeHash),
                             Span<const uint8>(server.Value().handshakeHash)));
    if (vector.handshakeHash[0] != '\0') {
        CHECK(Equals(Span<const uint8>(client.Value().handshakeHash), vector.handshakeHash));
    }

    for (usize index = 0; index < vector.transportCount; ++index) {
        const TransportCase& entry = vector.transport[index];
        DatagramSealer& sealer = entry.fromInitiator ? client.Value().send : server.Value().send;
        DatagramOpener& opener =
            entry.fromInitiator ? server.Value().receive : client.Value().receive;
        if (sealer.NextCounter() != entry.counter) {
            crypto::detail::ChannelTestAccess::SetNextCounter(sealer, entry.counter);
        }

        const HexBytes header(entry.header);
        const HexBytes plain(entry.payload);
        auto counter = sealer.TrySeal(header.View(), plain.View(), Span<uint8>(message));
        REQUIRE(counter);
        CHECK(counter.Value() == entry.counter);
        const Span<const uint8> sealed =
            Span<const uint8>(message).Left(plain.View().Size() + DatagramTagBytes);
        CHECK(Equals(sealed, entry.sealed));
        auto opened = opener.TryOpen(entry.counter, header.View(), sealed, Span<uint8>(payload));
        REQUIRE(opened);
        CHECK(ConstantTimeEquals(Span<const uint8>(payload).Left(opened.Value()), plain.View()));
    }
}

// 공개 API(OS 난수 ephemeral)로 handshake를 끝낸다. 실패하면 false다.
bool TryConnect(const ChannelServerKey& key, ChannelSession& client, ChannelSession& server) {
    auto initiator = NoiseHandshake::TryCreateInitiator(key.PublicKey(), Text("rocks test"));
    auto responder = NoiseHandshake::TryCreateResponder(key, Text("rocks test"));
    if (!initiator || !responder) {
        return false;
    }

    uint8 message[128];
    uint8 payload[64];
    auto hello = initiator.Value().TryWriteMessage(Text("hello"), Span<uint8>(message));
    if (!hello || !responder.Value().TryReadMessage(Span<const uint8>(message).Left(hello.Value()),
                                                    Span<uint8>(payload))) {
        return false;
    }

    auto welcome = responder.Value().TryWriteMessage(Text("welcome"), Span<uint8>(message));
    if (!welcome || !initiator.Value().TryReadMessage(
                        Span<const uint8>(message).Left(welcome.Value()), Span<uint8>(payload))) {
        return false;
    }

    auto clientSession = initiator.Value().TrySplit();
    auto serverSession = responder.Value().TrySplit();
    if (!clientSession || !serverSession) {
        return false;
    }

    client = Move(clientSession.Value());
    server = Move(serverSession.Value());
    return true;
}

ChannelServerKey GenerateServerKey() {
    auto id = TryNewKeyId();
    if (!id) {
        return ChannelServerKey();
    }

    auto key = ChannelServerKey::TryGenerate(id.Value());
    return key ? Move(key.Value()) : ChannelServerKey();
}

// 고정 키 handshake(transcript A)의 첫 메시지를 message에 쓴다. 실패하면 0이다.
usize WriteFixedHello(NoiseHandshake& initiator, Span<uint8> message) {
    auto written = crypto::detail::ChannelFactory::TryWriteMessage(
        initiator, HexBytes(PythonAInitiatorEphemeral).View(), Span<const uint8>(), message);
    return written ? written.Value() : 0;
}

} // namespace

TEST(CryptoChannelNoiseVectors) {
    for (const NoiseVector& vector : Vectors) {
        CheckVector(vector);
    }
}

TEST(CryptoChannelServerKey) {
    // Nil id, 길이 오류는 InvalidArgument다.
    CHECK(ChannelServerKey::TryGenerate(KeyId()).Error() == ErrorCode::InvalidArgument);
    uint8 secret[33] = {};
    CHECK(ChannelServerKey::TryImport(FixedKeyId(), Span<const uint8>(secret)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ChannelServerKey::TryImport(KeyId(), Span<const uint8>(secret).Left(32)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ChannelPublicKey::TryFromBytes(KeyId(), Span<const uint8>(secret).Left(32)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ChannelPublicKey::TryFromBytes(FixedKeyId(), Span<const uint8>(secret)).Error() ==
          ErrorCode::InvalidArgument);

    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        ChannelServerKey key = GenerateServerKey();
        REQUIRE(!key.IsEmpty());
        CHECK(GetSecretMemoryStatus().arenaUsedBytes > used);
        auto other = ChannelServerKey::TryGenerate(key.Id());
        REQUIRE(other);
        CHECK(!(other.Value().PublicKey() == key.PublicKey()));
        CHECK(key.PublicKey().id == key.Id());

        // 내보낸 비밀키로 다시 만든 키는 같은 공개키다.
        REQUIRE(hazmat::TryExportKey(key, Span<uint8>(secret).Left(32)));
        CHECK(hazmat::TryExportKey(key, Span<uint8>(secret)).Error() == ErrorCode::InvalidArgument);
        auto restored = ChannelServerKey::TryImport(key.Id(), Span<const uint8>(secret).Left(32));
        SecureZero(Span<uint8>(secret));
        REQUIRE(restored);
        CHECK(restored.Value().PublicKey() == key.PublicKey());
        auto parsed = ChannelPublicKey::TryFromBytes(key.Id(), key.PublicKey().View());
        REQUIRE(parsed);
        CHECK(parsed.Value() == key.PublicKey());

        // 옮겨 간 키는 비고 Nil id·0 공개키이며 StaleHandle이다.
        ChannelServerKey moved = Move(key);
        CHECK(key.IsEmpty() && key.Id().IsNil());
        CHECK(key.PublicKey() == ChannelPublicKey());
        CHECK(!moved.IsEmpty() && moved.PublicKey() == restored.Value().PublicKey());
        CHECK(hazmat::TryExportKey(key, Span<uint8>(secret).Left(32)).Error() ==
              ErrorCode::StaleHandle);
        CHECK(NoiseHandshake::TryCreateResponder(key, Span<const uint8>()).Error() ==
              ErrorCode::StaleHandle);
    }

    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoChannelHandshakeRoundTrip) {
    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    {
        const ChannelServerKey key = GenerateServerKey();
        REQUIRE(!key.IsEmpty());
        ChannelSession client;
        ChannelSession server;
        REQUIRE(TryConnect(key, client, server));
        CHECK(ConstantTimeEquals(Span<const uint8>(client.handshakeHash),
                                 Span<const uint8>(server.handshakeHash)));
        CHECK(!AllZero(Span<const uint8>(client.handshakeHash)));

        // 두 방향의 키가 다르고 역할이 묶인다: 클라이언트 send = 서버 receive.
        uint8 sealed[64];
        uint8 opened[64];
        auto counter = client.send.TrySeal(Text("hdr"), Text("ping"), Span<uint8>(sealed));
        REQUIRE(counter && counter.Value() == 0);
        CHECK(client.send.NextCounter() == 1);
        uint8 ping[20];
        memcpy(ping, sealed, sizeof(ping));
        CHECK(client.receive.TryOpen(0, Text("hdr"), Span<const uint8>(ping), Span<uint8>(opened))
                  .Error() == ErrorCode::AuthenticationFailed);
        auto pingOpened =
            server.receive.TryOpen(0, Text("hdr"), Span<const uint8>(ping), Span<uint8>(opened));
        REQUIRE(pingOpened && pingOpened.Value() == 4);
        CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(4), Text("ping")));
        auto pong = server.send.TrySeal(Text("hdr"), Text("pong"), Span<uint8>(sealed));
        REQUIRE(pong && pong.Value() == 0);
        CHECK(client.receive.TryOpen(0, Text("hdr"), Span<const uint8>(sealed).Left(20),
                                     Span<uint8>(opened)));
        CHECK(ConstantTimeEquals(Span<const uint8>(opened).Left(4), Text("pong")));

        // 같은 서버 키로 다시 연결하면 세션이 다르다(양쪽 ephemeral).
        ChannelSession again;
        ChannelSession againServer;
        REQUIRE(TryConnect(key, again, againServer));
        CHECK(!ConstantTimeEquals(Span<const uint8>(again.handshakeHash),
                                  Span<const uint8>(client.handshakeHash)));
        CHECK(againServer.receive
                  .TryOpen(0, Text("hdr"), Span<const uint8>(ping), Span<uint8>(opened))
                  .Error() == ErrorCode::AuthenticationFailed);
    }

    // 세션 키와 handshake 상태가 모두 키 저장소로 돌아갔다.
    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoChannelHandshakeOrderAndMisuse) {
    const ChannelServerKey key = GenerateServerKey();
    REQUIRE(!key.IsEmpty());
    CHECK(NoiseHandshake::TryCreateInitiator(ChannelPublicKey(), Span<const uint8>()).Error() ==
          ErrorCode::InvalidArgument);
    auto initiator = NoiseHandshake::TryCreateInitiator(key.PublicKey(), Text("p"));
    auto responder = NoiseHandshake::TryCreateResponder(key, Text("p"));
    REQUIRE(initiator && responder);
    CHECK(initiator.Value().NextStep() == NoiseStep::WriteMessage);
    CHECK(responder.Value().NextStep() == NoiseStep::ReadMessage);

    // 차례가 아닌 호출은 InvalidArgument이고 상태를 바꾸지 않는다.
    static uint8 message[NoiseMaxMessageBytes + 2];
    uint8 payload[256];
    CHECK(initiator.Value()
              .TryReadMessage(Span<const uint8>(message).Left(48), Span<uint8>(payload))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(initiator.Value().TrySplit().Error() == ErrorCode::InvalidArgument);
    CHECK(responder.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(responder.Value().TrySplit().Error() == ErrorCode::InvalidArgument);
    CHECK(initiator.Value().NextStep() == NoiseStep::WriteMessage);

    // 겹침·고정 ephemeral 길이 오류는 InvalidArgument, 출력 부족·payload 상한은 CapacityExceeded다.
    CHECK(
        initiator.Value()
            .TryWriteMessage(Span<const uint8>(message).Left(4), Span<uint8>(message).RightChop(2))
            .Error() == ErrorCode::InvalidArgument);
    CHECK(crypto::detail::ChannelFactory::TryWriteMessage(initiator.Value(),
                                                          Span<const uint8>(payload, 31),
                                                          Span<const uint8>(), Span<uint8>(message))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(initiator.Value().TryWriteMessage(Text("abc"), Span<uint8>(message).Left(50)).Error() ==
          ErrorCode::CapacityExceeded);
    static uint8 large[NoiseMaxPayloadBytes + 1];
    CHECK(
        initiator.Value().TryWriteMessage(Span<const uint8>(large), Span<uint8>(message)).Error() ==
        ErrorCode::CapacityExceeded);
    CHECK(initiator.Value().NextStep() == NoiseStep::WriteMessage);

    // 상한 그대로는 받는다(메시지 65535 byte). payload 버퍼가 작으면 CapacityExceeded이고 상태가
    // 그대로라 더 큰 버퍼로 다시 부를 수 있다.
    auto maxHello = NoiseHandshake::TryCreateInitiator(key.PublicKey(), Text("p"));
    REQUIRE(maxHello);
    auto maxWritten = maxHello.Value().TryWriteMessage(
        Span<const uint8>(large).Left(NoiseMaxPayloadBytes), Span<uint8>(message));
    REQUIRE(maxWritten && maxWritten.Value() == NoiseMaxMessageBytes);
    auto big = NoiseHandshake::TryCreateResponder(key, Text("p"));
    REQUIRE(big);
    CHECK(big.Value()
              .TryReadMessage(Span<const uint8>(message).Left(maxWritten.Value()),
                              Span<uint8>(payload))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(big.Value().NextStep() == NoiseStep::ReadMessage);
    auto bigRead = big.Value().TryReadMessage(Span<const uint8>(message).Left(maxWritten.Value()),
                                              Span<uint8>(large));
    REQUIRE(bigRead && bigRead.Value() == NoiseMaxPayloadBytes);

    auto hello = initiator.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message));
    REQUIRE(hello && hello.Value() == NoiseMessageOverheadBytes);
    CHECK(initiator.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message)).Error() ==
          ErrorCode::InvalidArgument);

    static uint8 helloCopy[NoiseMaxMessageBytes];
    memcpy(helloCopy, message, hello.Value());
    CHECK(responder.Value()
              .TryReadMessage(Span<const uint8>(helloCopy).Left(hello.Value()),
                              Span<uint8>(helloCopy).RightChop(hello.Value() - 1))
              .Error() == ErrorCode::InvalidArgument);
    auto read = responder.Value().TryReadMessage(Span<const uint8>(helloCopy).Left(hello.Value()),
                                                 Span<uint8>());
    REQUIRE(read && read.Value() == 0);
    auto welcome = responder.Value().TryWriteMessage(Text("w"), Span<uint8>(message));
    REQUIRE(welcome);
    CHECK(responder.Value().NextStep() == NoiseStep::Split);
    CHECK(
        responder.Value()
            .TryReadMessage(Span<const uint8>(helloCopy).Left(hello.Value()), Span<uint8>(payload))
            .Error() == ErrorCode::InvalidArgument);
    auto welcomeRead = initiator.Value().TryReadMessage(
        Span<const uint8>(message).Left(welcome.Value()), Span<uint8>(payload));
    REQUIRE(welcomeRead && welcomeRead.Value() == 1);

    // Split 뒤에는 모든 호출이 InvalidArgument다(한 번 쓰고 버린다).
    auto session = initiator.Value().TrySplit();
    REQUIRE(session);
    CHECK(initiator.Value().NextStep() == NoiseStep::Done);
    CHECK(initiator.Value().TrySplit().Error() == ErrorCode::InvalidArgument);
    CHECK(initiator.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(
        initiator.Value()
            .TryReadMessage(Span<const uint8>(message).Left(welcome.Value()), Span<uint8>(payload))
            .Error() == ErrorCode::InvalidArgument);

    // 옮겨 간 객체와 기본 객체는 StaleHandle이다.
    NoiseHandshake moved = Move(responder.Value());
    CHECK(responder.Value().IsEmpty() && responder.Value().NextStep() == NoiseStep::None);
    CHECK(responder.Value().TrySplit().Error() == ErrorCode::StaleHandle);
    CHECK(responder.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message)).Error() ==
          ErrorCode::StaleHandle);
    CHECK(responder.Value()
              .TryReadMessage(Span<const uint8>(message).Left(48), Span<uint8>(payload))
              .Error() == ErrorCode::StaleHandle);
    CHECK(moved.TrySplit());
    NoiseHandshake empty;
    CHECK(empty.TrySplit().Error() == ErrorCode::StaleHandle);
    CHECK(crypto::detail::ChannelFactory::TryWriteMessage(empty, Span<const uint8>(),
                                                          Span<const uint8>(), Span<uint8>(message))
              .Error() == ErrorCode::StaleHandle);
}

TEST(CryptoChannelHandshakeRejectsBadMessages) {
    auto serverKey =
        ChannelServerKey::TryImport(FixedKeyId(), HexBytes(PythonAResponderStatic).View());
    REQUIRE(serverKey);
    const usize used = GetSecretMemoryStatus().arenaUsedBytes;
    const HexBytes prologue(PythonAPrologue);
    const HexBytes hello(PythonAMessage1);
    const HexBytes welcome(PythonAMessage2);
    uint8 payload[64];
    uint8 message[256];

    // 받는 쪽은 message를 읽다 실패하면 payload를 지우고 Failed가 된다. 이후 모든 호출이
    // AuthenticationFailed다(sticky, 쓰기 출력도 지운다).
    const auto expectRejected = [&](Span<const uint8> input) {
        auto responder = NoiseHandshake::TryCreateResponder(serverKey.Value(), prologue.View());
        if (!responder) {
            return false;
        }

        memset(payload, 0xa5, sizeof(payload));
        const bool rejected =
            responder.Value().TryReadMessage(input, Span<uint8>(payload)).Error() ==
            ErrorCode::AuthenticationFailed;
        const bool zeroed = AllZero(Span<const uint8>(payload));
        memset(message, 0xa5, sizeof(message));
        return rejected && zeroed && responder.Value().NextStep() == NoiseStep::Failed &&
               responder.Value().TryReadMessage(hello.View(), Span<uint8>(payload)).Error() ==
                   ErrorCode::AuthenticationFailed &&
               responder.Value()
                       .TryWriteMessage(Span<const uint8>(), Span<uint8>(message))
                       .Error() == ErrorCode::AuthenticationFailed &&
               AllZero(Span<const uint8>(message)) &&
               responder.Value().TrySplit().Error() == ErrorCode::AuthenticationFailed;
    };

    // 원본은 열린다. 첫 메시지는 다시 보내도(replay) 다른 서버 객체가 받는다(SecureChannel 3.1절:
    // 첫 payload에는 비밀이나 한 번만 받아야 하는 값을 넣지 않는다).
    for (usize round = 0; round < 2; ++round) {
        auto responder = NoiseHandshake::TryCreateResponder(serverKey.Value(), prologue.View());
        REQUIRE(responder);
        CHECK(responder.Value().TryReadMessage(hello.View(), Span<uint8>(payload)));
    }

    // 모든 byte의 bit 하나, 자르기, 늘리기, 상한 초과.
    uint8 tampered[256];
    for (usize offset = 0; offset < hello.size; ++offset) {
        memcpy(tampered, hello.bytes, hello.size);
        tampered[offset] ^= 0x80;
        CHECK(expectRejected(Span<const uint8>(tampered).Left(hello.size)));
    }

    for (usize length = 0; length < hello.size; ++length) {
        CHECK(expectRejected(hello.View().Left(length)));
    }

    memcpy(tampered, hello.bytes, hello.size);
    tampered[hello.size] = 0;
    CHECK(expectRejected(Span<const uint8>(tampered).Left(hello.size + 1)));
    static uint8 tooLong[NoiseMaxMessageBytes + 1];
    CHECK(expectRejected(Span<const uint8>(tooLong)));

    // small-order ephemeral(u = 0, 1, p − 1)은 DH 결과가 0이라 거부한다.
    const char* const smallOrder[] = {
        "0000000000000000000000000000000000000000000000000000000000000000",
        "0100000000000000000000000000000000000000000000000000000000000000",
        "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
    };
    for (const char* point : smallOrder) {
        memcpy(tampered, hello.bytes, hello.size);
        memcpy(tampered, HexBytes(point).bytes, ChannelKeyBytes);
        CHECK(expectRejected(Span<const uint8>(tampered).Left(hello.size)));
    }

    // 다른 prologue와 다른 서버 키로 만든 첫 메시지는 거부한다.
    {
        auto other = NoiseHandshake::TryCreateInitiator(serverKey.Value().PublicKey(), Text("x"));
        REQUIRE(other);
        const usize written = WriteFixedHello(other.Value(), Span<uint8>(tampered));
        REQUIRE(written == hello.size);
        CHECK(expectRejected(Span<const uint8>(tampered).Left(written)));
    }

    {
        const ChannelServerKey wrongKey = GenerateServerKey();
        auto other = NoiseHandshake::TryCreateInitiator(wrongKey.PublicKey(), prologue.View());
        REQUIRE(other);
        const usize written = WriteFixedHello(other.Value(), Span<uint8>(tampered));
        REQUIRE(written == hello.size);
        CHECK(expectRejected(Span<const uint8>(tampered).Left(written)));
    }

    // 클라이언트가 가진 서버 공개키가 small-order 점이면 쓰기가 AuthenticationFailed이고 Failed다.
    {
        auto bad = ChannelPublicKey::TryFromBytes(FixedKeyId(), HexBytes(smallOrder[0]).View());
        REQUIRE(bad);
        auto initiator = NoiseHandshake::TryCreateInitiator(bad.Value(), prologue.View());
        REQUIRE(initiator);
        memset(message, 0xa5, sizeof(message));
        CHECK(
            initiator.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(message)).Error() ==
            ErrorCode::AuthenticationFailed);
        CHECK(AllZero(Span<const uint8>(message).Left(NoiseMessageOverheadBytes)));
        CHECK(initiator.Value().NextStep() == NoiseStep::Failed);
    }

    // 둘째 메시지: 변조·자르기·small-order e·다른 transcript의 응답은 클라이언트가 거부한다.
    // 거부해도 클라이언트는 읽기 전 상태로 돌아가 ReadMessage에 머물고, 뒤에 온 진짜 둘째 메시지를
    // 읽어 Split할 수 있다(경로 밖 공격자의 가짜 응답 하나로 handshake가 끝나지 않는다).
    const auto expectWelcomeRejected = [&](Span<const uint8> input) {
        auto initiator =
            NoiseHandshake::TryCreateInitiator(serverKey.Value().PublicKey(), prologue.View());
        if (!initiator || WriteFixedHello(initiator.Value(), Span<uint8>(message)) != hello.size) {
            return false;
        }

        memset(payload, 0xa5, sizeof(payload));
        if (initiator.Value().TryReadMessage(input, Span<uint8>(payload)).Error() !=
                ErrorCode::AuthenticationFailed ||
            !AllZero(Span<const uint8>(payload)) ||
            initiator.Value().NextStep() != NoiseStep::ReadMessage) {
            return false;
        }

        auto real = initiator.Value().TryReadMessage(welcome.View(), Span<uint8>(payload));
        return real && real.Value() == welcome.size - NoiseMessageOverheadBytes &&
               static_cast<bool>(initiator.Value().TrySplit());
    };

    for (usize offset = 0; offset < welcome.size; ++offset) {
        memcpy(tampered, welcome.bytes, welcome.size);
        tampered[offset] ^= 0x01;
        CHECK(expectWelcomeRejected(Span<const uint8>(tampered).Left(welcome.size)));
    }

    CHECK(expectWelcomeRejected(welcome.View().Left(welcome.size - 1)));
    CHECK(expectWelcomeRejected(welcome.View().Left(NoiseMessageOverheadBytes - 1)));
    for (const char* point : smallOrder) {
        memcpy(tampered, welcome.bytes, welcome.size);
        memcpy(tampered, HexBytes(point).bytes, ChannelKeyBytes);
        CHECK(expectWelcomeRejected(Span<const uint8>(tampered).Left(welcome.size)));
    }

    // 다른 서버 키는 첫 메시지를 열지 못하고, 다른 첫 메시지(다른 클라이언트의 Hello)에 대한 정상
    // 응답은 이 클라이언트가 받지 않는다(transcript가 다르다).
    {
        const ChannelServerKey wrongKey = GenerateServerKey();
        auto wrongServer = NoiseHandshake::TryCreateResponder(wrongKey, prologue.View());
        REQUIRE(wrongServer);
        CHECK(wrongServer.Value().TryReadMessage(hello.View(), Span<uint8>(payload)).Error() ==
              ErrorCode::AuthenticationFailed);
        auto otherClient =
            NoiseHandshake::TryCreateInitiator(serverKey.Value().PublicKey(), prologue.View());
        auto server = NoiseHandshake::TryCreateResponder(serverKey.Value(), prologue.View());
        REQUIRE(otherClient && server);
        auto otherHello =
            otherClient.Value().TryWriteMessage(Span<const uint8>(), Span<uint8>(tampered));
        REQUIRE(otherHello);
        REQUIRE(server.Value().TryReadMessage(Span<const uint8>(tampered).Left(otherHello.Value()),
                                              Span<uint8>(payload)));
        auto otherWelcome = server.Value().TryWriteMessage(Text("x"), Span<uint8>(tampered));
        REQUIRE(otherWelcome);
        CHECK(expectWelcomeRejected(Span<const uint8>(tampered).Left(otherWelcome.Value())));
    }

    // 실패한 handshake는 비밀 상태를 바로 지우고 돌려준다.
    CHECK(GetSecretMemoryStatus().arenaUsedBytes == used);
}

TEST(CryptoChannelDatagramWindow) {
    const ChannelServerKey key = GenerateServerKey();
    REQUIRE(!key.IsEmpty());
    ChannelSession client;
    ChannelSession server;
    REQUIRE(TryConnect(key, client, server));
    DatagramSealer& sealer = client.send;
    DatagramOpener& opener = server.receive;

    // counter 0..3099의 봉인(payload = LE64(counter), header = type 4 ‖ index 0 ‖ LE64(counter)).
    constexpr usize Count = 3100;
    static uint8 sealed[Count][24];
    uint8 header[13] = {4};
    uint8 payload[8];
    const auto headerFor = [&](uint64 counter) {
        StoreLittleEndian<uint64>(header + 5, counter);
        return Span<const uint8>(header);
    };
    const auto seal = [&](uint64 counter, Span<uint8> output) {
        uint8 plain[8];
        StoreLittleEndian<uint64>(plain, counter);
        crypto::detail::ChannelTestAccess::SetNextCounter(sealer, counter);
        auto used = sealer.TrySeal(headerFor(counter), Span<const uint8>(plain), output);
        return used && used.Value() == counter;
    };
    for (usize index = 0; index < Count; ++index) {
        REQUIRE(seal(index, Span<uint8>(sealed[index])));
    }

    // 결과: None(열고 payload가 맞음), 오류(payload가 모두 0), TypeMismatch(그 밖).
    const auto openSealed = [&](uint64 counter, Span<const uint8> input) {
        memset(payload, 0xa5, sizeof(payload));
        auto opened = opener.TryOpen(counter, headerFor(counter), input, Span<uint8>(payload));
        if (!opened) {
            return AllZero(Span<const uint8>(payload)) ? opened.Error() : ErrorCode::TypeMismatch;
        }

        return opened.Value() == 8 && LoadLittleEndian<uint64>(payload) == counter
                   ? ErrorCode::None
                   : ErrorCode::TypeMismatch;
    };
    const auto open = [&](uint64 counter) {
        return openSealed(counter, Span<const uint8>(sealed[counter]));
    };

    // 순서 바뀜·유실: 4, 2, 0, 3을 받고 1은 잃는다. 재전송은 복호 없이 거부하고 세지 않는다.
    CHECK(open(4) == ErrorCode::None);
    CHECK(open(2) == ErrorCode::None);
    CHECK(open(0) == ErrorCode::None);
    CHECK(open(3) == ErrorCode::None);
    CHECK(open(2) == ErrorCode::AuthenticationFailed);
    CHECK(open(4) == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 0);

    // 5..2047을 받고(999·1000은 잃는다) 2147로 뛴다. 창은 [100, 2147]이고, 창이 밀리며 지운
    // 자리(2048..2146은 예전 0..98의 bit 자리)는 새 counter로 받는다. 잃었던 1은 창 밖이다.
    for (uint64 counter = 5; counter < 2048; ++counter) {
        if (counter != 999 && counter != 1000) {
            CHECK(open(counter) == ErrorCode::None);
        }
    }

    CHECK(open(2147) == ErrorCode::None);
    CHECK(open(1) == ErrorCode::AuthenticationFailed);
    CHECK(open(99) == ErrorCode::AuthenticationFailed);
    CHECK(open(100) == ErrorCode::AuthenticationFailed); // 창 안이지만 이미 받았다
    CHECK(open(2100) == ErrorCode::None);
    CHECK(open(2048) == ErrorCode::None);
    CHECK(open(2048) == ErrorCode::AuthenticationFailed);

    // 창 아래 경계: 가장 큰 counter가 3047이면 창은 [1000, 3047]이다. 받지 않은 1000은 받고, 받지
    // 않은 999는 창 밖이라 거부한다.
    CHECK(open(3047) == ErrorCode::None);
    CHECK(open(1000) == ErrorCode::None);
    CHECK(open(1000) == ErrorCode::AuthenticationFailed);
    CHECK(open(999) == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 0);

    // 위조는 창을 밀지 않는다: 먼 counter의 위조를 거부한 뒤에도 창 안 counter는 열린다(위조가 창을
    // 1000000으로 밀었다면 3060은 창 밖이다).
    uint8 forged[24] = {};
    CHECK(openSealed(1000000, Span<const uint8>(forged)) == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 1);
    CHECK(open(3060) == ErrorCode::None);
    // 다른 header(AD)나 다른 counter로 연 진짜 봉인도 AEAD 실패로 센다. 실패한 counter는 기록하지
    // 않으므로 원래 패킷은 그 뒤에 열린다.
    CHECK(
        opener.TryOpen(3061, headerFor(3062), Span<const uint8>(sealed[3061]), Span<uint8>(payload))
            .Error() == ErrorCode::AuthenticationFailed);
    CHECK(openSealed(3063, Span<const uint8>(sealed[3061])) == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 3);
    CHECK(open(3061) == ErrorCode::None);
    CHECK(open(3063) == ErrorCode::None);

    // 형식 오류(짧은 봉인, counter 상한 이상, datagram 상한 초과)는 AEAD 없이 거부하고 세지 않는다.
    CHECK(openSealed(3064, Span<const uint8>(sealed[3064]).Left(15)) ==
          ErrorCode::AuthenticationFailed);
    CHECK(opener
              .TryOpen(DatagramCounterLimit, headerFor(3064), Span<const uint8>(sealed[3064]),
                       Span<uint8>(payload))
              .Error() == ErrorCode::AuthenticationFailed);
    static uint8 bigHeader[DatagramMaxBytes];
    CHECK(opener
              .TryOpen(3064, Span<const uint8>(bigHeader).Left(DatagramMaxBytes - 23),
                       Span<const uint8>(sealed[3064]), Span<uint8>(payload))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 3);
    CHECK(open(3064) == ErrorCode::None);

    // 호출자 오류는 상태를 바꾸지 않는다: 작은 payload는 CapacityExceeded, 겹침은
    // InvalidArgument다.
    CHECK(opener
              .TryOpen(3065, headerFor(3065), Span<const uint8>(sealed[3065]),
                       Span<uint8>(payload).Left(7))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(opener
              .TryOpen(3065, Span<const uint8>(payload), Span<const uint8>(sealed[3065]),
                       Span<uint8>(payload))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(opener
              .TryOpen(3065, headerFor(3065), Span<const uint8>(sealed[3065]),
                       Span<uint8>(sealed[3065]).RightChop(1))
              .Error() == ErrorCode::InvalidArgument);
    // 제자리 열기(payload가 봉인과 같은 주소)는 받는다.
    auto inPlace = opener.TryOpen(3065, headerFor(3065), Span<const uint8>(sealed[3065]),
                                  Span<uint8>(sealed[3065]));
    REQUIRE(inPlace && inPlace.Value() == 8);
    CHECK(LoadLittleEndian<uint64>(sealed[3065]) == 3065);

    // 창 크기 이상 뛰면 창 전체를 지운다: 6000을 받은 뒤 창 안의 4000(예전 1952의 bit 자리)은
    // 받고, 3099는 창 밖이다.
    uint8 far[2][24];
    REQUIRE(seal(4000, Span<uint8>(far[0])) && seal(6000, Span<uint8>(far[1])));
    CHECK(openSealed(6000, Span<const uint8>(far[1])) == ErrorCode::None);
    CHECK(openSealed(4000, Span<const uint8>(far[0])) == ErrorCode::None);
    CHECK(open(3099) == ErrorCode::AuthenticationFailed);
    CHECK(opener.AuthenticationFailures() == 3);

    // 옮겨 간 객체·기본 객체는 StaleHandle이다.
    DatagramOpener movedOpener = Move(opener);
    CHECK(opener.IsEmpty() && !movedOpener.IsEmpty());
    CHECK(movedOpener.AuthenticationFailures() == 3);
    CHECK(
        opener.TryOpen(3066, headerFor(3066), Span<const uint8>(sealed[3066]), Span<uint8>(payload))
            .Error() == ErrorCode::StaleHandle);
    DatagramSealer movedSealer = Move(sealer);
    CHECK(sealer.IsEmpty() && !movedSealer.IsEmpty());
    CHECK(sealer.TrySeal(Span<const uint8>(), Span<const uint8>(), Span<uint8>(payload)).Error() ==
          ErrorCode::StaleHandle);
    DatagramSealer emptySealer;
    CHECK(emptySealer.TrySeal(Span<const uint8>(), Span<const uint8>(), Span<uint8>(payload))
              .Error() == ErrorCode::StaleHandle);
}

TEST(CryptoChannelDatagramLimits) {
    const ChannelServerKey key = GenerateServerKey();
    REQUIRE(!key.IsEmpty());
    ChannelSession client;
    ChannelSession server;
    REQUIRE(TryConnect(key, client, server));
    DatagramSealer& sealer = server.send;
    DatagramOpener& opener = client.receive;

    // datagram 상한: header + payload + 16 = 65507은 받고 65508은 CapacityExceeded다.
    static uint8 header[DatagramMaxBytes];
    static uint8 sealed[DatagramMaxBytes + 1];
    uint8 payload[32] = {};
    const Span<const uint8> exact = Span<const uint8>(header).Left(DatagramMaxBytes - 16 - 4);
    CHECK(sealer.TrySeal(exact, Span<const uint8>(payload).Left(5), Span<uint8>(sealed)).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(sealer.NextCounter() == 0);
    auto fits = sealer.TrySeal(exact, Span<const uint8>(payload).Left(4), Span<uint8>(sealed));
    REQUIRE(fits && fits.Value() == 0);
    CHECK(opener.TryOpen(0, exact, Span<const uint8>(sealed).Left(20), Span<uint8>(payload)));
    static uint8 bigPayload[DatagramMaxBytes + 1];
    CHECK(sealer.TrySeal(Span<const uint8>(), Span<const uint8>(bigPayload), Span<uint8>(sealed))
              .Error() == ErrorCode::CapacityExceeded);

    // 출력 부족은 CapacityExceeded, header·출력 겹침은 InvalidArgument이고 counter는 그대로다.
    CHECK(sealer
              .TrySeal(Span<const uint8>(), Span<const uint8>(payload).Left(8),
                       Span<uint8>(sealed).Left(23))
              .Error() == ErrorCode::CapacityExceeded);
    CHECK(sealer
              .TrySeal(Span<const uint8>(sealed).Left(4), Span<const uint8>(payload).Left(8),
                       Span<uint8>(sealed))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(sealer.NextCounter() == 1);
    // 제자리 봉인(payload가 출력과 같은 주소)은 받는다.
    memcpy(sealed, "in-place", 8);
    auto inPlace =
        sealer.TrySeal(Text("h"), Span<const uint8>(sealed).Left(8), Span<uint8>(sealed));
    REQUIRE(inPlace && inPlace.Value() == 1);
    auto inPlaceOpened =
        opener.TryOpen(1, Text("h"), Span<const uint8>(sealed).Left(24), Span<uint8>(payload));
    REQUIRE(inPlaceOpened);
    CHECK(ConstantTimeEquals(Span<const uint8>(payload).Left(8), Text("in-place")));

    // counter 상한: 2^63 − 1은 쓰고 그 뒤는 CapacityExceeded(소진)다.
    crypto::detail::ChannelTestAccess::SetNextCounter(sealer, DatagramCounterLimit - 1);
    CHECK(!sealer.IsExhausted());
    auto last = sealer.TrySeal(Text("h"), Text("last"), Span<uint8>(sealed));
    REQUIRE(last && last.Value() == DatagramCounterLimit - 1);
    CHECK(sealer.IsExhausted() && sealer.NextCounter() == DatagramCounterLimit);
    CHECK(sealer.TrySeal(Text("h"), Text("more"), Span<uint8>(sealed)).Error() ==
          ErrorCode::CapacityExceeded);
    auto lastOpened = opener.TryOpen(DatagramCounterLimit - 1, Text("h"),
                                     Span<const uint8>(sealed).Left(20), Span<uint8>(payload));
    REQUIRE(lastOpened);
    CHECK(ConstantTimeEquals(Span<const uint8>(payload).Left(4), Text("last")));

    // 인증 실패 상한: 2^36 − 1번째 실패까지는 계속 열고, 2^36번째에서 소진되어 이후 모두 거부한다.
    auto first = client.send.TrySeal(Text("h"), Text("ok"), Span<uint8>(sealed));
    REQUIRE(first && first.Value() == 0);
    DatagramOpener& serverOpener = server.receive;
    crypto::detail::ChannelTestAccess::SetAuthenticationFailures(serverOpener,
                                                                 DatagramFailureLimit - 2);
    uint8 forged[18] = {};
    CHECK(serverOpener.TryOpen(5, Text("h"), Span<const uint8>(forged), Span<uint8>(payload))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(!serverOpener.IsExhausted());
    CHECK(serverOpener.AuthenticationFailures() == DatagramFailureLimit - 1);
    CHECK(serverOpener.TryOpen(0, Text("h"), Span<const uint8>(sealed).Left(18),
                               Span<uint8>(payload)));
    auto next = client.send.TrySeal(Text("h"), Text("ok"), Span<uint8>(sealed));
    REQUIRE(next && next.Value() == 1);
    CHECK(serverOpener.TryOpen(6, Text("h"), Span<const uint8>(forged), Span<uint8>(payload))
              .Error() == ErrorCode::AuthenticationFailed);
    CHECK(serverOpener.IsExhausted());
    CHECK(serverOpener.AuthenticationFailures() == DatagramFailureLimit);
    memset(payload, 0xa5, sizeof(payload));
    CHECK(
        serverOpener.TryOpen(1, Text("h"), Span<const uint8>(sealed).Left(18), Span<uint8>(payload))
            .Error() == ErrorCode::AuthenticationFailed);
    CHECK(AllZero(Span<const uint8>(payload)));
    CHECK(serverOpener.AuthenticationFailures() == DatagramFailureLimit);
}
} // namespace channel_tests_file
