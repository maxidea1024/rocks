#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Crypto/Secret.h>

// 암호 해시와 XOF다(Docs/Design/Crypto.ko.md 4·8·9.2절). 구현은 libsodium이다.
//
// - BLAKE2b: 기본 해시다(digest 16–64 byte, 기본 32). 키를 주는 BLAKE2b keyed는 MAC이므로 Mac.h에
//   있다.
// - SHA-512, SHA3-256, SHA3-512: 상호운용용 고정 길이 해시다. SHA-256은 Core Sha256 하나만
//   둔다(결정 D7).
// - SHAKE128·SHAKE256·TurboSHAKE128·TurboSHAKE256: 원하는 길이만큼 짜내는 XOF다.
//
// 모든 함수는 여러 스레드에서 동시에 불러도 된다(상태 객체는 한 번에 한 스레드). 출력 Span이 입력과
// 겹치면 쓰기 전에 InvalidArgument다(7절 제자리 규칙). 조각 입력(pieces)은 조각을 이어 붙인 것과
// 같은 결과다.
//
// 사용 예:
//   uint8 digest[32];
//   auto hashed = crypto::TryBlake2b(content, Span<uint8>(digest));   // 32 byte BLAKE2b
//   if (!hashed) { return hashed.Error(); }
//   crypto::Sha512Digest sha = crypto::Sha512(content);
//
//   auto state = crypto::HashState::TryCreate(crypto::HashAlgorithm::Sha3_256);
//   state.Value().TryUpdate(header); state.Value().TryUpdate(body);
//   state.Value().TryFinish(Span<uint8>(digest));
namespace rocks::crypto {

inline constexpr usize Blake2bMinDigestBytes = 16;
inline constexpr usize Blake2bMaxDigestBytes = 64;
inline constexpr usize Blake2bDefaultDigestBytes = 32;

// 알고리즘마다 다른 타입의 고정 길이 digest 값이다(D8: 값 타입 + View). Tag가 달라 서로 바꿔 넣을
// 수 없다.
template <typename Tag, usize N>
struct FixedDigest {
    static constexpr usize Bytes = N;
    uint8 bytes[N] = {};

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes, N);
    }
};

struct Sha512Tag;
struct Sha3_256Tag;
struct Sha3_512Tag;
using Sha512Digest = FixedDigest<Sha512Tag, 64>;
using Sha3_256Digest = FixedDigest<Sha3_256Tag, 32>;
using Sha3_512Digest = FixedDigest<Sha3_512Tag, 64>;

// BLAKE2b다. digest.Size()가 출력 길이다(16–64 byte, 아니면 InvalidArgument).
[[nodiscard]] Result<void> TryBlake2b(Span<const uint8> input, Span<uint8> digest) noexcept;
[[nodiscard]] Result<void> TryBlake2b(Span<const Span<const uint8>> pieces,
                                      Span<uint8> digest) noexcept;

[[nodiscard]] Sha512Digest Sha512(Span<const uint8> input) noexcept;
[[nodiscard]] Sha512Digest Sha512(Span<const Span<const uint8>> pieces) noexcept;
[[nodiscard]] Sha3_256Digest Sha3_256(Span<const uint8> input) noexcept;
[[nodiscard]] Sha3_256Digest Sha3_256(Span<const Span<const uint8>> pieces) noexcept;
[[nodiscard]] Sha3_512Digest Sha3_512(Span<const uint8> input) noexcept;
[[nodiscard]] Sha3_512Digest Sha3_512(Span<const Span<const uint8>> pieces) noexcept;

// XOF: output.Size()만큼 짜낸다(0이면 아무것도 하지 않는 성공). TurboSHAKE의 domain은
// 0x01–0x7F다(기본 0x1F, 밖이면 InvalidArgument). 같은 입력에서 짧은 출력은 긴 출력의 앞부분이다.
[[nodiscard]] Result<void> TryShake128(Span<const uint8> input, Span<uint8> output) noexcept;
[[nodiscard]] Result<void> TryShake256(Span<const uint8> input, Span<uint8> output) noexcept;
[[nodiscard]] Result<void> TryTurboShake128(Span<const uint8> input, Span<uint8> output,
                                            uint8 domain = 0x1F) noexcept;
