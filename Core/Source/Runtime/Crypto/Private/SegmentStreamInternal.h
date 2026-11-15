#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Error.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Crypto/Aead.h>
#include <Crypto/KeyId.h>
#include <Crypto/Secret.h>
#include <Crypto/SegmentStream.h>

// 세그먼트 스트림 형식 v1의 조립·파생·세그먼트 연산과 스트림 객체 상태다(Docs/Design/Crypto.ko.md
// 9.5절, Docs/Design/CryptoSegmentFormat.ko.md). 구현은 SegmentFormat.cpp(형식·파생·연산),
// SegmentStreamSequential.cpp(순차 암·복호), SegmentStreamRandomAccess.cpp(임의 접근·병렬
// writer)다. 공개 경로가 인자를 먼저 검사하고 이 함수들을 부른다. 여기 함수는 그 검사를 다시 하지
// 않고, 어기면 범위 밖 쓰기로 이어지는 조건만 Fatal로 막는다.
namespace rocks::crypto::detail {

inline constexpr uint8 SegmentFormatVersion = 1;
inline constexpr usize SegmentSaltOffset = 24;
inline constexpr usize SegmentSaltBytes = 32;
// 헤더 앞 56 byte(magic·버전·알고리즘·log2 S·flags·keyId·salt)는 키 파생 info에 들어가고, 뒤 32
// byte는 commitment다.
inline constexpr usize SegmentCommitmentOffset = 56;
inline constexpr usize SegmentCommitmentBytes = 32;
inline constexpr usize SegmentAssociatedHashBytes = 32;
inline constexpr usize SegmentNonceBytes = 12;

// 매개변수가 형식 범위 안인가(열거값, log2 S 12–24).
[[nodiscard]] bool IsValidSegmentParams(const SegmentStreamParams& params) noexcept;

// 세그먼트 AEAD 알고리즘이다(ChaCha20-Poly1305-IETF 또는 AES-256-GCM).
[[nodiscard]] AeadAlgorithm SegmentAeadAlgorithm(SegmentStreamAlgorithm algorithm) noexcept;

// 범위 안 매개변수의 평문 상한이다(공개 TryGetSegmentStreamMaxPlaintextBytes와 같은 값).
[[nodiscard]] uint64 SegmentMaxPlaintextBytes(const SegmentStreamParams& params) noexcept;

// 범위 안 매개변수와 상한 이하 평문의 배치다(마지막 세그먼트 크기 포함).
[[nodiscard]] SegmentLayout SegmentLayoutForPlaintext(const SegmentStreamParams& params,
                                                      uint64 plaintextBytes) noexcept;

// SHA-256(associatedData)를 digest(32 byte)에 쓴다. 키 파생 info의 일부다(D7: libsodium SHA-256은
// HMAC·HKDF 같은 파생 내부에서만 쓴다).
void HashSegmentAssociatedData(Span<const uint8> associatedData, Span<uint8> digest) noexcept;

// 스트림을 시작한다: header(88 byte)의 앞 56 byte를 쓰고 세그먼트 키와 commitment를 파생해 뒤 32
// byte를 채운다. salt는 32 byte다(공개 경로는 OS 난수, 시험은 고정 값).
// 빈 키 StaleHandle → 범위 밖 매개변수 InvalidArgument → AES HW 없음 Unsupported → 키 저장소
// OutOfMemory 순이다. 실패하면 header 내용은 정해지지 않는다(호출자 버퍼가 아닌 곳에 쓴다).
[[nodiscard]] Result<AeadKey> TryStartSegmentStream(const SegmentStreamKey& key,
                                                    const SegmentStreamParams& params,
                                                    Span<const uint8> associatedData,
                                                    Span<const uint8> salt,
                                                    Span<uint8> header) noexcept;

// 받은 헤더(88 byte)를 확인하고 세그먼트 키를 만든다: 헤더 형식(TryPeekSegmentHeader 규칙) → 키 ID
// 일치(AuthenticationFailed) → AES HW(Unsupported) → 파생(OutOfMemory) → commitment 상수 시간
// 비교(AuthenticationFailed). master는 32 byte 마스터 키다. 성공하면 params에 헤더 매개변수를 쓴다.
[[nodiscard]] Result<AeadKey> TryOpenSegmentStream(Span<const uint8> master, KeyId keyId,
                                                   Span<const uint8> associatedHash,
                                                   Span<const uint8> header,
                                                   SegmentStreamParams& params) noexcept;

// 세그먼트 index를 봉인한다. sealed.Size() == plaintext.Size() + 16이고 sealed는 plaintext와 같은
// 주소에서 시작하거나 겹치지 않는다(아니면 Fatal).
void SealSegment(const AeadKey& key, uint64 index, bool last, Span<const uint8> plaintext,
                 Span<uint8> sealed) noexcept;

// 세그먼트 index를 열어 plaintext(sealed.Size() − 16 byte)에 쓴다. 맞으면 true, 틀리면 plaintext를
// 지우고 false다. plaintext는 sealed와 같은 주소에서 시작하거나 겹치지 않는다.
[[nodiscard]] bool OpenSegment(const AeadKey& key, uint64 index, bool last,
                               Span<const uint8> sealed, Span<uint8> plaintext) noexcept;

// 평문을 쓰지 않고 tag만 확인한다(libsodium 복호 함수의 m = NULL 검증 경로).
[[nodiscard]] bool VerifySegment(const AeadKey& key, uint64 index, bool last,
                                 Span<const uint8> sealed) noexcept;

// salt를 값으로 받는 생성 경로다. 공개 TryCreate는 OS 난수 salt를 넘기고, 시험은 고정 salt로
// golden을 재현한다(11절 결정성: 바꿔 둘 수 있는 전역 hook 없이 값을 받는 함수). 인자·오류는 공개
// TryCreate와 같고 salt는 32 byte여야 한다(아니면 InvalidArgument).
struct SegmentStreamFactory {
    [[nodiscard]] static Result<SegmentStreamEncryptor>
    TryCreateEncryptor(const SegmentStreamKey& key, const SegmentStreamParams& params,
                       Span<const uint8> associatedData, Span<const uint8> salt,
                       Allocator& bufferAllocator) noexcept;

