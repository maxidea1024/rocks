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
// - BLAKE2b KDF: 마스터 키 하나에서 용도별 키를 64 bit id와 8 byte context로 나눠 뽑는다. 결과는
//   일반 Span이 아니라 SecretBytes나 알고리즘에 묶인 키 타입이다.
//
// 도메인 분리 지침:
// - HKDF info는 버전이 붙은 고유 label로 시작한다(예: "rocks.segment.v1" ‖ 헤더).
// - BLAKE2b KDF context는 용도(기능)마다 하나를 쓰고, id는 그 용도 안의 항목 번호로만 쓴다. "Rk"로
//   시작하는 context는 엔진 예약이고 엔진 context는 Crypto/Labels.h에 모은다. 앱은 다른 접두를
//   쓴다.
// - 타입 파생(TryDeriveMacKey·TryDeriveKdfKey)은 키 타입·알고리즘을 파생 입력에 섞으므로, 같은 (id,
//   context)라도 타입·알고리즘이 다르면 독립인 키가 나온다. TryDeriveSecret은 libsodium
//   crypto_kdf_derive_from_key와 byte가 같아(상호운용) 타입 파생과도 겹치지 않는다.
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

// extract와 expand를 한 번에 한다. salt가 비어 있으면 RFC 5869대로 해시 길이의 0 byte와 같다.
// output은 해시 길이 × 255 이하다(넘거나 알고리즘이 열거값이 아니면 InvalidArgument). 빈 output은
// 아무것도 하지 않는 성공이다. output이 다른 인자와 겹치면 쓰기 전에 InvalidArgument다.
[[nodiscard]] Result<void> TryHkdf(HkdfAlgorithm algorithm, Span<const uint8> inputKeyMaterial,
                                   Span<const uint8> salt, Span<const uint8> info,
                                   Span<uint8> output) noexcept;

// PRK = HMAC(salt, ikm)다. prk.Size()는 해시 길이(32·64)와 같아야 한다. 증분 extract는 두지
// 않는다(IKM은 공유 비밀처럼 짧다).
[[nodiscard]] Result<void> TryHkdfExtract(HkdfAlgorithm algorithm, Span<const uint8> salt,
                                          Span<const uint8> inputKeyMaterial,
                                          Span<uint8> prk) noexcept;

// PRK에서 output.Size()만큼 뽑는다. prk.Size()는 해시 길이와 정확히 같아야 한다(RFC 5869는 더 긴
// PRK도 허용하지만 libsodium이 받지 않는다).
[[nodiscard]] Result<void> TryHkdfExpand(HkdfAlgorithm algorithm, Span<const uint8> prk,
                                         Span<const uint8> info, Span<uint8> output) noexcept;

// ---- BLAKE2b KDF ----

namespace detail {
// 상수식이 아닌 함수다. KdfContext의 consteval 생성자가 부르면 컴파일 오류가 된다.
void KdfContextNeedsNulTerminatedLiteral() noexcept;
} // namespace detail

// 파생 용도를 나누는 8 byte context다. NUL로 끝나는 8 문자 문자열 상수로만, 컴파일 시간에 만든다.
// 런타임·외부 입력으로 context를 고르지 못하게 하려는 것이다. 엔진이 쓰는 context는 첫 context와
// 함께 Crypto/Labels.h에 모아 중복을 시험으로 막는다.
class KdfContext {
public:
    static constexpr usize Bytes = 8;

    template <usize N>
        requires(N == Bytes + 1)
    consteval explicit KdfContext(const char (&text)[N]) noexcept : bytes_{} {
        if (text[Bytes] != '\0') {
            detail::KdfContextNeedsNulTerminatedLiteral();
        }

        for (usize i = 0; i < Bytes; ++i) {
            bytes_[i] = text[i];
        }
    }

    [[nodiscard]] constexpr const char* Data() const noexcept {
        return bytes_;
    }

    [[nodiscard]] constexpr bool operator==(const KdfContext& other) const noexcept = default;

private:
    char bytes_[Bytes];
};

inline constexpr usize KdfKeyBytes = 32;
inline constexpr usize KdfMinDerivedBytes = 16;
inline constexpr usize KdfMaxDerivedBytes = 64;