[[nodiscard]] Result<void> TryTurboShake256(Span<const uint8> input, Span<uint8> output,
                                            uint8 domain = 0x1F) noexcept;

enum class HashAlgorithm : uint8 {
    Blake2b,
    Sha512,
    Sha3_256,
    Sha3_512,
};

// 증분 해시다. 큰 입력을 조각으로 넣는다. move-only이며 한 번에 한 스레드만 쓴다.
//
// - 상태는 allocator에서 할당한다(기본 GetSecretAllocator: 평문이 지나간 상태이므로 해제 때
//   지운다). 키 없는 해시라 키 저장소는 쓰지 않는다.
// - TryFinish 뒤에는 Reset 전까지 TryUpdate·TryFinish가 InvalidArgument다. 이동된 객체는
//   StaleHandle이다.
// - 소멸·Reset 때 상태를 지운다.
class HashState {
public:
    HashState() noexcept = default;
    ~HashState() noexcept;

    HashState(const HashState&) = delete;
    HashState& operator=(const HashState&) = delete;
    HashState(HashState&& other) noexcept;
    HashState& operator=(HashState&& other) noexcept;

    // digestBytes는 BLAKE2b만 고른다(16–64, 0이면 32). 다른 알고리즘은 0이거나 고정 길이와 같아야
    // 한다. 상태 할당 실패는 allocator의 오류(OutOfMemory 등)다.
    [[nodiscard]] static Result<HashState>
    TryCreate(HashAlgorithm algorithm, usize digestBytes = 0,
              Allocator& allocator = GetSecretAllocator()) noexcept;

    [[nodiscard]] Result<void> TryUpdate(Span<const uint8> input) noexcept;
    [[nodiscard]] Result<void> TryUpdate(Span<const Span<const uint8>> pieces) noexcept;

    // digest.Size()는 DigestBytes()와 같아야 한다(아니면 InvalidArgument, 상태 불변).
    [[nodiscard]] Result<void> TryFinish(Span<uint8> digest) noexcept;

    // 같은 알고리즘·길이의 처음 상태로 되돌린다. 이동된 객체는 아무 일도 하지 않는다.
    void Reset() noexcept;

    [[nodiscard]] HashAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    [[nodiscard]] usize DigestBytes() const noexcept {
        return digestBytes_;
    }

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    void* state_ = nullptr;
    HashAlgorithm algorithm_ = HashAlgorithm::Blake2b;
    usize digestBytes_ = 0;
    bool finished_ = false;
};

enum class XofAlgorithm : uint8 {
    Shake128,
    Shake256,
    TurboShake128,
    TurboShake256,
};

// 증분 XOF다. 입력을 넣은 뒤 TrySqueeze를 여러 번 불러 이어지는 출력을 얻는다. 첫 TrySqueeze 뒤에는
// Reset 전까지 TryUpdate가 InvalidArgument다. 상태·수명 규칙은 HashState와 같다.
class XofState {
public:
    XofState() noexcept = default;
    ~XofState() noexcept;

    XofState(const XofState&) = delete;
    XofState& operator=(const XofState&) = delete;
    XofState(XofState&& other) noexcept;
    XofState& operator=(XofState&& other) noexcept;

    // domain은 TurboSHAKE만 쓴다(0x01–0x7F, 기본 0x1F). SHAKE에 0x1F가 아닌 값을 주면
    // InvalidArgument다.
    [[nodiscard]] static Result<XofState>
    TryCreate(XofAlgorithm algorithm, uint8 domain = 0x1F,
              Allocator& allocator = GetSecretAllocator()) noexcept;

    [[nodiscard]] Result<void> TryUpdate(Span<const uint8> input) noexcept;
    [[nodiscard]] Result<void> TrySqueeze(Span<uint8> output) noexcept;
    void Reset() noexcept;

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    void* state_ = nullptr;
    XofAlgorithm algorithm_ = XofAlgorithm::Shake128;
    uint8 domain_ = 0x1F;
    bool squeezing_ = false;
};

} // namespace rocks::crypto
