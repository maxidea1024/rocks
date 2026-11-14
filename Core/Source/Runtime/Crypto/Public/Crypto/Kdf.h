#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Move.h>
#include <Core/Result.h>
#include <Crypto/Mac.h>
#include <Crypto/Secret.h>

// 키 파생(KDF)이다(Docs/Design/Crypto.ko.md 4·8·10.1절). 구현은 libsodium이다.
//
// - HKDF-SHA-256/512(RFC 5869): 상호운용 형식과 공유 비밀(키 교환 결과 등)에서 키를 뽑을 때 쓴다.
//   출력은 호출자 Span이다. 비밀을 받는 Span은 SecretBytes::MutableView처럼 지워지는 메모리를 쓴다.
// - BLAKE2b KDF(libsodium crypto_kdf): 마스터 키 하나에서 용도별 키를 64 bit id와 8 byte context로
//   나눠 뽑는다. 결과는 일반 Span이 아니라 SecretBytes나 알고리즘에 묶인 키 타입이다.
//
// 사용 예:
//   constexpr crypto::KdfContext SaveGame("SaveGame");   // 정확히 8 문자, 컴파일 시간 상수
//   auto master = crypto::KdfKey::TryGenerate();
//   if (!master) { return master.Error(); }
//   using crypto::MacAlgorithm;
//   auto macKey = crypto::TryDeriveMacKey(master.Value(), 1, SaveGame, MacAlgorithm::HmacSha256);
namespace rocks::crypto {

// ---- HKDF ----

enum class HkdfAlgorithm : uint8 {
    Sha256 = 1,
    Sha512 = 2,
};

inline constexpr usize HkdfSha256PrkBytes = 32;
inline constexpr usize HkdfSha512PrkBytes = 64;
inline constexpr usize HkdfSha256MaxOutputBytes = 255 * HkdfSha256PrkBytes;
inline constexpr usize HkdfSha512MaxOutputBytes = 255 * HkdfSha512PrkBytes;

// extract와 expand를 한 번에 한다. salt가 비어 있으면 RFC 5869대로 해시 길이의 0 byte다. output은
// 해시 길이 × 255 이하다(넘거나 알고리즘이 열거값이 아니면 InvalidArgument). 빈 output은 아무것도
// 하지 않는 성공이다. output이 다른 인자와 겹치면 쓰기 전에 InvalidArgument다.
[[nodiscard]] Result<void> TryHkdf(HkdfAlgorithm algorithm, Span<const uint8> inputKeyMaterial,
                                   Span<const uint8> salt, Span<const uint8> info,
                                   Span<uint8> output) noexcept;

// PRK = HMAC(salt, ikm)다. prk.Size()는 해시 길이(32·64)와 같아야 한다.
[[nodiscard]] Result<void> TryHkdfExtract(HkdfAlgorithm algorithm, Span<const uint8> salt,
                                          Span<const uint8> inputKeyMaterial,
                                          Span<uint8> prk) noexcept;

// PRK에서 output.Size()만큼 뽑는다. prk.Size()는 해시 길이와 같아야 한다.
[[nodiscard]] Result<void> TryHkdfExpand(HkdfAlgorithm algorithm, Span<const uint8> prk,
                                         Span<const uint8> info, Span<uint8> output) noexcept;

// ---- BLAKE2b KDF ----

// 파생 용도를 나누는 8 byte context다. 컴파일 시간 문자열 상수(정확히 8 문자)로만 만든다.
// 런타임·외부 입력으로 context를 고르지 못하게 하려는 것이다. 엔진이 쓰는 context는
// Crypto/Labels.h에 모아 중복을 시험으로 막는다.
class KdfContext {
public:
    static constexpr usize Bytes = 8;

    template <usize N>
    consteval explicit KdfContext(const char (&text)[N]) noexcept : bytes_{} {
        static_assert(N == Bytes + 1, "KdfContext needs exactly 8 characters");
        for (usize i = 0; i < Bytes; ++i) {
            bytes_[i] = text[i];
        }
    }

    [[nodiscard]] constexpr const char* Data() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr bool operator==(const KdfContext& other) const noexcept {
        for (usize i = 0; i < Bytes; ++i) {
            if (bytes_[i] != other.bytes_[i]) {
                return false;
            }
        }

        return true;
    }

private:
    char bytes_[Bytes];
};

inline constexpr usize KdfKeyBytes = 32;
inline constexpr usize KdfMinDerivedBytes = 16;
inline constexpr usize KdfMaxDerivedBytes = 64;

// BLAKE2b KDF의 마스터 키(32 byte)다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
// 파생에 써도 된다. 키 저장소에 있고 소멸 때 지운다.
class KdfKey {
public:
    KdfKey() noexcept = default;

    [[nodiscard]] static Result<KdfKey> TryGenerate() noexcept;

    // keyBytes는 정확히 32 byte다(아니면 InvalidArgument). keyBytes는 호출자가 지운다.
    [[nodiscard]] static Result<KdfKey> TryImport(Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

    // 명시적 내보내기만 있다. output.Size()는 32여야 한다. 빈 키는 StaleHandle이다.
    [[nodiscard]] Result<void> TryExport(Span<uint8> output) const noexcept;

private:
    // 키 byte는 모듈 내부(detail::KeyAccess)만 읽는다. 하위 마스터 파생은 파생한 비밀을 그대로
    // 넘겨받는다.
    friend struct detail::KeyAccess;
    friend Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId,
                                          KdfContext context) noexcept;

    explicit KdfKey(SecretBytes&& key) noexcept : key_(Move(key)) {
    }

    SecretBytes key_;
};

// master에서 subkeyId·context로 bytes(16–64) 길이의 비밀을 뽑는다. 같은 입력은 언제나 같은
// 결과이고, id나 context가 다르면 독립인 값이다. 빈 키는 StaleHandle, 길이가 범위 밖이면
// InvalidArgument다.
[[nodiscard]] Result<SecretBytes> TryDeriveSecret(const KdfKey& master, uint64 subkeyId,
                                                  KdfContext context, usize bytes) noexcept;

// 알고리즘의 권장 길이로 MAC 키를 뽑는다(TryDeriveSecret 위의 타입 파생).
[[nodiscard]] Result<MacKey> TryDeriveMacKey(const KdfKey& master, uint64 subkeyId,
                                             KdfContext context, MacAlgorithm algorithm) noexcept;

// 하위 마스터 키(32 byte)를 뽑는다. 키 계층(앱 → 기능 → 항목)을 만들 때 쓴다.
[[nodiscard]] Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId,
                                             KdfContext context) noexcept;

} // namespace rocks::crypto