// BLAKE2b KDF의 마스터 키(32 byte)다. move-only이며 생성 뒤 바뀌지 않으므로 여러 스레드가 동시에
// 파생에 써도 된다. 키 저장소에 있고 소멸 때 지운다. 평문 내보내기는 hazmat::TryExportKey다.
class KdfKey {
public:
    KdfKey() noexcept = default;

    // OS 난수 키다. OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다.
    [[nodiscard]] static Result<KdfKey> TryGenerate() noexcept;

    // keyBytes는 정확히 32 byte다(아니면 InvalidArgument). keyBytes는 호출자가 지운다.
    [[nodiscard]] static Result<KdfKey> TryImport(Span<const uint8> keyBytes) noexcept;

    [[nodiscard]] bool IsEmpty() const noexcept {
        return key_.IsEmpty();
    }

private:
    // 키 byte와 생성자는 모듈 내부(detail::KeyAccess)만 쓴다.
    friend struct detail::KeyAccess;

    explicit KdfKey(SecretBytes&& key) noexcept : key_(Move(key)) {
    }

    SecretBytes key_;
};

// master에서 subkeyId·context로 bytes(16–64) 길이의 비밀을 뽑는다. libsodium
// crypto_kdf_derive_from_key와 같은 byte다. 같은 입력은 언제나 같은 결과이고, id나 context가 다르면
// 독립인 값이다. 빈 키는 StaleHandle, 길이가 범위 밖이면 InvalidArgument다.
[[nodiscard]] Result<SecretBytes> TryDeriveSecret(const KdfKey& master, uint64 subkeyId,
                                                  KdfContext context, usize bytes) noexcept;

// 알고리즘의 권장 길이로 MAC 키를 뽑는다. 키 타입과 알고리즘을 파생 입력에 섞는다(위 지침).
// 알고리즘이 열거값이 아니면 InvalidArgument, 이 기기에서 쓸 수 없으면(AES HW가 없는 AES-CMAC)
// Unsupported다(파생 전에 검사한다).
[[nodiscard]] Result<MacKey> TryDeriveMacKey(const KdfKey& master, uint64 subkeyId,
                                             KdfContext context, MacAlgorithm algorithm) noexcept;

// 하위 마스터 키(32 byte)를 뽑는다. 키 계층(앱 → 기능 → 항목)을 만들 때 쓴다. 키 타입을 파생 입력에
// 섞는다.
[[nodiscard]] Result<KdfKey> TryDeriveKdfKey(const KdfKey& master, uint64 subkeyId,
                                             KdfContext context) noexcept;

// transcript(조각 목록)에 묶인 하위 마스터 키(32 byte)를 뽑는다. KEM 공유 비밀처럼 맥락(공개키·
// 암호문·역할·협상 값)에 묶어야 하는 비밀에서 세션 키를 만들 때 쓴다(예: 서명 기반 AKE의 ML-KEM
// 단독 사용, MAL-BIND). 조각마다 길이(LE64)를 앞에 붙여 넣으므로 조각 경계가 다르면 다른 키다. 키
// 타입 tag가 TryDeriveKdfKey와 달라 같은 (id, context)라도 겹치지 않는다. 빈 master는 StaleHandle,
// 키 저장소가 찼으면 OutOfMemory다.
//
// 사용 예:
//   const Span<const uint8> transcript[] = {recipientKey.View(), ciphertext, role};
//   auto session = crypto::TryDeriveBoundKdfKey(shared.Value(), 1, HandshakeContext,
//                                               Span<const Span<const uint8>>(transcript));
[[nodiscard]] Result<KdfKey>
TryDeriveBoundKdfKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                     Span<const Span<const uint8>> transcript) noexcept;

namespace hazmat {

// 키 byte(32)를 평문으로 output에 복사한다(10.4절: 시험·상호운용 도구 전용, 저장은 KeyBlob).
// output.Size()는 32여야 한다(아니면 InvalidArgument). 빈 키는 StaleHandle이다.
[[nodiscard]] Result<void> TryExportKey(const KdfKey& key, Span<uint8> output) noexcept;

} // namespace hazmat

} // namespace rocks::crypto
