#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/Aead.h>
#include <Crypto/KeyId.h>
#include <Crypto/Secret.h>

// 보안 채널의 Crypto 부품이다(Docs/Design/SecureChannel.ko.md 3·5절). 게임 클라이언트와 dedicated
// server가 UDP datagram을 주고받기 전에 handshake로 세션 키를 정하고, 그 키로 datagram을 봉인·연다.
// 패킷 형식(type·version·keyId·cookie·receiverIndex)·쿠키·rate limit·재시도는 NetCore 몫이고
// 여기에는 없다. 이 헤더의 객체는 wire 형식을 모른다: 메시지 byte를 만들고 읽을 뿐 framing·주소를
// 모른다.
//
// - ChannelServerKey: 서버 정적 X25519 키 쌍이다(이 용도 전용, KeyBlob 타입 10). 클라이언트는 공개
//   부분(ChannelPublicKey)을 배포 설정 등으로 미리 안다.
// - NoiseHandshake: Noise_NK_25519_ChaChaPoly_SHA256(Noise Protocol Framework rev 34) 하나만
// 지원한다.
//   패턴은 `<- s` / `-> e, es` / `<- e, ee`다. 클라이언트(initiator)는 서버 정적 키를 알고, 서버는
//   클라이언트를 암호 계층에서 모른다(클라이언트 인증은 handshake 뒤 토큰으로 한다, 4.3절).
//   - 첫 메시지 payload는 서버 정적 키로만 보호되어 전방 비밀성이 없고 replay될 수 있다. 비밀이나
//   한
//     번만 받아야 하는 값을 넣지 않는다.
//   - 둘째 메시지는 서버를 인증한다. 첫 transport datagram부터 전방 비밀성이 있다.
//   - prologue는 양쪽이 같은 byte를 준다(NetCore는 평문 패킷 헤더를 넣는다). 다르면 첫 메시지가
//     AuthenticationFailed로 거부된다.
// - DatagramSealer·DatagramOpener: 명시 64 bit counter datagram 봉인·열기다. nonce = 0^32 ‖
//   LE64(counter), AD = 호출자 header(counter를 실은 평문 패킷 헤더)다. 받는 쪽은 2048개 sliding
//   window로 재전송을 거부하고, 순서 바뀜·유실을 받아들인다.
//
// 오류(Docs/Design/Crypto.ko.md 11절): 빈 객체 StaleHandle을 먼저 보고, 함수마다의 판정 순서는
// 각 함수 주석에 적는다. 호출자 인자 오류(InvalidArgument: 잘못된 단계·겹침, CapacityExceeded:
// 출력 부족·상한)는 쓰기 전에 돌아오고 상태를 바꾸지 않는다. 공격자가 보낸 byte는
// AuthenticationFailed로만 거부되고 InvalidArgument·Fatal이 되지 않는다. AuthenticationFailed는
// 출력 Span 전체를 지운다.
//
// 사용 예(클라이언트와 서버, 패킷 framing은 NetCore가 한다):
//   // 서버: 키는 KeyBlob(타입 10)에서 풀거나 처음 한 번 만든다.
//   auto serverKey = crypto::ChannelServerKey::TryGenerate(crypto::TryNewKeyId().Value());
//   const crypto::ChannelPublicKey published = serverKey.Value().PublicKey();   // 클라이언트에
//   배포
//
//   // 클라이언트: Hello를 만든다.
//   auto client = crypto::NoiseHandshake::TryCreateInitiator(published, prologue);
//   auto helloBytes = client.Value().TryWriteMessage(Span<const uint8>(), hello);  // e ‖ tag
//
//   // 서버: Hello를 읽고 Welcome을 쓴다.
//   auto server = crypto::NoiseHandshake::TryCreateResponder(serverKey.Value(), prologue);
//   auto read = server.Value().TryReadMessage(helloView, payload);
//   if (!read) { /* AuthenticationFailed: 버린다 */ }
//   auto welcomeBytes = server.Value().TryWriteMessage(receiverIndexBytes, welcome);
//   auto serverSession = server.Value().TrySplit();
//
//   // 클라이언트: Welcome을 읽고 세션을 얻는다.
//   auto clientRead = client.Value().TryReadMessage(welcomeView, payload);
//   auto clientSession = client.Value().TrySplit();
//
//   // datagram: 봉인한 counter와 header를 패킷에 싣는다.
//   auto counter = clientSession.Value().send.TrySeal(header, input, sealed);
//   auto opened = serverSession.Value().receive.TryOpen(counterFromPacket, header, sealedView,
//   output); if (!opened && serverSession.Value().receive.IsExhausted()) { /* 새 handshake */ }
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
struct ChannelFactory;
struct ChannelTestAccess;
} // namespace detail

