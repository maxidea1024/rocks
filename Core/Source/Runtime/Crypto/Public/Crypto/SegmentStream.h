#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Core/StreamProgress.h>
#include <Crypto/Aead.h>
#include <Crypto/Kdf.h>
#include <Crypto/KeyId.h>
#include <Crypto/Secret.h>

// 세그먼트 스트림(Rocks 세그먼트 형식 v1)이다(Docs/Design/Crypto.ko.md 9.1·9.5절, 형식 명세
// Docs/Design/CryptoSegmentFormat.ko.md). 파일처럼 길이가 크고 일부만 읽거나 병렬로 처리해야 하는
// 데이터에 쓴다. 구성은 STREAM(스트림별 HKDF 키 + 세그먼트 번호 nonce + last 표시)에 key
// commitment와 헤더(알고리즘·세그먼트 크기·키 ID·salt) 결합을 더한 것이다.
//
// - 형식: 헤더 88 byte ‖ 세그먼트 0 ‖ … ‖ 세그먼트 n−1. 세그먼트는 암호문 ‖ tag 16 byte이고,
// 마지막을
//   뺀 모든 세그먼트는 S(= 2^segmentSizeLog2, tag 포함) byte로 꽉 찬다. 같은 평문의 인코딩은
//   하나다.
// - 키: SegmentStreamKey(마스터 32 byte + 키 ID)다. 스트림마다 무작위 salt로 세그먼트 키와
//   commitment를 새로 파생하므로 같은 키로 스트림을 몇 개 만들어도 nonce가 겹치지 않는다. 복호는
//   헤더의 키 ID로 키를 찾고(TryPeekSegmentHeader), 세그먼트를 열기 전에 commitment로 키·헤더·AD를
//   확인한다.
// - associatedData: 스트림 전체에 한 번 묶인다(SHA-256(AD)가 키 파생에 들어간다). 세그먼트마다 다른
//   AD는 없다.
// - 알고리즘: ChaCha20-Poly1305(모든 기기, 기본)와 AES-256-GCM(AES HW 기기군, 없으면 Unsupported).
//   AES 형식은 스트림당 세그먼트 암호문 합(tag 포함)을 2^40 − 16 byte로 제한한다(11절 기밀성 목표
//   2^−57). 두 형식 모두 세그먼트 수는 2^32개까지다.
// - 복호는 세그먼트를 검증한 뒤에만 그 평문을 내보낸다. 세그먼트 단위로 인증된 평문이 나가지만
// 스트림
//   완결(절단 없음)은 순차 복호기의 TryFinish 성공, 임의 접근은 TryOpen(마지막 세그먼트 검증)이
//   확정한다. 전체가 원자적이어야 하는 소비자는 완결 확인 뒤에 쓴다.
// - 오류(2·11절): 옮겨 간 객체 StaleHandle → 호출자 인자 InvalidArgument(상태 불변) → 한도·출력
// 부족
//   CapacityExceeded → 메시지에서 온 byte의 형식·인증 위반 AuthenticationFailed(출력 전체를 지우고
//   복호기는 sticky). 헤더의 모르는 버전·알고리즘과 HW 없는 AES는 Unsupported다. 공격자 입력에서는
//   InvalidArgument·Fatal이 나오지 않는다.
// - 압축과 함께 쓸 때는 압축 다음 암호화다(같은 StreamProgress 역압 규약이라 중간 버퍼 하나로
// 잇는다,
//   9.6절). 공격자가 영향을 주는 데이터와 비밀을 한 압축 문맥에 섞지 않는다. 섞으면 암호문 길이가
//   비밀을 흘린다(CRIME·BREACH와 같은 원리).
// - 메모리: 세그먼트 키와 키 상태는 키 저장소(SecretArena), 세그먼트 버퍼·비트맵은
// bufferAllocator(기본
//   GetSecretAllocator, 해제 때 지운다)에 둔다. 할당기는 객체보다 오래 살아야 한다.
//
// 사용 예(순차 암호화, 출력 창이 작아도 된다):
//   auto key = crypto::SegmentStreamKey::TryGenerate(keyId);
//   auto encryptor = crypto::SegmentStreamEncryptor::TryCreate(key.Value(), {}, associatedData);
//   auto step = encryptor.Value().TryProcess(chunk, window);    // step.Value().consumed·produced
//   auto last = encryptor.Value().TryFinish(window);             // complete가 될 때까지 반복
//
// 사용 예(임의 접근 복호):
//   auto info = crypto::TryPeekSegmentHeader(header);           // 키 ID로 키를 찾는다
//   auto layout = crypto::TryGetSegmentLayout(info.Value().Params(), fileBytes);
//   // lastSegment = file[layout.lastSegmentOffset, + layout.lastSegmentBytes)
//   auto reader = crypto::SegmentStreamRandomAccess::TryOpen(key, ad, header, lastSegment,
//   fileBytes); auto range = reader.Value().TryMapPlaintextRange(offset, bytes); auto written =
//   reader.Value().TryOpenSegment(index, segmentCiphertext, plain);
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
struct SegmentStreamFactory;
struct SegmentStreamTestAccess;
} // namespace detail

