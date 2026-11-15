#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Crypto/Secret.h>

// 서명(Ed25519·Ed25519ph, RFC 8032)이다(Docs/Design/Crypto.ko.md 4·8·9.2절). 구현은 libsodium
// crypto_sign_ed25519다.
//
// - 서명은 결정적이다. 같은 키·메시지는 늘 같은 서명이다(RFC 8032).
// - 검증 규칙은 libsodium이 정본이다: 비정규 S, small-order R, 비정규·small-order 공개키를
//   거부하고, 검증식은 cofactorless([S]B = R + [k]A)다. 다른 구현이 받아들이는 서명("acceptable"
//   경계 사례, ZIP-215식 cofactored 검증)을 거부할 수 있으므로, 서명을 만드는 도구도 같은 Rocks
//   경로로 검증한다(4절). 비정규 R encoding은 계산한 R과의 byte 비교에서 거부된다.
// - 혼합 차수(torsion 성분이 있는) 공개키·R은 거부하지 않고 cofactorless 식이 맞으면 받는다. 키를
//   등록할 때 소수 차수 부분군 검사가 필요하면 따로 한다(libsodium
//   crypto_core_ed25519_is_valid_point).
// - Ed25519ph(RFC 8032 5.1, context 없음)는 메시지의 SHA-512에 dom2 접두(phflag 1)를 붙여 서명한다.
//   큰 메시지를 조각으로 넣을 때 쓴다(Ed25519phSigner·Ed25519phVerifier). 순수 Ed25519(m)와도,
//   SHA-512(m)에 대한 순수 서명과도 호환되지 않으므로 형식은 SignatureAlgorithm으로 구분해 적는다.
// - 서명 키는 키 저장소에 있고 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에 서명해도 된다. 공개키와
//   서명은 일반 값 타입이다. 키 byte 내보내기는 hazmat::TryExportSeed다(서명 키의 정본은 seed다).
//
// 사용 예:
//   auto key = crypto::Ed25519SigningKey::TryGenerate();
//   if (!key) { return key.Error(); }
//   auto signature = crypto::TrySign(key.Value(), manifest);
//   const crypto::Ed25519PublicKey publicKey = key.Value().PublicKey();
//   auto verified = crypto::TryVerify(publicKey, manifest, signature.Value().View());
//   if (!verified) { /* AuthenticationFailed: 변조되었거나 다른 키다 */ }
namespace rocks::crypto {

namespace detail {
struct KeyAccess;
} // namespace detail

// 영속 형식(서명 헤더·KeyBlob)에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class SignatureAlgorithm : uint8 {
    Ed25519 = 1,
    Ed25519ph = 2,
};

inline constexpr usize Ed25519PublicKeyBytes = 32;
inline constexpr usize Ed25519SignatureBytes = 64;
inline constexpr usize Ed25519SeedBytes = 32;

struct Ed25519PublicKey {
    uint8 bytes[Ed25519PublicKeyBytes] = {};

    // 32 byte가 아니면 InvalidArgument다. 점이 올바른지(정규·small-order 아님)는 검증 때 보고,
    // 틀리면 TryVerify가 AuthenticationFailed다.
    [[nodiscard]] static Result<Ed25519PublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }

    [[nodiscard]] bool operator==(const Ed25519PublicKey& other) const noexcept = default;
};

struct Ed25519Signature {
    uint8 bytes[Ed25519SignatureBytes] = {};

    [[nodiscard]] Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes);
    }
};

// Ed25519 서명 키다. move-only이며 생성 뒤 바뀌지 않는다. 키 저장소에 libsodium 비밀키(seed 32 byte
// ‖ 공개키 32 byte)를 둔다.
class Ed25519SigningKey {
public:
    Ed25519SigningKey() noexcept = default;

    // OS 난수 seed로 만든다. OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<Ed25519SigningKey> TryGenerate() noexcept;

    // 32 byte seed(RFC 8032의 비밀키)에서 만든다. 길이가 다르면 InvalidArgument다. seed는 호출자가
    // 지운다.
    [[nodiscard]] static Result<Ed25519SigningKey> TryFromSeed(Span<const uint8> seed) noexcept;

    // 공개키다. 빈 키는 모두 0이다.
    [[nodiscard]] Ed25519PublicKey PublicKey() const noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    friend struct detail::KeyAccess;

    explicit Ed25519SigningKey(SecretBytes&& key) noexcept;

    SecretBytes key_;
};

// message에 서명한다. 빈 키는 StaleHandle이다.
[[nodiscard]] Result<Ed25519Signature> TrySign(const Ed25519SigningKey& key,
                                               Span<const uint8> message) noexcept;