// X25519 공개키·비밀키 길이다.
inline constexpr usize ChannelKeyBytes = 32;
// handshake hash(SHA-256) 길이다.
inline constexpr usize ChannelHashBytes = 32;
// Noise 메시지 길이 상한(Noise 3절)이다.
inline constexpr usize NoiseMaxMessageBytes = 65535;
// NK 두 메시지가 payload보다 긴 byte 수(ephemeral 공개키 32 + tag 16)다.
inline constexpr usize NoiseMessageOverheadBytes = 48;
inline constexpr usize NoiseMaxPayloadBytes = NoiseMaxMessageBytes - NoiseMessageOverheadBytes;
// datagram 봉인이 payload보다 긴 byte 수(tag)다.
inline constexpr usize DatagramTagBytes = 16;
// datagram 전체(header + 봉인) 상한이다(IPv4 UDP payload 상한).
inline constexpr usize DatagramMaxBytes = 65507;
// 쓸 수 있는 counter는 0부터 이 값 - 1까지다.
inline constexpr uint64 DatagramCounterLimit = uint64{1} << 63;
// 키 하나로 받아들이는 AEAD 인증 실패 상한(RFC 9001 6.6절 integrity limit)이다.
inline constexpr uint64 DatagramFailureLimit = uint64{1} << 36;
// 재전송 창 크기(RFC 6479)다. 받은 가장 큰 counter에서 이만큼 아래까지 기억한다.
inline constexpr usize DatagramWindowSize = 2048;

// 서버 정적 공개키와 그 KeyId다. 클라이언트가 배포 설정·서버 목록으로 받는 값이다. NetCore는 id를
// 패킷에 실어 서버가 키를 찾게 한다(시도하지 않는다).
struct ChannelPublicKey {
    KeyId id;
    uint8 bytes[ChannelKeyBytes] = {};

    // id와 공개키 32 byte로 만든다. Nil id나 길이가 32가 아니면 InvalidArgument다. 점이 올바른지는
    // handshake가 본다(small-order 점이면 첫 메시지 쓰기가 AuthenticationFailed).
    [[nodiscard]] static Result<ChannelPublicKey> TryFromBytes(KeyId id,
                                                               Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const ChannelPublicKey& other) const noexcept = default;
};

// 서버 정적 X25519 키 쌍과 KeyId다. 이 용도에만 쓴다(kx·sealed box 키와 섞지 않는다, Crypto 2절).
// move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에 TryCreateResponder에 넘겨도 된다.
// 비밀키는 키 저장소에 있고 소멸 때 지운다. 보관은 KeyBlob(타입 10), 평문 내보내기는
// hazmat::TryExportKey다. 키 회전은 새 KeyId로 새 키를 만들고, 옛 키는 그 id를 아는 클라이언트가
// 남아 있는 동안 둔다.
class ChannelServerKey {
public:
    ChannelServerKey() noexcept = default;

    // OS 난수로 만든다. Nil id는 InvalidArgument, OS 난수 실패는 DependencyFailed, 키 저장소가
    // 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<ChannelServerKey> TryGenerate(KeyId id) noexcept;

    // X25519 비밀키 32 byte에서 만든다(공개키를 계산한다). Nil id나 길이 오류는 InvalidArgument다.
    // secretKey는 호출자가 지운다.
    [[nodiscard]] static Result<ChannelServerKey> TryImport(KeyId id,
                                                            Span<const uint8> secretKey) noexcept;

    // 빈 키(기본 생성·옮겨 간 객체)는 Nil id와 0이다.
    [[nodiscard]] ChannelPublicKey PublicKey() const noexcept {
        return key_.IsEmpty() ? ChannelPublicKey() : publicKey_;
    }

    [[nodiscard]] KeyId Id() const noexcept {
        return key_.IsEmpty() ? KeyId() : publicKey_.id;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    ChannelServerKey(SecretBytes&& key, const ChannelPublicKey& publicKey) noexcept
        : key_(Move(key)), publicKey_(publicKey) {
    }

    SecretBytes key_;
    ChannelPublicKey publicKey_;
};

class NoiseHandshake;

// 한 방향 datagram 봉인기다. NoiseHandshake::TrySplit이 만든다. counter는 0부터 늘고 객체가
// 소유한다(되돌리거나 지정하는 API가 없어 nonce가 다시 쓰이지 않는다).
//
// 한 스레드에서 쓴다(봉인과 전송 큐 넣기를 함께 직렬화한다). 받는 쪽 DatagramOpener와는 다른
// 객체이므로 송신 스레드와 수신 스레드가 따로 써도 된다. move-only이고 옮겨 간 객체는
// StaleHandle이다. 키는 키 저장소에 있고 소멸 때 지운다.
class DatagramSealer {
public:
    DatagramSealer() noexcept = default;