    [[nodiscard]] static Result<SegmentStreamParallelWriter>
    TryCreateParallelWriter(const SegmentStreamKey& key, const SegmentStreamParams& params,
                            Span<const uint8> associatedData, uint64 plaintextBytes,
                            Span<const uint8> salt, Span<uint8> header,
                            Allocator& bufferAllocator) noexcept;
};

// 순차 암호기 상태다. allocator에서 할당하고 버퍼는 [평문 S − 16][봉인 S]다.
// - plaintextFill: 버퍼에 모은 평문(다음 세그먼트 후보)이다.
// - staged[stagedBegin, stagedEnd): 아직 내보내지 않은 출력(헤더 또는 봉인한 세그먼트)이다.
struct SegmentEncryptorState {
    Allocator* allocator = nullptr;
    AeadKey key;
    uint8* buffer = nullptr;
    usize segmentBytes = 0;
    usize plaintextFill = 0;
    usize stagedBegin = 0;
    usize stagedEnd = 0;
    uint64 index = 0;          // 다음에 봉인할 세그먼트 번호
    uint64 plaintextBytes = 0; // 소비한 평문 합
    uint64 maxPlaintextBytes = 0;
    bool finishing = false;
    bool lastSealed = false;
    bool finished = false;

    [[nodiscard]] Span<uint8> Plaintext() const noexcept {
        return Span<uint8>(buffer, segmentBytes - SegmentTagBytes);
    }

    [[nodiscard]] Span<uint8> Staged() const noexcept {
        return Span<uint8>(buffer + segmentBytes - SegmentTagBytes, segmentBytes);
    }
};

// 순차 복호기 상태다. 헤더를 처리하기 전에는 master 사본이 있고, 처리한 뒤에는 key와 세그먼트
// 버퍼(S byte)가 있다.
// - buffer[0, bufferFill): 받은 세그먼트 암호문이다. 연 뒤에는 [pendingBegin, pendingEnd)가 아직
//   내보내지 않은 검증된 평문이다.
struct SegmentDecryptorState {
    Allocator* allocator = nullptr;
    SecretBytes master;
    KeyId keyId;
    uint8 associatedHash[SegmentAssociatedHashBytes] = {};
    uint8 header[SegmentHeaderBytes] = {};
    usize headerFill = 0;
    AeadKey key;
    SegmentStreamParams params;
    uint8* buffer = nullptr;
    usize segmentBytes = 0;
    usize bufferFill = 0;
    usize pendingBegin = 0;
    usize pendingEnd = 0;
    uint64 index = 0;     // 다음에 열 세그먼트 번호
    uint64 bodyBytes = 0; // 연 세그먼트(마지막 제외)의 암호문 합
    ErrorCode failure = ErrorCode::None;
    bool failed = false;
    bool finishing = false;
    bool lastOpened = false;
    bool finished = false;
};

} // namespace rocks::crypto::detail
