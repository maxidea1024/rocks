# 보안 채널 — 게임 transport 보호 설계안

> 상태: 채택(2026-11-15 사용자 결정: SC1 Noise NK, 나머지는 추천안). 리뷰 1회 반영. 엔진 계획 17.1절(NetCore가 암호화된 transport adapter를 맡고 자체 암호 알고리즘을 설계하지
> 않는다)과 [Crypto](Crypto.ko.md) 13절 후속 판단을 따른다. 표준 Noise Protocol Framework(rev 34)를 libsodium primitive로 구현하는
> 것은 자체 알고리즘 설계가 아니며, 외부 transport 라이브러리(DTLS·QUIC)를 들이지 않는 근거는 외부 의존 최소화다. Crypto 부품(3절)을 먼저
> 구현하고, 4절은 NetCore가 생길 때 구현한다.

## 1. 범위와 나눔

- **대상.** 게임 클라이언트 ↔ dedicated server의 UDP datagram(초기 dedicated server와 16 client). 순서·유실·중복이 있는 전송이다.
- **Crypto가 맡는 것.** wire 형식이 없는 두 부품이다. (1) `NoiseHandshake`: `Noise_NK_25519_ChaChaPoly_SHA256` 하나만 지원하는
  handshake 상태 기계(메시지 byte를 만들고 읽을 뿐 framing·주소를 모른다). (2) 명시 counter datagram 봉인·열기(Crypto 8절이 예고한
  "명시적 순번 변형", 재전송 창 포함).
- **NetCore가 맡는 것(이 문서 4·5절은 그 요구사항이다).** 패킷 형식(type·version·keyId·cookie·receiverIndex·padding), 쿠키와 rate
  limit, 증폭 방지, handshake 재시도·timeout·half-open 상한, connection ID와 주소 이동, 인증 토큰 확인, 재전송·MTU·fragment.
- **막는 것.** 도청, 위·변조, 패킷 주입, 재전송, 서버 위장, handshake를 이용한 증폭·자원 고갈. **막지 않는 것.** 변조된
  클라이언트(치트, 서버 권한 검증 몫), 트래픽 양·시점 분석.

## 2. 결정 요약

| 코드 | 질문 | 추천안 | 대안 |
|---|---|---|---|
| SC1 | handshake | Noise NK(서버 정적 키를 클라이언트가 안다, 클라이언트는 암호 계층에서 익명). 근거: 백엔드 없는 LAN·개발 서버에서도 쓰고, 첫 transport부터 전방 비밀성이 있다 | 백엔드(matchmaker)가 세션 키를 HTTPS로 양쪽에 배포(netcode.io 방식, handshake 없음, 백엔드 필수) / DTLS·QUIC 라이브러리(외부 의존 큼) |
| SC2 | cipher | ChaCha20-Poly1305 IETF. 게임 트래픽은 대역폭이 작아 AES HW 이점이 거의 없다 | AESGCM suite |
| SC3 | datagram 순서 | 명시 64 bit counter(Noise 11.4 out-of-order), 2048 sliding window(RFC 6479) | 순서 보장 전송만(UDP에 맞지 않음) |
| SC4 | 재키잉 | 하지 않는다. counter 2^63 또는 인증 실패 2^36회에서 소진을 알리고 NetCore가 새 handshake | Noise Rekey와 key epoch |
| SC5 | DoS | 쿠키(주소 증명) + 주소별 handshake token bucket + e 캐시(같은 Hello에 저장한 Welcome 재전송) + half-open 상한. WireGuard 방식 | 쿠키만 |

## 3. Crypto 부품

### 3.1 `NoiseHandshake`(NK 하나)

Noise rev 34의 CipherState·SymmetricState·HandshakeState를 그대로 따른다. primitive: DH = X25519(`crypto_scalarmult`, all-zero 결과
거부), cipher = ChaCha20-Poly1305 IETF(nonce = 0³² ‖ LE64(n)), hash = SHA-256, HKDF = HMAC-SHA-256(`crypto_auth_hmacsha256`).
raw X25519는 공개 API로 내지 않는다.