// 영속 형식에 기록하는 값이다(헤더 byte 5). 값을 바꾸지 않고 끝에 덧붙인다.
enum class SegmentStreamAlgorithm : uint8 {
    ChaCha20Poly1305 = 1, // ChaCha20-Poly1305-IETF(RFC 8439), 모든 기기
    Aes256Gcm = 2,        // AES-256-GCM, AES HW 기기군(없으면 Unsupported)
};

inline constexpr usize SegmentHeaderBytes = 88;
inline constexpr usize SegmentTagBytes = 16;
inline constexpr usize SegmentStreamKeyBytes = 32;
inline constexpr uint8 SegmentSizeLog2Min = 12;     // 4 KiB
inline constexpr uint8 SegmentSizeLog2Max = 24;     // 16 MiB
inline constexpr uint8 SegmentSizeLog2Default = 16; // 64 KiB(결정 D6)

// 스트림당 세그먼트 수 상한이다(세그먼트 번호는 nonce의 64 bit 자리에 들어가지만 2^32개로 묶는다).
inline constexpr uint64 SegmentStreamMaxSegments = uint64{1} << 32;

// AES-256-GCM 형식의 스트림당 세그먼트 암호문 합(헤더 제외, tag 포함) 상한이다. 세그먼트 AD가 비어
// 세그먼트마다 GCM 블록이 ⌈암호문 / 16⌉개이므로 블록 2^36 − 1개(기밀성 이점 (σ + 1)²/2^129 ≤ 2^−57,
// 11절 순번 봉인기와 같은 목표)다.
inline constexpr uint64 SegmentStreamAesMaxBodyBytes = (uint64{1} << 40) - 16;

// 스트림 매개변수다. 헤더에 기록되고 키 파생에 묶인다.
struct SegmentStreamParams {
    SegmentStreamAlgorithm algorithm = SegmentStreamAlgorithm::ChaCha20Poly1305;
    // 세그먼트 크기 S(tag 포함)의 log2다. SegmentSizeLog2Min–SegmentSizeLog2Max.
    uint8 segmentSizeLog2 = SegmentSizeLog2Default;
};

// 세그먼트 스트림 마스터 키(32 byte)와 키 ID다. 헤더의 keyId는 Id()에서 온다(따로 지정하지 않으므로
// 어긋날 수 없다). 매개변수는 헤더에 기록되고 키 파생에 묶이므로 같은 마스터 키를 다른 매개변수로
// 써도 파생 키가 다르다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에 써도 된다. 키
// 저장소에 있고 소멸 때 지운다. 평문 내보내기는 hazmat::TryExportKey다.
class SegmentStreamKey {
public:
    SegmentStreamKey() noexcept = default;