    // payload를 다음 counter로 봉인해 sealed 앞 payload.Size() + 16 byte에 암호문 ‖ tag를 쓴다. 쓴
    // counter를 돌려준다. 호출자는 counter와 header를 패킷에 싣는다(header는 AD로 인증되고 봉인에는
    // 들어가지 않는다). 받는 쪽은 같은 counter·header로 TryOpen한다.
    // - 빈 객체는 StaleHandle이다.
    // - counter를 다 썼으면(IsExhausted) CapacityExceeded다. 새 handshake로 바꾼다.
    // - header + payload + 16이 DatagramMaxBytes를 넘거나 sealed가 작으면 CapacityExceeded다.
    // - 제자리(payload.Data() == sealed.Data())만 겹침을 허용하고, 그 밖의 겹침과 header·sealed
    //   겹침은 InvalidArgument다.
    // 오류는 모두 쓰기 전에 돌아오고 counter를 바꾸지 않는다.
    [[nodiscard]] Result<uint64> TrySeal(Span<const uint8> header, Span<const uint8> payload,
                                         Span<uint8> sealed) noexcept;

    // 다음 봉인에 쓸 counter다. 다 썼으면 DatagramCounterLimit이다.
    [[nodiscard]] uint64 NextCounter() const noexcept {
        return next_;
    }

    [[nodiscard]] bool IsExhausted() const noexcept {
        return next_ >= DatagramCounterLimit;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend class NoiseHandshake;
    friend struct detail::ChannelTestAccess;

    explicit DatagramSealer(AeadKey&& key) noexcept : key_(Move(key)) {
    }

    AeadKey key_;
    uint64 next_ = 0;
};

// 한 방향 datagram 열기다. NoiseHandshake::TrySplit이 만든다. 순서가 바뀌거나 빠진 datagram을
// 받아들이고, 받은 가장 큰 counter에서 DatagramWindowSize 아래까지 이미 받은 counter와 그보다
// 오래된 counter는 거부한다(재전송 방어).
//
// - 열기 순서: counter가 창보다 오래되었거나 이미 받았으면 복호 없이 거부한다 → AEAD 검증 → 성공한
//   counter만 창에 기록한다. 위조 datagram은 창을 밀지 못한다.
// - 실패는 객체를 멈추지 않는다(다음 정상 datagram은 열린다). 다만 AEAD 검증 실패가
//   DatagramFailureLimit(2^36)에 이르면 IsExhausted가 true가 되고 이후 모든 TryOpen이 거부된다
//   (위조 시도 상한, RFC 9001 6.6절). NetCore는 새 handshake로 바꾼다. 창에서 거부한 재전송과 길이
//   오류는 AEAD를 시도하지 않으므로 세지 않는다.
// - 한 스레드에서 쓴다. move-only이고 옮겨 간 객체는 StaleHandle이다. 키는 키 저장소에 있고 소멸 때
//   지운다.
class DatagramOpener {
public:
    DatagramOpener() noexcept = default;

    // 패킷의 counter·header와 봉인(암호문 ‖ tag)을 열어 payload 앞에 평문을 쓰고 그 길이(sealed −
    // 16)를 돌려준다.
    // - 빈 객체는 StaleHandle이다.
    // - 제자리(payload.Data() == sealed.Data())만 겹침을 허용하고, 그 밖의 겹침과 header·payload
    //   겹침은 InvalidArgument다.
    // - sealed가 16 byte 이상이고 payload가 평문 길이보다 작으면 CapacityExceeded다.
    // - 소진 뒤, header + sealed가 DatagramMaxBytes를 넘음, sealed가 16 byte보다 짧음, counter가
    //   DatagramCounterLimit 이상, 창보다 오래됨, 이미 받음, AEAD 불일치(변조·다른 키·다른
    //   header)는 모두 AuthenticationFailed이고 payload 전체를 지운다.
    // InvalidArgument·CapacityExceeded는 쓰기 전에 돌아오고 상태를 바꾸지 않는다.
    [[nodiscard]] Result<usize> TryOpen(uint64 counter, Span<const uint8> header,
                                        Span<const uint8> sealed, Span<uint8> payload) noexcept;

