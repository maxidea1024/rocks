#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Crypto/Kdf.h>
#include <Crypto/Secret.h>

// 인증 암호(AEAD)다(Docs/Design/Crypto.ko.md 4·6·8·11절). 구현은 libsodium이다(AES-128-GCM은 AES
// 경량 계층 단계에서 더한다).
//
// - 기본은 XChaCha20-Poly1305다(GetPreferredAead, 결정 D2). 모든 CPU에서 상수 시간이고 nonce가 24
//   byte라 무작위로 만들어도 안전하다.
// - AES-256-GCM·AEGIS는 HW 명령(x86-64 AES-NI·PCLMULQDQ·AVX, ARM64 AES·PMULL)이 있을 때만 쓴다. 두
//   알고리즘은 같은 조건이라 함께 쓸 수 있거나 함께 Unsupported다(소프트웨어 AES와 AEGIS soft
//   경로는 상수 시간이 아니라서 쓰지 않는다, 결정 D3).
// - 고수준 TrySeal·TryOpen은 nonce를 만들어 앞에 붙인다(출력 = nonce ‖ 암호문 ‖ tag). 무작위
//   nonce가 안전한 알고리즘(XChaCha20-Poly1305, AEGIS-256)만 받고, 나머지(ChaCha20-IETF·AES-GCM의
//   96 bit, AEGIS-128L의 128 bit nonce)는 Unsupported다. 그 알고리즘은 hazmat 순번
//   봉인기(AeadSequenceSealer·AeadSequenceOpener, 카운터 nonce)나 nonce 지정 API, 세그먼트
//   스트림으로 쓴다.
// - 복호 실패(tag 불일치, 잘린 입력, 다른 키·AD)는 모두 AuthenticationFailed이고 평문 출력 전체를
//   0으로 지운다. 어느 검사가 실패했는지는 알리지 않는다(결정 D5). AES-GCM·AEGIS는 검증 전에 복호한
//   byte를 plaintext에 잠시 쓰고 실패하면 지운다(ChaCha20 계열은 검증 뒤에 쓴다). 호출 중에 다른
//   스레드나 프로세스가 같은 버퍼를 읽지 않게 한다.
// - 키는 알고리즘에 묶인 AeadKey이고 키 저장소에 있다. 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
//   써도 된다. GCM·ChaCha20-Poly1305·AEGIS는 key commitment가 없다. 한 암호문이 다른 키로도 열릴 수
//   있으므로 형식은 키 ID를 기록하고 시도 복호를 하지 않는다(2절).
//
// 제자리 처리(7절): TrySeal은 plaintext.Data() == output.Data() + NonceBytes일 때, TryOpen은
// plaintext.Data() == sealed.Data() + NonceBytes일 때만 겹침을 허용한다. hazmat 함수는 입력과
// 출력이 같은 주소에서 시작할 때만 허용한다. 그 밖의 겹침은 쓰기 전에 InvalidArgument다.
//
// 사용 예:
//   auto key = crypto::AeadKey::TryGenerate(crypto::GetPreferredAead());
//   if (!key) { return key.Error(); }
//   auto sealedBytes = crypto::TryGetSealedBytes(key.Value().Algorithm(), plaintext.Size());
//   Array<uint8> sealed(allocator);   // sealedBytes.Value() 크기로 준비
//   auto written = crypto::TrySeal(key.Value(), associatedData, plaintext, sealed.MutableView());
//   auto opened = crypto::TryOpen(key.Value(), associatedData, sealed.View(), plaintextOut);
namespace rocks::crypto {

// 영속 형식에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class AeadAlgorithm : uint8 {
    XChaCha20Poly1305 = 1,    // key 32, nonce 24, tag 16
    ChaCha20Poly1305Ietf = 2, // key 32, nonce 12, tag 16 (RFC 8439)
    Aes256Gcm = 3,            // key 32, nonce 12, tag 16
    Aes128Gcm = 4,            // key 16, nonce 12, tag 16 (AES 경량 계층, 아직 Unsupported)
    Aegis256 = 5,             // key 32, nonce 32, tag 32
    Aegis128L = 6,            // key 16, nonce 16, tag 32
};