- 패턴 `<- s` / `-> e, es` / `<- e, ee`. 첫 메시지 payload는 서버 정적 키로만 보호되고(전방 비밀성 없음) replay될 수 있으므로 비밀을
  넣지 않는다. 둘째 메시지는 서버를 인증하지만 받는 쪽이 아직 ephemeral만으로 묶인다. 첫 transport 메시지부터 전방 비밀성이 있다.
- prologue는 호출자가 준다(NetCore는 4절의 평문 헤더를 넣는다). 서버 정적 키는 `ChannelServerKey`(X25519 키 쌍, 이 용도 전용 KeyBlob
  타입 10. kx 키를 재사용하지 않는다, Crypto 2절).
- `Split()` 결과로 방향별 datagram 봉인·열기 객체를 만들고 handshake hash `h`를 내보낸다. NK에는 클라이언트 키가 없으므로 `h`를 bearer
  토큰에 묶는 용도로는 쓰지 않는다(4.3절).
- 검증: Noise 공식 시험 벡터(cacophony 형식 `NK_25519_ChaChaPoly_SHA256`)와 Python `noiseprotocol` 상호운용(시험 전용).

### 3.2 명시 counter datagram

- 봉인: counter를 0부터 늘려 `ChaCha20-Poly1305(키, nonce = 0³² ‖ LE64(counter), AD = 호출자 header, payload)`. counter는 객체가 소유하고
  되돌리는 API가 없다. counter와 header는 호출자가 패킷에 싣는다.
- 열기: (1) counter가 창보다 오래되었거나 이미 받았으면 복호 없이 `AuthenticationFailed` (2) AEAD 검증 (3) 성공한 counter만 창에
  기록한다(위조 패킷이 창을 밀지 못한다). 실패는 연결을 멈추지 않는다. 인증 실패 2^36회에서 소진(`IsExhausted`)을 알린다(RFC 9001
  6.6절의 integrity limit, 재키잉이 없으므로).
- 상한: counter 2^63에서 봉인이 `CapacityExceeded`. datagram 전체 상한은 65,507 byte(IPv4 UDP)이고 payload 상한은 거기서 header·tag를
  뺀 값을 NetCore가 정한다.
- 시험: AD가 있으면 cacophony transport 벡터와 다르므로 datagram golden은 Python(PyNaCl/`cryptography`)으로 따로 만든다. 창 경계·순서
  바꿈·유실·재전송·위조가 창을 밀지 않음·상한을 본다.

## 4. NetCore 요구사항(패킷·DoS·인증)

### 4.1 handshake 패킷

- Hello = `type 1 ‖ version(LE32) ‖ serverKeyId(16) ‖ cookie(16, 없으면 0) ‖ e(32) ‖ tag(16, 빈 payload) ‖ 0 padding`, 길이 고정(예:
  1200 byte). padding이 0이 아니거나 길이가 다르면 버린다. 서버는 keyId로 키를 찾고(시도하지 않는다) version이 맞지 않으면 DH 없이
  평문 거절을 보낸다.
- prologue = `"rocks.channel.v1" ‖ type ‖ version ‖ serverKeyId`. 쿠키는 prologue에 넣지 않는다(재시도 때 Hello를 다시 만들지 않게,
  쿠키는 주소 증명일 뿐이다).
- Welcome = `type 2 ‖ e(32) ‖ Enc(receiverIndex LE32) ‖ tag`. receiverIndex를 payload로 보내 인증한다. 클라이언트 쪽 index는 클라이언트가
  첫 transport 메시지에 싣는다.
- transport = `type 4 ‖ receiverIndex(LE32) ‖ counter(LE64) ‖ 3.2 봉인(AD = 앞 13 byte)`.