    // OS 난수 키다. Nil id는 InvalidArgument, OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면
    // OutOfMemory다.
    [[nodiscard]] static Result<SegmentStreamKey> TryGenerate(KeyId id) noexcept;

    // keyBytes는 정확히 32 byte다. Nil id나 길이 오류는 InvalidArgument다. keyBytes는 호출자가
    // 지운다.
    [[nodiscard]] static Result<SegmentStreamKey> TryImport(KeyId id,
                                                            Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] KeyId Id() const noexcept {
        return id_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    SegmentStreamKey(KeyId id, SecretBytes&& key) noexcept : id_(id), key_(Move(key)) {
    }

    KeyId id_;
    SecretBytes key_;
};

// master에서 세그먼트 스트림 키를 뽑는다(Kdf.h 타입 파생, 키 타입을 섞는다). id는 결과 키의 Id()다.
// 빈 master는 StaleHandle, Nil id는 InvalidArgument, 키 저장소가 찼으면 OutOfMemory다.
[[nodiscard]] Result<SegmentStreamKey> TryDeriveSegmentStreamKey(const KdfKey& master,
                                                                 uint64 subkeyId,
                                                                 KdfContext context,
                                                                 KeyId id) noexcept;

// 헤더에서 읽은 값이다. commitment 확인 전이라 아직 인증되지 않았다.
struct SegmentHeaderInfo {
    SegmentStreamAlgorithm algorithm = SegmentStreamAlgorithm::ChaCha20Poly1305;
    uint8 segmentSizeLog2 = SegmentSizeLog2Default;
    KeyId keyId;

    [[nodiscard]] SegmentStreamParams Params() const noexcept {
        return SegmentStreamParams{algorithm, segmentSizeLog2};
    }
};

// 헤더(SegmentHeaderBytes byte)를 읽어 키 ID와 매개변수를 돌려준다. 키 ID로 키를 찾는 데만 쓰고,
// 값은 복호 객체를 만들 때 commitment로 확인된다. byte 순서대로 판정한다: 5 byte 미만이나 magic
// 불일치 AuthenticationFailed → 모르는 버전 Unsupported → 길이가 SegmentHeaderBytes가 아님
// AuthenticationFailed → 모르는 알고리즘 Unsupported → 범위 밖 크기·모르는 flags bit·Nil keyId
// AuthenticationFailed. 비밀을 다루지 않는다.
[[nodiscard]] Result<SegmentHeaderInfo> TryPeekSegmentHeader(Span<const uint8> header) noexcept;

// 전체 암호문 크기에서 정해지는 세그먼트 배치다. 오프셋은 헤더를 포함한 전체 암호문 기준이다.
struct SegmentLayout {
    uint64 segmentCount = 0;      // n(1 이상)
    uint64 segmentBytes = 0;      // S(tag 포함). 마지막을 뺀 세그먼트의 크기
    uint64 lastSegmentOffset = 0; // 88 + (n − 1)·S
    uint64 lastSegmentBytes = 0;  // r(16–S)
    uint64 plaintextBytes = 0;    // 전체 평문 크기
};

// 매개변수의 스트림당 평문 상한이다. ChaCha20-Poly1305는 2^32 × (S − 16), AES-256-GCM은 세그먼트
// 암호문 합 상한에서 정해지는 2^40 − 2^44 / S − 16 byte다. 범위 밖 매개변수는 InvalidArgument다.
[[nodiscard]] Result<uint64>
TryGetSegmentStreamMaxPlaintextBytes(const SegmentStreamParams& params) noexcept;

// plaintextBytes를 봉인한 스트림의 전체 암호문 크기(헤더 포함)다. 범위 밖 매개변수는
// InvalidArgument, 평문 상한을 넘으면 CapacityExceeded다.
[[nodiscard]] Result<uint64> TryGetSegmentStreamCiphertextBytes(const SegmentStreamParams& params,
                                                                uint64 plaintextBytes) noexcept;

// 전체 암호문 크기(헤더 포함, 파일 크기 등 메시지에서 온 값)에서 세그먼트 배치를 계산한다. 범위 밖
// 매개변수는 InvalidArgument다. 형식이 허용하지 않는 크기(본문 16 byte 미만, 세그먼트 2^32개 초과,
// 세그먼트가 둘 이상인데 마지막이 16 byte 이하, AES 세그먼트 암호문 합 상한 초과)는
// AuthenticationFailed다.
[[nodiscard]] Result<SegmentLayout> TryGetSegmentLayout(const SegmentStreamParams& params,
                                                        uint64 ciphertextBytes) noexcept;

// 순차 암호화다. 헤더와 세그먼트를 역압(StreamProgress)이 있는 byte 스트림으로 낸다. 출력 창은 1
// byte여도 된다. 마지막 세그먼트인지는 다음 입력이나 TryFinish가 와야 알 수 있으므로 세그먼트
// 출력은 한 세그먼트 늦게 나온다(평문 한 세그먼트와 봉인 한 세그먼트를 버퍼에 둔다).
class SegmentStreamEncryptor {
public:
    SegmentStreamEncryptor() noexcept = default;
    ~SegmentStreamEncryptor() noexcept;