// 모든 AEAD 알고리즘의 nonce 길이 상한(AEGIS-256)이다.
inline constexpr usize AeadMaxNonceBytes = 32;

struct AeadInfo {
    usize keyBytes = 0;
    usize nonceBytes = 0;
    usize tagBytes = 0;
    // (key, nonce) 한 쌍으로 봉인할 수 있는 평문 상한이다(11절 표).
    uint64 maxMessageBytes = 0;
    // 키 하나로 봉인할 수 있는 16 byte 블록 수 상한이다(모든 메시지의 평문 블록 + AD 블록 +
    // 메시지당 1). AES-GCM만 있고(2^36 - 1, 기밀성 이점 2^-57, 11절), 0이면 키당 데이터 한도가
    // 없다. AeadSequenceSealer가 이 값으로 막는다. hazmat nonce 지정 API는 호출자가 센다.
    uint64 maxBlocksPerKey = 0;
    // 무작위 nonce를 써도 되는가(XChaCha20-Poly1305, AEGIS-256). 고수준 TrySeal은 이 알고리즘만
    // 받는다.
    bool randomNonceSafe = false;
    // 이 기기에서 쓸 수 있는가. AES·AEGIS는 HW 명령이 있을 때만 true다.
    bool available = false;
    // 진단용 구현 이름이다(예: "libsodium/aesni"). 쓸 수 없으면 빈 문자열이다.
    StringView implementation;
};

// 알고리즘 정보다. 열거값이 아니면 InvalidArgument다.
[[nodiscard]] Result<AeadInfo> TryGetAeadInfo(AeadAlgorithm algorithm) noexcept;

// 기본 AEAD다(XChaCha20-Poly1305). 모든 기기에서 같은 값이다(결정 D2).
[[nodiscard]] AeadAlgorithm GetPreferredAead() noexcept;

// 고수준 TrySeal의 출력 길이(nonce + plaintextBytes + tag)다. 넘치면 Overflow, 열거값이 아니면
// InvalidArgument다.
[[nodiscard]] Result<usize> TryGetSealedBytes(AeadAlgorithm algorithm,
                                              usize plaintextBytes) noexcept;

// 알고리즘에 묶인 AEAD 키다. move-only이며 생성 뒤 바뀌지 않는다. AES-256-GCM 키는 만들 때 키 확장
// 상태를 한 번 계산해 키 저장소에 둔다. 평문 내보내기는 hazmat::TryExportKey다.
class AeadKey {
public:
    AeadKey() noexcept = default;
    ~AeadKey() noexcept;

    AeadKey(const AeadKey&) = delete;
    AeadKey& operator=(const AeadKey&) = delete;
    AeadKey(AeadKey&& other) noexcept;
    AeadKey& operator=(AeadKey&& other) noexcept;

    // OS 난수 키다. 이 기기에서 쓸 수 없는 알고리즘은 Unsupported, 열거값이 아니면 InvalidArgument,
    // OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<AeadKey> TryGenerate(AeadAlgorithm algorithm) noexcept;

    // keyBytes를 복사한 키다. 길이가 알고리즘 키 길이와 다르면 InvalidArgument다. 나머지는
    // TryGenerate와 같다. keyBytes는 호출자가 지운다.
    [[nodiscard]] static Result<AeadKey> TryImport(AeadAlgorithm algorithm,
                                                   Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] AeadAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    AeadKey(AeadAlgorithm algorithm, SecretBytes&& key, void* state) noexcept
        : algorithm_(algorithm), key_(Move(key)), state_(state) {
    }

    void Release() noexcept;

    AeadAlgorithm algorithm_ = AeadAlgorithm::XChaCha20Poly1305;
    SecretBytes key_;
    // AES-256-GCM의 libsodium beforenm 상태(키 저장소 슬롯)다. 다른 알고리즘은 nullptr다.
    void* state_ = nullptr;
};