### 4.2 DoS·증폭

- 부하가 높을 때(half-open 수 또는 초당 handshake가 설정값을 넘을 때) 쿠키를 요구한다. cookie = HMAC-SHA-256(서버 비밀, 주소 ‖ 시간
  창 ‖ e) 앞 16 byte(Hello 하나에만 쓰인다). 비밀은 2분마다 바꾸고 직전 비밀까지 받는다.
- 쿠키로 확인된 출발 주소(IPv4 /32, IPv6 /64)마다 handshake token bucket을 둔다(쿠키 하나로 무한히 DH를 시키지 못하게).
- 쿠키 유효 기간 동안 받은 e를 캐시하고, 같은 e의 Hello(재전송·replay)에는 DH 없이 저장한 Welcome을 다시 보낸다. half-open 상한을
  둔다.
- 증폭: Hello는 Welcome·쿠키 응답·거절보다 크다. 클라이언트의 첫 유효 transport 메시지(키 확인이자 주소 확인)를 받기 전에는 Hello
  하나에 Welcome 한 번 말고는 보내지 않는다. 주소 이동은 경로 확인 전 보내는 byte를 받은 byte 이하로 제한한다.

### 4.3 클라이언트 인증 토큰

- handshake 뒤 첫 transport 메시지로 보낸다(전방 비밀성·서버 인증 뒤). NK는 클라이언트 키가 없어 토큰을 `h`에 묶을 수 없으므로, 토큰은
  audience(서버 keyId·match id), 만료 시각, 한 번 쓰는 nonce를 담고 서버가 모두 확인한다(같은 신뢰 키를 가진 다른 서버로의 중계 방지).
- 클라이언트 장기 키가 필요해지면(토큰 없는 상호 인증) XK·IK 패턴을 따로 더한다.

## 5. API(초안)

```cpp
namespace rocks::crypto {
class ChannelServerKey;                              // X25519 정적 키 쌍, KeyId, KeyBlob 타입 10
struct ChannelPublicKey { KeyId id; uint8 bytes[32]; };

class NoiseHandshake {                               // Noise_NK_25519_ChaChaPoly_SHA256, 한 번 쓰고 버린다
    static Result<NoiseHandshake> TryCreateInitiator(const ChannelPublicKey& server, Span<const uint8> prologue) noexcept;
    static Result<NoiseHandshake> TryCreateResponder(const ChannelServerKey& key, Span<const uint8> prologue) noexcept;
    Result<usize> TryWriteMessage(Span<const uint8> payload, Span<uint8> message) noexcept;
    Result<usize> TryReadMessage(Span<const uint8> message, Span<uint8> payload) noexcept;   // 변조 AuthenticationFailed
    Result<ChannelSession> TrySplit() noexcept;      // 두 메시지가 끝난 뒤
};

struct ChannelSession { DatagramSealer send; DatagramOpener receive; uint8 handshakeHash[32]; };
class DatagramSealer { Result<uint64> TrySeal(Span<const uint8> header, Span<const uint8> payload, Span<uint8> sealed) noexcept; };  // 쓴 counter
class DatagramOpener { Result<usize> TryOpen(uint64 counter, Span<const uint8> header, Span<const uint8> sealed, Span<uint8> payload) noexcept; };
}
```

오류는 [Crypto](Crypto.ko.md) 11절을 따른다. 공격자 입력은 `AuthenticationFailed`로만 거부되고 Fatal·`InvalidArgument`가 되지 않는다.

## 6. 단계

(1) `NoiseHandshake` + cacophony 벡터·Python 상호운용 (2) datagram 봉인·열기·창·integrity limit (3) `ChannelServerKey`와 KeyBlob 타입
10 (4) NetCore가 생기면 4절을 구현한다. Crypto 13절 후속 행과 엔진 계획 17.1 문구는 이 나눔에 맞춰 고친다.