    SegmentStreamEncryptor(const SegmentStreamEncryptor&) = delete;
    SegmentStreamEncryptor& operator=(const SegmentStreamEncryptor&) = delete;
    SegmentStreamEncryptor(SegmentStreamEncryptor&& other) noexcept;
    SegmentStreamEncryptor& operator=(SegmentStreamEncryptor&& other) noexcept;

    // 무작위 salt로 헤더(commitment 포함)를 만들고 첫 출력으로 내보낼 준비를 한다. associatedData는
    // 키 파생에 한 번 묶인다(복사하지 않는다). 빈 키는 StaleHandle, 범위 밖 매개변수는
    // InvalidArgument, AES HW가 없으면 Unsupported, OS 난수 실패는 DependencyFailed, 키 저장소·버퍼
    // 할당 실패는 OutOfMemory다. 버퍼는 S + (S − 16) byte다.
    [[nodiscard]] static Result<SegmentStreamEncryptor>
    TryCreate(const SegmentStreamKey& key, const SegmentStreamParams& params,
              Span<const uint8> associatedData,
              Allocator& bufferAllocator = GetSecretAllocator()) noexcept;

    // plaintext를 소비하고 만들어진 암호문을 output 앞에 쓴다. 출력이 모자라면 입력 일부만
    // 소비한다. complete는 입력을 모두 소비했고 내보낼 출력이 남지 않았는지다(마지막 세그먼트가 될
    // 수 있는 평문은 다음 입력이나 TryFinish까지 버퍼에 남는다).
    // - 옮겨 간 객체·Reset 뒤는 StaleHandle, TryFinish를 부른 뒤나 plaintext와 output이 겹치면
    //   InvalidArgument다.
    // - 누적 평문이 TryGetSegmentStreamMaxPlaintextBytes를 넘게 되는 입력은 읽기 전에
    //   CapacityExceeded다(상태 불변, TryFinish로 지금까지의 스트림을 닫을 수 있다).
    [[nodiscard]] Result<StreamProgress> TryProcess(Span<const uint8> plaintext,
                                                    Span<uint8> output) noexcept;

    // 입력 끝을 알리고 남은 평문을 마지막 세그먼트(last = 1)로 봉인해 내보낸다. complete가 될
    // 때까지 출력 공간을 주며 반복해 부른다. 끝난 뒤의 호출은 {0, 0, true}다. 옮겨 간 객체는
    // StaleHandle이다.
    [[nodiscard]] Result<StreamProgress> TryFinish(Span<uint8> output) noexcept;