// master에서 AEAD 키를 뽑는다(Kdf.h 타입 파생, 키 타입·알고리즘을 섞는다). 알고리즘 오류는
// AeadKey::TryGenerate와 같고 먼저 검사한다. 빈 master는 StaleHandle, 키 저장소가 찼으면
// OutOfMemory다.
[[nodiscard]] Result<AeadKey> TryDeriveAeadKey(const KdfKey& master, uint64 subkeyId,
                                               KdfContext context,
                                               AeadAlgorithm algorithm) noexcept;

// 무작위 nonce를 만들어 봉인한다. 출력 = nonce ‖ 암호문 ‖ tag이고 쓴 byte 수를 돌려준다.
// - 무작위 nonce가 안전하지 않은 알고리즘(randomNonceSafe false)은 Unsupported다.
// - output이 TryGetSealedBytes보다 작거나 평문이 상한을 넘으면 CapacityExceeded다.
// - 허용되지 않은 겹침(제자리 규칙 밖, AD와 출력)은 InvalidArgument다.
// - 빈 키는 StaleHandle, nonce용 OS 난수 실패는 DependencyFailed다.
[[nodiscard]] Result<usize> TrySeal(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> plaintext, Span<uint8> output) noexcept;

// TrySeal의 출력을 연다. 평문 byte 수를 돌려준다.
// - sealed가 nonce + tag보다 짧거나 tag가 맞지 않으면 AuthenticationFailed이고 plaintext 전체를
//   0으로 지운다.
// - plaintext가 평문 길이보다 작으면 CapacityExceeded다(쓰지 않는다).
// - 허용되지 않은 겹침은 InvalidArgument, 무작위 nonce 알고리즘이 아닌 키는 Unsupported, 빈 키는
//   StaleHandle이다.
[[nodiscard]] Result<usize> TryOpen(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> sealed, Span<uint8> plaintext) noexcept;

namespace detail {
struct SequenceTestAccess;
} // namespace detail

namespace hazmat {

// 키 byte를 평문으로 output에 복사한다(10.4절). output.Size()는 키 길이와 같아야 한다.
[[nodiscard]] Result<void> TryExportKey(const AeadKey& key, Span<uint8> output) noexcept;

// nonce를 호출자가 책임진다. 같은 키로 같은 nonce를 두 번 쓰면 기밀성과 무결성이 모두
// 깨진다(AEGIS-256·XChaCha20-Poly1305가 아니면 무작위 nonce도 쓰지 않는다). 결합 형식은 암호문 ‖
// tag다. 이 함수들은 모든 알고리즘을 받는다.
// - nonce 길이가 틀리거나 허용되지 않은 겹침이면 InvalidArgument, 빈 키는 StaleHandle이다. GCM의 96
//   bit가 아닌 nonce는 Unsupported다(표준은 허용하지만 지원하지 않는다).
// - 출력이 작거나 평문이 상한을 넘으면 CapacityExceeded다.
// - 여는 쪽은 tag 불일치·잘린 입력이 AuthenticationFailed이고 plaintext 전체를 0으로 지운다.
[[nodiscard]] Result<usize> TrySealWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                             Span<const uint8> associatedData,
                                             Span<const uint8> plaintext,
                                             Span<uint8> sealed) noexcept;
[[nodiscard]] Result<usize> TryOpenWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                             Span<const uint8> associatedData,
                                             Span<const uint8> sealed,
                                             Span<uint8> plaintext) noexcept;

// 분리 형식이다. ciphertext.Size()는 plaintext.Size()와 같아야 한다(다르면 InvalidArgument). 봉인
// 쪽 tag.Size()는 tag 길이와 같아야 하고(InvalidArgument), 여는 쪽 tag 길이가 틀리면 잘린 tag로
// 보고 AuthenticationFailed다.
[[nodiscard]] Result<void> TrySealDetached(const AeadKey& key, Span<const uint8> nonce,
                                           Span<const uint8> associatedData,
                                           Span<const uint8> plaintext, Span<uint8> ciphertext,
                                           Span<uint8> tag) noexcept;
