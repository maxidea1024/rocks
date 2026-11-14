#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/Secret.h>

// 메시지 인증 코드(MAC)다(Docs/Design/Crypto.ko.md 4·8·9.2절). 구현은 libsodium이다.
//
// - HMAC-SHA-256(tag 32 byte), HMAC-SHA-512(tag 64 byte): 상호운용용이다. 키는 1–4096
//   byte다(블록보다 긴 키는 HMAC 규칙대로 먼저 해시된다).
// - BLAKE2b keyed(tag 32 byte): 기본 MAC이다. 키는 16–64 byte다.
//
// 키는 알고리즘에 묶인 MacKey이고 키 저장소에 있다(Secret.h). 검증은 상수 시간이며, 불일치와 길이가
// 틀린 tag는 모두 AuthenticationFailed다(2절: tag는 메시지에서 온 byte이고 잘린 tag는 받지 않는다).
// 출력 tag가 입력과 겹치면 쓰기 전에 InvalidArgument다.
//
// 사용 예:
//   auto key = crypto::MacKey::TryGenerate(crypto::MacAlgorithm::Blake2bKeyed);
//   uint8 tag[crypto::Blake2bKeyedTagBytes];
//   crypto::TryMac(key.Value(), message, Span<uint8>(tag));
//   if (!crypto::TryVerifyMac(key.Value(), message, receivedTag)) { return AuthenticationFailed; }
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
} // namespace detail

// 영속 형식에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class MacAlgorithm : uint8 {
    HmacSha256 = 1,
    HmacSha512 = 2,
    Blake2bKeyed = 3,
};

inline constexpr usize HmacSha256TagBytes = 32;
inline constexpr usize HmacSha512TagBytes = 64;
inline constexpr usize Blake2bKeyedTagBytes = 32;

// 알고리즘의 tag 길이다. 열거값이 아니면 InvalidArgument다.
[[nodiscard]] Result<usize> TryGetMacTagBytes(MacAlgorithm algorithm) noexcept;

// 알고리즘에 묶인 MAC 키다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
// TryMac·TryVerifyMac에 써도 된다. 소멸 때 지운다.
class MacKey {
public:
    MacKey() noexcept = default;

    // 알고리즘의 권장 길이(HMAC-SHA-256 32, HMAC-SHA-512 64, BLAKE2b keyed 32 byte)로 OS 난수 키를
    // 만든다.
    [[nodiscard]] static Result<MacKey> TryGenerate(MacAlgorithm algorithm) noexcept;

    // keyBytes를 복사한 키다. 길이가 알고리즘 범위 밖이면 InvalidArgument다. keyBytes는 호출자가
    // 지운다.
    [[nodiscard]] static Result<MacKey> TryImport(MacAlgorithm algorithm,
                                                  Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] MacAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

    // 키 byte를 output에 복사한다(명시적 내보내기만 있다). output.Size()는 키 길이와 같아야
    // 한다(아니면 InvalidArgument). 빈 키는 StaleHandle이다.
    [[nodiscard]] Result<void> TryExport(Span<uint8> output) const noexcept;

private:
    // 키 byte는 모듈 내부(detail::KeyAccess)만 읽는다. 공개 경로는 TryExport 하나다.
    friend struct detail::KeyAccess;

    MacKey(MacAlgorithm algorithm, SecretBytes&& key) noexcept
        : algorithm_(algorithm), key_(Move(key)) {
    }

    MacAlgorithm algorithm_ = MacAlgorithm::Blake2bKeyed;
    SecretBytes key_;
};

// tag.Size()는 알고리즘의 tag 길이와 같아야 한다(아니면 InvalidArgument). 빈 키는 StaleHandle이다.
[[nodiscard]] Result<void> TryMac(const MacKey& key, Span<const uint8> message,
                                  Span<uint8> tag) noexcept;
[[nodiscard]] Result<void> TryMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                                  Span<uint8> tag) noexcept;

// tag가 맞으면 성공, 다르거나 길이가 틀리면 AuthenticationFailed다(상수 시간 비교).
[[nodiscard]] Result<void> TryVerifyMac(const MacKey& key, Span<const uint8> message,
                                        Span<const uint8> tag) noexcept;
[[nodiscard]] Result<void> TryVerifyMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                                        Span<const uint8> tag) noexcept;

// 증분 MAC이다. 큰 메시지를 조각으로 넣는다. 상태는 키에서 파생된 비밀이므로 키 저장소 슬롯에 둔다.
// move-only이며 한 번에 한 스레드만 쓴다. 키 객체는 만든 뒤 필요 없다(상태가 키를 복사해 둔다).
// TryFinish·TryFinishVerify 뒤에는 모든 호출이 InvalidArgument이고, 이동된 객체는 StaleHandle이다.
// 소멸 때 상태를 지운다.
class MacState {
public:
    MacState() noexcept = default;
    ~MacState() noexcept;

    MacState(const MacState&) = delete;
    MacState& operator=(const MacState&) = delete;
    MacState(MacState&& other) noexcept;
    MacState& operator=(MacState&& other) noexcept;

    // 빈 키는 StaleHandle, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<MacState> TryCreate(const MacKey& key) noexcept;

    [[nodiscard]] Result<void> TryUpdate(Span<const uint8> input) noexcept;
    [[nodiscard]] Result<void> TryFinish(Span<uint8> tag) noexcept;
    [[nodiscard]] Result<void> TryFinishVerify(Span<const uint8> expectedTag) noexcept;

private:
    void Release() noexcept;

    void* state_ = nullptr;
    MacAlgorithm algorithm_ = MacAlgorithm::Blake2bKeyed;
    bool finished_ = false;
};

} // namespace rocks::crypto