    // 상태·버퍼를 지우고 빈 객체가 된다.
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

private:
    friend struct detail::SegmentStreamFactory;
    friend struct detail::SegmentStreamTestAccess;

    void* state_ = nullptr;
};

// 순차 복호다. 헤더부터 전체 암호문을 역압이 있는 byte 스트림으로 받는다. 세그먼트 하나를 다 받아
// 검증한 뒤에만 그 평문을 내보낸다(버퍼는 세그먼트 하나 크기). 마지막 판정에는 시도가 없다: 꽉 찬
// 세그먼트 뒤에 byte가 더 오면 last = 0으로, TryFinish가 오면 last = 1로 연다.
class SegmentStreamDecryptor {
public:
    SegmentStreamDecryptor() noexcept = default;
    ~SegmentStreamDecryptor() noexcept;

    SegmentStreamDecryptor(const SegmentStreamDecryptor&) = delete;
    SegmentStreamDecryptor& operator=(const SegmentStreamDecryptor&) = delete;
    SegmentStreamDecryptor(SegmentStreamDecryptor&& other) noexcept;
    SegmentStreamDecryptor& operator=(SegmentStreamDecryptor&& other) noexcept;

    // 헤더를 받기 전이므로 마스터 키를 복사해 둔다(헤더를 처리한 뒤 지운다). 빈 키는 StaleHandle,
    // 키 저장소·할당 실패는 OutOfMemory다. key는 만든 뒤 버려도 된다.
    [[nodiscard]] static Result<SegmentStreamDecryptor>
    TryCreate(const SegmentStreamKey& key, Span<const uint8> associatedData,
              Allocator& bufferAllocator = GetSecretAllocator()) noexcept;

    // ciphertext를 소비하고 검증된 평문을 plaintext 앞에 쓴다. complete는 입력을 모두 소비했고
    // 내보낼 평문이 남지 않았는지다.
    // - 옮겨 간 객체는 StaleHandle, ciphertext와 plaintext가 겹치거나 TryFinish를 부른 뒤면
    //   InvalidArgument다(상태 불변).
    // - 헤더 형식 위반, 다른 키 ID, commitment 불일치(잘못된 키·헤더·AD 변조), 세그먼트 검증 실패,
    //   세그먼트 수·AES 한도를 넘는 입력은 AuthenticationFailed다. 헤더의 모르는 버전·알고리즘과 HW
    //   없는 AES는 Unsupported, 헤더 처리 중 할당 실패는 OutOfMemory다. 이 실패들은 plaintext
    //   전체를 지우고 Reset 전까지 같은 오류다.
    [[nodiscard]] Result<StreamProgress> TryProcess(Span<const uint8> ciphertext,
                                                    Span<uint8> plaintext) noexcept;

    // 입력 끝을 알린다. 남은 세그먼트를 last = 1로 열어 그 평문을 내보낸다(출력이 모자라면
    // complete가 될 때까지 반복). 헤더나 마지막 세그먼트가 없거나 비정규이거나 검증에
    // 실패하면(절단) AuthenticationFailed다(sticky, plaintext 소거). complete가 된 성공 호출이
    // 스트림 완결을 확정한다. 끝난 뒤의 호출은 {0, 0, true}다.
    [[nodiscard]] Result<StreamProgress> TryFinish(Span<uint8> plaintext) noexcept;

    // 상태·버퍼를 지우고 빈 객체가 된다(sticky 실패도 지운다).
    void Reset() noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return state_ == nullptr;
    }

private:
    friend struct detail::SegmentStreamTestAccess;

    void* state_ = nullptr;
};