[[nodiscard]] Result<void> TryOpenDetached(const AeadKey& key, Span<const uint8> nonce,
                                           Span<const uint8> associatedData,
                                           Span<const uint8> ciphertext, Span<const uint8> tag,
                                           Span<uint8> plaintext) noexcept;

// 순번 봉인기가 다 썼다는 표지다(NextSequence()). 쓸 수 있는 순번은 0부터 이 값 - 1까지다.
inline constexpr uint64 AeadSequenceExhausted = static_cast<uint64>(-1);

// 순번 nonce 봉인기다. 키를 소유하고 seq번째 메시지의 nonce를 iv XOR (0 ‖ BE64(seq))로 스스로
// 만든다(TLS 1.3 5.3절·QUIC·HPKE와 같은 레코드 nonce 구성). 출력은 암호문 ‖ tag이고 nonce는 싣지
// 않는다(받는 쪽 AeadSequenceOpener가 같은 순서를 센다).
//
// 쓰는 곳: 신뢰할 수 있고 순서가 보장되는 전송에서 AES-GCM·AEGIS의 HW 처리량이 필요하거나 nonce를
// 싣지 않아야 할 때, 또는 TLS 1.3·QUIC·HPKE형 레코드 nonce와 맞춰야 할 때다. 그 밖의 레코드 채널은
// secretstream(9.4절)이 낫다(데이터 한도·재키잉·끝 표시를 스스로 처리한다).
//
// 계약(어기면 nonce가 다시 쓰인다):
// - 키 하나는 평생 sealer 하나(한 방향)만 쓴다. 두 방향은 키를 따로 둔다(kx의 rx·tx 키처럼 역할이
//   묶인 키가 좋다). 같은 키 byte를 다시 가져오거나 같은 인자로 다시 파생해 새 sealer를 만들면 seq
//   0부터 같은 nonce가 나온다.
// - 키는 세션마다 새것이어야 한다. kx로 얻는다면 봉인하는 쪽 자기 키 쌍이 세션마다 새로 만든
//   것이어야 한다. kx 세션 키에는 세션 난수가 없어 같은 두 키 쌍이면 늘 같은 키이므로, 상대만 새 키
//   쌍이면 공격자가 상대의 이전 공개키를 재전송해 같은 키를 다시 만들게 하고 seq 0부터 같은 nonce를
//   얻는다(KeyExchange.h). kx 키에는 iv를 0으로 둔다.
// - TryDerive로 얻는다면 subkeyId는 쓰기 전에 durable하게 올린 값이고, 같은 master를 쓰는 다른
//   프로세스·기기와 카운터를 공유하지 않는다. 상태를 둘 수 없으면 128 bit 이상 무작위 salt의
//   HKDF(Kdf.h)로 세션 master를 먼저 만든다. 무작위 64 bit subkeyId는 2^32 세션 규모에서 충돌한다.
// - iv는 nonce 길이(AeadInfo::nonceBytes)의 비밀 값이고 키와 같은 키 스케줄에서 나온다(TryDerive가
//   함께 뽑는다). 전송하지 않고 상수 라벨로 두지 않는다. 0이어도 nonce는 겹치지 않지만 다중 사용자
//   보강(키마다 다른 nonce 오프셋, AES-128-GCM·AEGIS-128L처럼 128 bit 키에서 의미가 크다)을 잃는다.
// - KeyBlob에서 푼 AeadKey로 만들지 않는다(복원할 때마다 seq 0부터 같은 nonce가 나온다).
// - 재시작은 새 키로 한다. 같은 키로 순번을 이어 가는 길은 두지 않는다(봉인 뒤 저장 전에 죽으면
//   nonce가 다시 쓰인다, NIST SP 800-38D 9.1절).
//
// 한도: seq는 0부터 AeadSequenceExhausted - 1까지 쓴다. 다 쓰거나, AES-GCM에서 정상 메시지가 키당
// 블록 한도(AeadInfo::maxBlocksPerKey, 11절)를 넘으면 IsExhausted()가 true가 되고 이후 TrySeal은
// CapacityExceeded다. 이 한도는 마지막 방어선이다. 키 교체 메시지를 옛 키로 보낼 자리가 남도록
// 프로토콜은 그 전(예: UsedBlocks()가 한도의 절반)에 새 키로 바꾼다. IsExhausted()가 false인
// CapacityExceeded는 출력 버퍼가 작거나 평문이 메시지 상한(maxMessageBytes)을 넘은 것이다.
//
// 한 스레드에서 쓴다(봉인과 전송 큐 넣기를 함께 직렬화한다). move-only이고 옮겨 간 객체는
// StaleHandle이다. 키와 iv는 키 저장소에 있고 소멸 때 지운다.
//
// 사용 예:
//   auto sealer = hazmat::AeadSequenceSealer::TryDerive(sessionMaster, epoch, ChannelContext,
//                                                        AeadAlgorithm::Aes256Gcm);
//   if (!sealer) { return sealer.Error(); }
//   auto written = sealer.Value().TrySeal(header, payload, record);
//   if (!written && sealer.Value().IsExhausted()) { /* 새 키로 바꾼다 */ }
class AeadSequenceSealer {
public:
    AeadSequenceSealer() noexcept = default;