    // 지금까지 AEAD 검증에 실패한 수다(진단용).
    [[nodiscard]] uint64 AuthenticationFailures() const noexcept {
        return failures_;
    }

    [[nodiscard]] bool IsExhausted() const noexcept {
        return failures_ >= DatagramFailureLimit;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend class NoiseHandshake;
    friend struct detail::ChannelTestAccess;

    static constexpr usize WindowWords = DatagramWindowSize / 64;

    explicit DatagramOpener(AeadKey&& key) noexcept : key_(Move(key)) {
    }

    AeadKey key_;
    // 받은 가장 큰 counter + 1이다(0이면 아직 받지 않았다).
    uint64 top_ = 0;
    uint64 failures_ = 0;
    // counter c의 수신 여부는 bit (c mod DatagramWindowSize)다. [top_ − 2048, top_) 범위만 뜻이
    // 있다.
    uint64 window_[WindowWords] = {};
};

// handshake가 끝난 세션이다. send로 봉인해 보내고 receive로 상대가 보낸 것을 연다(방향별 키가
// 다르다). handshakeHash는 Noise handshake hash h다. NK에는 클라이언트 키가 없으므로 h를 bearer
// 토큰에 묶는 용도로는 쓰지 않는다(SecureChannel 4.3절). 채널 binding·로그 상관에는 쓸 수 있다.
struct ChannelSession {
    DatagramSealer send;
    DatagramOpener receive;
    uint8 handshakeHash[ChannelHashBytes] = {};
};

// NoiseHandshake가 다음에 받을 호출이다.
enum class NoiseStep : uint8 {
    None = 0,         // 빈 객체(기본 생성·옮겨 감)
    WriteMessage = 1, // 다음은 TryWriteMessage(클라이언트 첫 메시지, 서버 둘째 메시지)
    ReadMessage = 2,  // 다음은 TryReadMessage(서버 첫 메시지, 클라이언트 둘째 메시지)
    Split = 3,        // 두 메시지가 끝났다. 다음은 TrySplit
    Done = 4,         // TrySplit 뒤. 더 쓰지 않는다
    Failed = 5,       // 인증 실패로 버렸다. 모든 호출이 AuthenticationFailed다
};

// Noise_NK_25519_ChaChaPoly_SHA256 handshake 상태 기계다. 한 번 쓰고 버린다: 클라이언트는
// Write → Read → Split, 서버는 Read → Write → Split 순서로 부른다.
//
// - 순서가 틀린 호출과 Split 뒤의 호출은 InvalidArgument이고 상태를 바꾸지 않는다.
// - 받은 메시지가 짧거나 길거나, 변조되었거나, 다른 서버 키·prologue로 만들어졌거나, ephemeral
//   공개키가 small-order 점(DH 결과 0)이면 AuthenticationFailed이고 handshake는 Failed가 된다(비밀
//   상태를 지우고 이후 모든 호출이 AuthenticationFailed다). NetCore는 새 handshake로 다시 시작한다.
// - DH = X25519(결과가 0이면 거부), cipher = ChaCha20-Poly1305 IETF(nonce = 0^32 ‖ LE64(n)), hash =
//   SHA-256, HKDF = HMAC-SHA-256이다. Noise rev 34의 CipherState·SymmetricState·HandshakeState를
//   그대로 따르므로 다른 Noise 구현과 상호운용된다.
// - ephemeral 비밀키, chaining key, 대칭 키, 서버 정적 비밀키 사본은 키 저장소에 있고 Split·실패·
//   소멸 때 지운다.
// - 한 스레드에서 쓴다. move-only이고 옮겨 간 객체는 StaleHandle이다.
class NoiseHandshake {
public:
    NoiseHandshake() noexcept = default;
    ~NoiseHandshake() noexcept;

    NoiseHandshake(const NoiseHandshake&) = delete;
    NoiseHandshake& operator=(const NoiseHandshake&) = delete;
    NoiseHandshake(NoiseHandshake&& other) noexcept;
    NoiseHandshake& operator=(NoiseHandshake&& other) noexcept;

    // 클라이언트(initiator)다. server는 미리 아는 서버 정적 공개키, prologue는 양쪽이 같은 byte로
    // 주는 맥락(복사하지 않고 해시에 섞는다)이다. Nil server.id는 InvalidArgument, 키 저장소가
    // 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<NoiseHandshake>
    TryCreateInitiator(const ChannelPublicKey& server, Span<const uint8> prologue) noexcept;