// 평문 범위를 읽는 데 필요한 세그먼트다(TryMapPlaintextRange). segmentCount가 0이면 빈 범위다.
struct SegmentRange {
    uint64 firstSegment = 0;       // 첫 세그먼트 번호
    uint64 segmentCount = 0;       // 세그먼트 수
    uint64 ciphertextOffset = 0;   // 첫 세그먼트의 위치(헤더 포함 전체 기준)
    uint64 ciphertextBytes = 0;    // 읽을 암호문 byte 수(연속 구간)
    uint64 firstSegmentOffset = 0; // 첫 세그먼트 평문 안에서 범위가 시작하는 위치
};

// 전체 암호문 크기를 알 때 쓰는 임의 접근 복호다. 세그먼트마다 독립적으로 검증·복호한다. const
// 함수는 여러 스레드가 동시에 불러도 된다(키 상태는 생성 뒤 바뀌지 않는다). move-only다.
class SegmentStreamRandomAccess {
public:
    SegmentStreamRandomAccess() noexcept = default;

    // commitment를 확인하고 마지막 세그먼트를 한 번 검증한다(절단 검출, 정확한 평문 크기). 마지막
    // 세그먼트 검증은 평문을 쓰지 않는 검증 전용 경로라 scratch가 필요 없다.
    // - header: 전체 암호문의 앞 SegmentHeaderBytes byte. lastSegment: TryGetSegmentLayout이 알려
    // 준
    //   [lastSegmentOffset, + lastSegmentBytes) 구간. ciphertextBytes: 헤더를 포함한 전체 크기.
    // - 빈 키는 StaleHandle, 헤더·크기 형식 위반(TryPeekSegmentHeader·TryGetSegmentLayout 규칙),
    //   다른 키 ID, lastSegment 길이 불일치, commitment·마지막 세그먼트 검증 실패는
    //   AuthenticationFailed, 모르는 버전·알고리즘과 HW 없는 AES는 Unsupported, 키 저장소가 찼으면
    //   OutOfMemory다.
    [[nodiscard]] static Result<SegmentStreamRandomAccess>
    TryOpen(const SegmentStreamKey& key, Span<const uint8> associatedData, Span<const uint8> header,
            Span<const uint8> lastSegment, uint64 ciphertextBytes) noexcept;

    [[nodiscard]] uint64 PlaintextBytes() const noexcept {
        return layout_.plaintextBytes;
    }

    [[nodiscard]] uint64 SegmentCount() const noexcept {
        return layout_.segmentCount;
    }

    [[nodiscard]] const SegmentLayout& Layout() const noexcept {
        return layout_;
    }

    // 평문 [offset, offset + bytes)를 읽는 데 필요한 세그먼트 번호 범위와 암호문 byte 범위다.
    // bytes가 0이면 빈 범위다. offset + bytes가 넘치거나 평문 크기를 넘으면 InvalidArgument, 빈
    // 객체는 StaleHandle이다.
    [[nodiscard]] Result<SegmentRange> TryMapPlaintextRange(uint64 offset,
                                                            uint64 bytes) const noexcept;

    // 세그먼트 index의 암호문(마지막을 뺀 세그먼트는 S byte, 마지막은 lastSegmentBytes)을
    // 검증·복호해 plaintext 앞에 쓰고 평문 byte 수를 돌려준다. 마지막 세그먼트인지는
    // ciphertextBytes로 정해진다.
    // - 빈 객체는 StaleHandle, index ≥ SegmentCount()와 제자리(같은 시작 주소)가 아닌 겹침은
    //   InvalidArgument, plaintext가 그 세그먼트 평문보다 작으면 CapacityExceeded다(쓰지 않는다).
    // - 길이가 틀린 암호문과 검증 실패는 AuthenticationFailed이고 plaintext 전체를 지운다. 이
    // 객체는
    //   실패해도 상태가 없다(다른 세그먼트는 계속 열 수 있다).
    // AES-256-GCM은 검증 전에 복호한 byte를 plaintext에 잠시 쓰고 실패하면 지운다(Aead.h 규칙과
    // 같다).
    [[nodiscard]] Result<usize> TryOpenSegment(uint64 index, Span<const uint8> ciphertextSegment,
                                               Span<uint8> plaintext) const noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    AeadKey key_;
    SegmentLayout layout_;
};