    // key를 소유한다. 빈 키는 StaleHandle, iv 길이가 nonce 길이와 다르면 InvalidArgument, 키
    // 저장소가 찼으면 OutOfMemory이고, 실패하면 key는 그대로 남는다. iv는 복사한다.
    [[nodiscard]] static Result<AeadSequenceSealer> TryCreate(AeadKey&& key,
                                                              Span<const uint8> iv) noexcept;

    // master에서 키와 iv를 함께 뽑아 만든다(Kdf.h 타입 파생, 키·iv에 서로 다른 타입 tag). 받는 쪽은
    // 같은 인자로 AeadSequenceOpener::TryDerive를 부른다. 방향마다 subkeyId나 context를 다르게
    // 둔다. 알고리즘 오류는 AeadKey::TryGenerate와 같고 먼저 검사한다. 빈 master는 StaleHandle, 키
    // 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<AeadSequenceSealer> TryDerive(const KdfKey& master, uint64 subkeyId,
                                                              KdfContext context,
                                                              AeadAlgorithm algorithm) noexcept;

    // 다음 seq로 봉인한다. 출력 = 암호문 ‖ tag이고 쓴 byte 수를 돌려준다. 오류는 TrySealWithNonce와
    // 같고 쓰기 전에 돌아오며 seq·블록 집계를 바꾸지 않는다. 다 썼거나 정상 메시지가 블록 한도를
    // 넘으면 CapacityExceeded(소진)다.
    [[nodiscard]] Result<usize> TrySeal(Span<const uint8> associatedData,
                                        Span<const uint8> plaintext, Span<uint8> sealed) noexcept;

    // 다음 봉인에 쓸 seq다. 다 썼으면 AeadSequenceExhausted다.
    [[nodiscard]] uint64 NextSequence() const noexcept {
        return next_;
    }

    [[nodiscard]] bool IsExhausted() const noexcept {
        return next_ == AeadSequenceExhausted;
    }

    // 지금까지 봉인한 16 byte 블록 수다(메시지마다 평문 블록 + AD 블록 + 1, 넘치면 포화). AES-GCM은
    // 이 값이 AeadInfo::maxBlocksPerKey를 넘지 않게 막는다. 다른 알고리즘은 진단용이다.
    [[nodiscard]] uint64 UsedBlocks() const noexcept {
        return usedBlocks_;
    }

    [[nodiscard]] AeadAlgorithm Algorithm() const noexcept {
        return key_.Algorithm();
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::SequenceTestAccess;

    AeadSequenceSealer(AeadKey&& key, SecretBytes&& iv) noexcept;

    AeadKey key_;
    SecretBytes iv_;
    uint64 next_ = 0;
    uint64 usedBlocks_ = 0;
};