    // 서버(responder)다. key의 비밀키를 handshake 상태에 복사하므로 key는 이 호출 뒤 바로 버려도
    // 된다. 빈 key는 StaleHandle, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<NoiseHandshake>
    TryCreateResponder(const ChannelServerKey& key, Span<const uint8> prologue) noexcept;

    // 다음 메시지(ephemeral 공개키 32 ‖ 암호화한 payload ‖ tag 16)를 message 앞에 쓰고 그 길이
    // (payload + 48)를 돌려준다. ephemeral 키는 OS 난수로 만든다.
    // - 빈 객체 StaleHandle → payload·message 겹침 InvalidArgument → Failed면
    //   AuthenticationFailed(message 소거) → 쓸 차례가 아니면 InvalidArgument → payload가
    //   NoiseMaxPayloadBytes를 넘거나 message가 작으면 CapacityExceeded 순이다. 여기까지는 Failed의
    //   소거 말고는 쓰지 않고 상태를 바꾸지 않는다.
    // - OS 난수 실패는 DependencyFailed(상태 불변)다.
    // - 클라이언트가 가진 서버 공개키가 small-order 점이면 AuthenticationFailed이고 Failed가 된다.
    [[nodiscard]] Result<usize> TryWriteMessage(Span<const uint8> payload,
                                                Span<uint8> message) noexcept;

    // 상대 메시지를 읽어 payload 앞에 평문(message − 48 byte)을 쓰고 그 길이를 돌려준다.
    // - 빈 객체 StaleHandle → message·payload 겹침 InvalidArgument → Failed면
    //   AuthenticationFailed(payload 소거) → 읽을 차례가 아니면 InvalidArgument → message 길이가
    //   48 byte 미만이거나 NoiseMaxMessageBytes 초과면 AuthenticationFailed → payload가 평문
    //   길이보다 작으면 CapacityExceeded(상태 불변, 더 큰 버퍼로 다시 부른다) → 변조·다른 서버
    //   키·다른 prologue·small-order ephemeral은 AuthenticationFailed(payload 소거) 순이다.
    // - 메시지에서 온 AuthenticationFailed 뒤의 상태는 역할에 따라 다르다. 서버(responder)는
    //   Failed가 되어 버린다(첫 메시지마다 새 객체를 쓴다). 클라이언트(initiator)는 읽기 전 상태로
    //   돌아가 ReadMessage에 머문다. 서버 주소로 위장한 경로 밖 공격자의 가짜 둘째 메시지가
    //   handshake를 끝내지 못하게 하려는 것이므로, 진짜 메시지가 오면 그대로 읽으면 된다. 기다림의
    //   상한(timeout)은 호출자(NetCore)가 둔다.
    [[nodiscard]] Result<usize> TryReadMessage(Span<const uint8> message,
                                               Span<uint8> payload) noexcept;

    // 두 메시지가 끝난 뒤 방향별 datagram 봉인·열기와 handshake hash를 만든다. 성공하면 비밀 상태를
    // 지우고 Done이 된다. 빈 객체 StaleHandle, Failed는 AuthenticationFailed, 차례가 아니면
    // InvalidArgument, 키 저장소가 찼으면 OutOfMemory(상태 불변, 다시 부를 수 있다)다.
    [[nodiscard]] Result<ChannelSession> TrySplit() noexcept;

    [[nodiscard]] NoiseStep NextStep() const noexcept {
        return step_;
    }

    // 클라이언트(initiator)인가. 빈 객체는 false다.
    [[nodiscard]] bool IsInitiator() const noexcept {
        return initiator_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return step_ == NoiseStep::None;
    }

private:
    friend struct detail::ChannelFactory;

    NoiseHandshake(void* state, bool initiator, NoiseStep step) noexcept
        : state_(state), initiator_(initiator), step_(step) {
    }

    void Release() noexcept;

    // 키 저장소 슬롯(detail::NoiseState)이다. Split·실패·소멸 때 지우고 nullptr가 된다.
    void* state_ = nullptr;
    bool initiator_ = false;
    NoiseStep step_ = NoiseStep::None;
};

namespace hazmat {

// 서버 정적 비밀키(32 byte)를 평문으로 output에 복사한다(Crypto 10.4절). 빈 키는 StaleHandle,
// output.Size()가 32가 아니거나 키와 겹치면 InvalidArgument다. output은 호출자가 지운다.
[[nodiscard]] Result<void> TryExportKey(const ChannelServerKey& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