// 서명을 확인한다. 맞으면 성공이다. 서명이 64 byte가 아니거나, 맞지 않거나, 공개키가 올바른 점이
// 아니면 AuthenticationFailed다(어느 검사인지는 알리지 않는다). message는 기밀 평문이어도 된다(DIT
// 구간에서 해시한다, 6.3절).
[[nodiscard]] Result<void> TryVerify(const Ed25519PublicKey& key, Span<const uint8> message,
                                     Span<const uint8> signature) noexcept;

// Ed25519ph 서명(RFC 8032 5.1, context 없음)이다. 메시지를 조각으로 넣고 TryFinish로 서명한다.
// - SHA-512 상태는 allocator에서 할당한다(키가 없는 상태라 키 저장소를 쓰지 않는다, 9.1절).
// - TryFinish 뒤에는 Reset 전까지 TryUpdate·TryFinish가 InvalidArgument다.
// - 한 스레드에서 쓴다. move-only이고 기본 생성한 객체와 옮겨 간 객체는 StaleHandle이다.
class Ed25519phSigner {
public:
    Ed25519phSigner() noexcept = default;
    ~Ed25519phSigner() noexcept;

    Ed25519phSigner(const Ed25519phSigner&) = delete;
    Ed25519phSigner& operator=(const Ed25519phSigner&) = delete;
    Ed25519phSigner(Ed25519phSigner&& other) noexcept;
    Ed25519phSigner& operator=(Ed25519phSigner&& other) noexcept;

    // 상태 할당 실패는 allocator의 오류(OutOfMemory 등)다.
    [[nodiscard]] static Result<Ed25519phSigner>
    TryCreate(Allocator& allocator = GetSecretAllocator()) noexcept;

    [[nodiscard]] Result<void> TryUpdate(Span<const uint8> input) noexcept;
    [[nodiscard]] Result<void> TryUpdate(Span<const Span<const uint8>> pieces) noexcept;

    // 넣은 메시지에 서명한다. 빈 키는 StaleHandle이고 상태를 바꾸지 않는다.
    [[nodiscard]] Result<Ed25519Signature> TryFinish(const Ed25519SigningKey& key) noexcept;

    // 처음 상태로 되돌린다. 이동된 객체는 아무 일도 하지 않는다.
    void Reset() noexcept;

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    void* state_ = nullptr;
    bool finished_ = false;
};

// Ed25519ph 검증이다. 상태·수명 규칙은 Ed25519phSigner와 같다. 검증 실패는 sticky다(9.1절): 실패한
// 뒤에는 Reset 전까지 TryUpdate·TryFinish가 AuthenticationFailed다(성공한 뒤에는 InvalidArgument).
class Ed25519phVerifier {
public:
    Ed25519phVerifier() noexcept = default;
    ~Ed25519phVerifier() noexcept;

    Ed25519phVerifier(const Ed25519phVerifier&) = delete;
    Ed25519phVerifier& operator=(const Ed25519phVerifier&) = delete;
    Ed25519phVerifier(Ed25519phVerifier&& other) noexcept;
    Ed25519phVerifier& operator=(Ed25519phVerifier&& other) noexcept;

    [[nodiscard]] static Result<Ed25519phVerifier>
    TryCreate(Allocator& allocator = GetSecretAllocator()) noexcept;

    [[nodiscard]] Result<void> TryUpdate(Span<const uint8> input) noexcept;
    [[nodiscard]] Result<void> TryUpdate(Span<const Span<const uint8>> pieces) noexcept;

    // 넣은 메시지의 서명을 확인한다. 오류는 TryVerify와 같고, 결과와 관계없이 상태를 끝낸다.
    [[nodiscard]] Result<void> TryFinish(const Ed25519PublicKey& key,
                                         Span<const uint8> signature) noexcept;

    // 신뢰하는 공개키 여러 개(키 회전) 중 서명을 확인하는 첫 키의 순번을 돌려준다. 상태를 키마다
    // 복사해 확인하므로 스트림을 다시 해시하지 않는다. trusted가 비면 InvalidArgument(상태
    // 불변)이고, 나머지 오류는 TryFinish와 같다.
    [[nodiscard]] Result<usize> TryFinishAny(Span<const Ed25519PublicKey> trusted,
                                             Span<const uint8> signature) noexcept;

    void Reset() noexcept;

private:
    void Release() noexcept;

    Allocator* allocator_ = nullptr;
    void* state_ = nullptr;
    bool finished_ = false;
    bool verifyFailed_ = false;
};

namespace hazmat {

// 서명 키의 seed(32 byte)를 평문으로 output에 복사한다(10.4절, 서명 키의 정본이 seed라 이름이
// TryExportKey가 아니다). 빈 키는 StaleHandle, output.Size()가 32가 아니면 InvalidArgument다.
// output은 호출자가 지운다.
[[nodiscard]] Result<void> TryExportSeed(const Ed25519SigningKey& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