// 전체 평문 크기를 알 때 쓰는 병렬 봉인이다. 세그먼트 i는 암호문의 [88 + i·S, …) 자리에 들어간다.
// TrySealSegment는 여러 스레드가 동시에 불러도 된다. 같은 index를 두 번 봉인하면
// AlreadyExists다(nonce 재사용 차단). 순서는 길이 검사 → index 원자적 claim → 봉인이고 claim 뒤에는
// 실패 경로가 없다(성공 뒤에 표시하면 두 스레드가 같은 index를 서로 다른 평문으로 동시에 봉인할 수
// 있다). 봉인을 다시 하려면 새 writer(새 salt)로 전체를 다시 쓴다. move-only이며, 옮기거나
// 소멸시키는 동안 다른 스레드가 쓰지 않게 한다.
class SegmentStreamParallelWriter {
public:
    SegmentStreamParallelWriter() noexcept = default;
    ~SegmentStreamParallelWriter() noexcept;

    SegmentStreamParallelWriter(const SegmentStreamParallelWriter&) = delete;
    SegmentStreamParallelWriter& operator=(const SegmentStreamParallelWriter&) = delete;
    SegmentStreamParallelWriter(SegmentStreamParallelWriter&& other) noexcept;
    SegmentStreamParallelWriter& operator=(SegmentStreamParallelWriter&& other) noexcept;

    // 무작위 salt로 헤더를 만들어 header(SegmentHeaderBytes byte)에 쓴다. 비트맵(세그먼트당 1
    // bit)은 bufferAllocator에서 할당한다. 빈 키는 StaleHandle, 범위 밖 매개변수와 header 길이
    // 오류는 InvalidArgument, AES HW가 없으면 Unsupported, 평문 상한 초과는 CapacityExceeded, OS
    // 난수 실패는 DependencyFailed, 할당 실패는 OutOfMemory다. 실패하면 header를 쓰지 않는다.
    [[nodiscard]] static Result<SegmentStreamParallelWriter>
    TryCreate(const SegmentStreamKey& key, const SegmentStreamParams& params,
              Span<const uint8> associatedData, uint64 plaintextBytes, Span<uint8> header,
              Allocator& bufferAllocator = GetSecretAllocator()) noexcept;

    // 세그먼트 index를 봉인해 sealed 앞 plaintext.Size() + 16 byte에 쓰고 그 수를 돌려준다.
    // plaintext는 마지막을 뺀 세그먼트면 S − 16 byte, 마지막이면 lastSegmentBytes − 16 byte여야
    // 한다.
    // - 빈 객체는 StaleHandle, index ≥ 세그먼트 수·plaintext 길이 오류·제자리(같은 시작 주소)가
    // 아닌
    //   겹침은 InvalidArgument, sealed가 작으면 CapacityExceeded다(claim하지 않는다).
    // - 이미 봉인한 index는 AlreadyExists다(쓰지 않는다).
    [[nodiscard]] Result<usize> TrySealSegment(uint64 index, Span<const uint8> plaintext,
                                               Span<uint8> sealed) noexcept;

    // 세그먼트 배치다(헤더 포함 전체 암호문 크기 = lastSegmentOffset + lastSegmentBytes).
    [[nodiscard]] const SegmentLayout& Layout() const noexcept {
        return layout_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::SegmentStreamFactory;

    void Release() noexcept;

    AeadKey key_;
    SegmentLayout layout_;
    Allocator* allocator_ = nullptr;
    uint64* claimed_ = nullptr;
    usize claimedWords_ = 0;
};

namespace hazmat {

// 마스터 키 byte를 평문으로 output에 복사한다(10.4절). output.Size()는 SegmentStreamKeyBytes여야
// 한다.
[[nodiscard]] Result<void> TryExportKey(const SegmentStreamKey& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