// AeadSequenceSealer의 받는 쪽이다. 같은 키·iv로 만들고 같은 순서로 연다. 계약(키 하나 = opener
// 하나, 세션마다 새 키, 재시작은 새 키)은 sealer와 같다. 같은 키로 opener를 다시 만들면 이미 받은
// 메시지의 재전송을 받아들인다.
//
// - 신뢰할 수 있고 순서가 보장되는 전송에서만 쓴다. 빠지거나 바뀐 순서·재전송·변조·잘린 입력·다른
//   AD는 모두 AuthenticationFailed이고 plaintext 전체를 지운다. 이 실패는 sticky다: 이후 모든
//   TryOpen이 AuthenticationFailed다(HasFailed). 세션과 키를 버린다(11절). 순번을 다 쓴 뒤 온
//   메시지도 같다.
// - 허용되지 않은 겹침은 실패 뒤에도 쓰기 전에 InvalidArgument다. plaintext가 sealed 길이로
//   정해지는 평문보다 작으면 인증 전에 CapacityExceeded다(상태 불변). 길이는 전송로에 보이는 값이라
//   새 정보는 아니지만, 프레이밍 계층이 최대 레코드 길이를 먼저 막는다.
// - 스트림 끝(절단) 인증은 프로토콜 몫이다(TLS close_notify처럼). 이 객체에는 끝 표시가 없다.
// - 한 스레드에서 쓴다. move-only이고 옮겨 간 객체는 StaleHandle이다.
class AeadSequenceOpener {
public:
    AeadSequenceOpener() noexcept = default;

    // 인자와 오류는 AeadSequenceSealer::TryCreate와 같다.
    [[nodiscard]] static Result<AeadSequenceOpener> TryCreate(AeadKey&& key,
                                                              Span<const uint8> iv) noexcept;

    // 인자와 오류는 AeadSequenceSealer::TryDerive와 같다. 같은 인자면 같은 키·iv가 나온다.
    [[nodiscard]] static Result<AeadSequenceOpener> TryDerive(const KdfKey& master, uint64 subkeyId,
                                                              KdfContext context,
                                                              AeadAlgorithm algorithm) noexcept;

    // 다음 seq로 연다. 평문 byte 수를 돌려준다. 오류는 TryOpenWithNonce와 같고, 앞선 인증 실패나
    // 소진 뒤에는 늘 AuthenticationFailed(plaintext 소거)다.
    [[nodiscard]] Result<usize> TryOpen(Span<const uint8> associatedData, Span<const uint8> sealed,
                                        Span<uint8> plaintext) noexcept;

    // 다음 열기에 쓸 seq다. 다 썼으면 AeadSequenceExhausted다.
    [[nodiscard]] uint64 NextSequence() const noexcept {
        return next_;
    }

    [[nodiscard]] bool IsExhausted() const noexcept {
        return next_ == AeadSequenceExhausted;
    }

    // 멈췄는가(인증 실패, 또는 소진 뒤 메시지를 받음). 멈춘 뒤 TryOpen은 늘 AuthenticationFailed다.
    [[nodiscard]] bool HasFailed() const noexcept {
        return failed_;
    }

    [[nodiscard]] AeadAlgorithm Algorithm() const noexcept {
        return key_.Algorithm();
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::SequenceTestAccess;

    AeadSequenceOpener(AeadKey&& key, SecretBytes&& iv) noexcept;

    AeadKey key_;
    SecretBytes iv_;
    uint64 next_ = 0;
    bool failed_ = false;
};

} // namespace hazmat

} // namespace rocks::crypto
