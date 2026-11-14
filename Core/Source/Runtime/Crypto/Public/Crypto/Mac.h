#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/Secret.h>

// 메시지 인증 코드(MAC)다(Docs/Design/Crypto.ko.md 4·8·9.2절). 구현은 libsodium이다.
//
// - HMAC-SHA-256(tag 32 byte), HMAC-SHA-512(tag 64 byte): 상호운용용이다. 키는 16–4096 byte다.
//   블록(64·128 byte)보다 긴 키는 HMAC 규칙대로 부를 때마다 먼저 해시된다. 상호운용 형식이 16
//   byte보다 짧은 키를 요구하면 hazmat::TryImportShortMacKey를 쓴다.
// - BLAKE2b keyed(tag 32 byte): 기본 MAC이다. 키는 16–64 byte다. tag 길이는 BLAKE2b parameter
//   block에 들어가므로 잘라서 16·64 byte tag를 얻을 수 없다(다른 길이는 새 알고리즘 값이 필요하다).
//
// MAC의 보안 강도는 키 bit 수를 넘지 못한다. 키는 TryGenerate로 만들고(권장 길이 32·64·32 byte),
// 비밀번호처럼 엔트로피가 낮은 값은 키로 쓰지 말고 PasswordHash·Kdf로 키를 얻는다.
//
// 키는 알고리즘에 묶인 MacKey이고 키 저장소에 있다(Secret.h). 검증은 상수 시간이며, 불일치와 길이가
// 틀린 tag는 모두 AuthenticationFailed다(2절: tag는 메시지에서 온 byte이고 잘린 tag는 받지 않는다).
// 잘린 HMAC(RFC 4868 등)이 필요한 상호운용 형식은 TryMac으로 전체 tag를 만든 뒤 앞부분을
// ConstantTimeEquals로 비교한다(잘린 길이는 16 byte 이상을 권장한다). 출력 tag가 입력과 겹치면
// 쓰기 전에 InvalidArgument다.
//
// 사용 예:
//   auto key = crypto::MacKey::TryGenerate(crypto::MacAlgorithm::Blake2bKeyed);
//   if (!key) { return key.Error(); }
//   uint8 tag[crypto::Blake2bKeyedTagBytes];
//   if (auto made = crypto::TryMac(key.Value(), message, Span<uint8>(tag)); !made) { ... }
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

class MacKey;

namespace hazmat {
// 아래(파일 끝)에서 설명한다. MacKey가 friend로 두려고 먼저 선언한다.
[[nodiscard]] Result<MacKey> TryImportShortMacKey(MacAlgorithm algorithm,
                                                  Span<const uint8> keyBytes) noexcept;
} // namespace hazmat

inline constexpr usize HmacSha256TagBytes = 32;
inline constexpr usize HmacSha512TagBytes = 64;
inline constexpr usize Blake2bKeyedTagBytes = 32;
// 모든 알고리즘의 tag 길이 상한이다. 알고리즘을 더하면 이 값과 구현의 static_assert를 함께 고친다.
inline constexpr usize MaxMacTagBytes = 64;
inline constexpr usize MinMacKeyBytes = 16;
inline constexpr usize MaxHmacKeyBytes = 4096;

// 알고리즘의 tag 길이다. 열거값이 아니면 InvalidArgument다.
[[nodiscard]] Result<usize> TryGetMacTagBytes(MacAlgorithm algorithm) noexcept;

// 알고리즘에 묶인 MAC 키다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
// TryMac·TryVerifyMac에 써도 된다. 이동된 키는 빈 키다. 소멸 때 지운다.
class MacKey {
public:
    MacKey() noexcept = default;

    // 알고리즘의 권장 길이(HMAC-SHA-256 32, HMAC-SHA-512 64, BLAKE2b keyed 32 byte)로 OS 난수 키를
    // 만든다. 열거값이 아니면 InvalidArgument, OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면
    // OutOfMemory다.
    [[nodiscard]] static Result<MacKey> TryGenerate(MacAlgorithm algorithm) noexcept;

    // keyBytes를 복사한 키다. 길이가 알고리즘 범위(HMAC 16–4096, BLAKE2b keyed 16–64) 밖이면
    // InvalidArgument, 키 저장소가 찼으면 OutOfMemory다. keyBytes는 호출자가 지운다.
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
    friend Result<MacKey> hazmat::TryImportShortMacKey(MacAlgorithm algorithm,
                                                       Span<const uint8> keyBytes) noexcept;

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
//
// - TryFinish: tag.Size()가 TagBytes()와 다르면 InvalidArgument이고 상태는 그대로다(다시 부를 수
//   있다). 성공하면 끝난다.
// - TryFinishVerify: 결과와 관계없이 상태를 끝낸다. 불일치·길이 오류는 AuthenticationFailed다.
// - 끝난 뒤의 호출은 검증이 실패했으면 AuthenticationFailed(sticky, 9.1절), 아니면
//   InvalidArgument다. 기본 생성한 객체와 이동된 객체는 StaleHandle이다.
// - 끝난 직후와 소멸 때 상태를 지운다.
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
    [[nodiscard]] Result<void> TryUpdate(Span<const Span<const uint8>> pieces) noexcept;
    [[nodiscard]] Result<void> TryFinish(Span<uint8> tag) noexcept;
    [[nodiscard]] Result<void> TryFinishVerify(Span<const uint8> expectedTag) noexcept;

    [[nodiscard]] MacAlgorithm Algorithm() const noexcept {
        return algorithm_;
    }

    // TryFinish에 줄 tag 길이다.
    [[nodiscard]] usize TagBytes() const noexcept;

private:
    void Release() noexcept;
    [[nodiscard]] Result<void> CheckActive() const noexcept;

    void* state_ = nullptr;
    MacAlgorithm algorithm_ = MacAlgorithm::Blake2bKeyed;
    bool finished_ = false;
    bool verifyFailed_ = false;
};

namespace hazmat {

// HMAC 키를 1 byte부터 받는다. 16 byte보다 짧은 키를 요구하는 기존 상호운용 형식(예: RFC 4231 시험
// 사례 2의 4 byte 키)에서만 쓴다. MAC 강도는 키 bit 수를 넘지 못한다. BLAKE2b keyed는 libsodium
// 하한 16 byte가 그대로다. 나머지 규칙은 MacKey::TryImport와 같다.
[[nodiscard]] Result<MacKey> TryImportShortMacKey(MacAlgorithm algorithm,
                                                  Span<const uint8> keyBytes) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
