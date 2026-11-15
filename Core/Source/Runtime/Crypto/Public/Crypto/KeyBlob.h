#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Traits.h>
#include <Crypto/Aead.h>
#include <Crypto/Kdf.h>
#include <Crypto/Kem.h>
#include <Crypto/KeyExchange.h>
#include <Crypto/KeyId.h>
#include <Crypto/Mac.h>
#include <Crypto/PasswordHash.h>
#include <Crypto/SecretStream.h>
#include <Crypto/SegmentStream.h>
#include <Crypto/Signature.h>

// 키를 저장하거나 옮기는 형식(KeyBlob v1)이다(Docs/Design/Crypto.ko.md 10.4절). 키를 그대로
// 내보내면 알고리즘·ID·매개변수를 잃으므로 이 형식 하나로 둔다.
//
// - 담는 것: 키 타입·알고리즘, keyId, 비밀(키 byte 또는 seed)이다. 비밀은 wrap 키 아래
//   XChaCha20-Poly1305(무작위 nonce)로 봉인하고 nonce까지의 헤더 전체(T = blob[0, 50 + P))를 AD로
//   묶는다.
// - wrap 방식: Key는 KeyRing의 KdfKey(KEK)와 그 KeyId(wrapKeyId)를 기록한다. 여는 쪽은
//   TryPeekKeyBlob로 wrapKeyId를 읽어 KEK를 찾는다(시도 복호 없음). Password는 Argon2id(v1.3, lane
//   1, salt 16 byte)로 KEK를 얻고 비용·salt를 기록한다.
// - 실제 wrap 키는 KEK와 헤더(nonce까지)를 묶어 파생하고, 그 파생에서 commitment(32 byte)를 함께
//   뽑아 AEAD를 열기 전에 상수 시간으로 비교한다. 그래서 다른 KEK·비밀번호와 헤더 변조는 AEAD 전에
//   거부되고, 비밀번호 후보를 여러 개 시도하는 흐름에도 key commitment가 있다(2절).
// - 막지 않는 것: 같은 KEK 아래 예전 blob으로 되돌리기(롤백)·통째 교체·삭제(저장소 몫), 출처(KEK를
//   가진 쪽은 아무 keyId로나 만들 수 있다), 메타데이터 기밀성(type·알고리즘·ID·비용은 평문). 여는
//   쪽은 돌려받은 keyId가 기대한 ID인지 확인한다.
// - 비밀번호 하나로 키 여러 개를 보관할 때는 비밀번호로 KdfKey blob 하나(KEK)를 만들고 나머지는 그
//   KEK 아래 Key 방식으로 둔다(blob마다 Argon2id를 돌리지 않는다). 비밀번호는 UTF-8 NFC로 정규화한
//   byte를 넘기기를 권한다(PasswordHash.h).
// - 순번 봉인기(AeadSequenceSealer·Opener) 상태는 담지 않는다. blob에서 푼 AeadKey로 순번 봉인기를
//   만들면 복원할 때마다 seq 0부터 같은 nonce가 나오므로 그렇게 쓰지 않는다(Aead.h 계약). kx 세션
//   키·KEM 공유 비밀처럼 세션에만 쓰는 비밀도 보관하지 않는다.
//
// 오류: 호출자 인자 오류는 InvalidArgument(쓰기 전), 출력 부족·비용 상한은 CapacityExceeded,
// blob에서 온 형식 오류·타입/방식 불일치·commitment·tag 불일치는 AuthenticationFailed, 이 빌드가
// 모르는 버전·타입·방식·알고리즘과 이 기기에서 쓸 수 없는 알고리즘은 Unsupported다. 모든 거부는
// KDF·Argon2id를 돌리기 전에 끝난다(commitment·tag 제외). 쓰기 시작한 뒤 실패하면 blob 전체를
// 지운다.
//
// 읽기: Peek·Unwrap은 blob을 처음에 지역 버퍼로 한 번 복사하고 그 사본만 본다. 호출 중 원본이
// 바뀌어도(공유 메모리, 쓰기 가능한 파일 매핑) 한 시점의 byte로 판정한다. Peek와 Unwrap은 따로
// 복사하므로, Peek 결과(wrapKeyId·passwordCost)로 정책을 정했다면 두 호출에 같은 자기 버퍼를
// 넘긴다.
//
// 사용 예:
//   auto id = crypto::TryNewKeyId();
//   uint8 blob[crypto::KeyBlobMaxBytes];
//   auto written = crypto::TryWrapKey(kek, kekId, aeadKey, id.Value(), Span<uint8>(blob));
//   // 나중에
//   auto info = crypto::TryPeekKeyBlob(stored);                 // wrapKeyId로 KEK를 찾는다
//   auto restored = crypto::TryUnwrapKey<crypto::AeadKey>(kek, stored);
//   if (restored && restored.Value().id == expectedId) { /* restored.Value().key 사용 */ }
namespace rocks::crypto {

// 영속 형식에 기록하는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class KeyBlobType : uint8 {
    Aead = 1,          // 키 byte, algorithm = AeadAlgorithm
    Mac = 2,           // 키 byte(16 byte 이상, AES-CMAC은 16·32), algorithm = MacAlgorithm
    Kdf = 3,           // 키 32 byte
    Ed25519 = 4,       // seed 32 byte(Ed25519·Ed25519ph 공용이라 algorithm = 0)
    Kx = 5,            // X25519 비밀키 32 byte
    Box = 6,           // X25519 비밀키 32 byte
    Kem = 7,           // seed(X-Wing 32, ML-KEM 64), algorithm = KemAlgorithm
    SecretStream = 8,  // secretstream 키 32 byte(algorithm = 0)
    SegmentStream = 9, // 세그먼트 스트림 마스터 키 32 byte(algorithm = 0, keyId = key.Id())
};

enum class KeyBlobWrapMethod : uint8 {
    Key = 1,      // KdfKey KEK, wrapKeyId 기록
    Password = 2, // Argon2id v1.3, lane 1, salt 16 byte, 출력 32 byte
};

// blob 길이 상한(비밀번호 방식, 비밀 4096 byte)이다.
inline constexpr usize KeyBlobMaxBytes = 4218;

// 헤더에서 읽은 값이다. 인증 전이라 KEK 찾기와 비용 정책 판단에만 쓴다.
struct KeyBlobInfo {
    KeyBlobType type = KeyBlobType::Aead;
    uint8 algorithm = 0;
    KeyBlobWrapMethod method = KeyBlobWrapMethod::Key;
    KeyId keyId;
    // Key 방식만. Password 방식은 Nil이다.
    KeyId wrapKeyId;
    // Password 방식만(memoryBytes는 1 KiB 단위). Key 방식은 0이다.
    PasswordCost passwordCost;
    usize secretBytes = 0;
    // blob 전체 길이(blob이 자기 길이를 담는다).
    usize blobBytes = 0;
};

// 헤더만 읽는다(인증하지 않는다). 헤더(Key 방식 42 byte, Password 방식 50 byte)만 있으면 되고 뒤에
// 다른 byte가 붙어 있어도 받는다. byte 순서대로 판정한다: magic·version 5 byte 미만이나 magic
// 불일치는 AuthenticationFailed, 모르는 버전은 (길이와 관계없이) Unsupported, 앞 26 byte가 모자라면
// AuthenticationFailed, 모르는 타입·(타입, 알고리즘)·방식은 Unsupported, 그 뒤 keyId·길이·방식
// 매개변수의 형식 오류는 AuthenticationFailed다. 비용 상한은 보지 않는다(호출자가 passwordCost로
// 정책을 정한다).
[[nodiscard]] Result<KeyBlobInfo> TryPeekKeyBlob(Span<const uint8> blob) noexcept;

// KeyBlob에 담을 수 있는 키 타입이다(닫힌 목록).
template <typename TKey>
struct KeyBlobTraits;

template <>
struct KeyBlobTraits<AeadKey> {
    static constexpr KeyBlobType Type = KeyBlobType::Aead;
};

template <>
struct KeyBlobTraits<MacKey> {
    static constexpr KeyBlobType Type = KeyBlobType::Mac;
};

template <>
struct KeyBlobTraits<KdfKey> {
    static constexpr KeyBlobType Type = KeyBlobType::Kdf;
};

template <>
struct KeyBlobTraits<Ed25519SigningKey> {
    static constexpr KeyBlobType Type = KeyBlobType::Ed25519;
};

template <>
struct KeyBlobTraits<KxKeyPair> {
    static constexpr KeyBlobType Type = KeyBlobType::Kx;
};

template <>
struct KeyBlobTraits<BoxKeyPair> {
    static constexpr KeyBlobType Type = KeyBlobType::Box;
};

template <>
struct KeyBlobTraits<KemKeyPair> {
    static constexpr KeyBlobType Type = KeyBlobType::Kem;
};

template <>
struct KeyBlobTraits<SecretStreamKey> {
    static constexpr KeyBlobType Type = KeyBlobType::SecretStream;
};

template <>
struct KeyBlobTraits<SegmentStreamKey> {
    static constexpr KeyBlobType Type = KeyBlobType::SegmentStream;
};

// 위 9개 타입의 닫힌 목록이다. 정의는 KeyBlob.cpp의 9개 인스턴스뿐이므로 KeyBlobTraits를 다른
// 타입으로 특수화해도 목록은 늘지 않는다(링크 오류가 아니라 컴파일 오류로 막는다).
template <typename TKey>
concept KeyBlobKey =
    IsSameV<TKey, AeadKey> || IsSameV<TKey, MacKey> || IsSameV<TKey, KdfKey> ||
    IsSameV<TKey, Ed25519SigningKey> || IsSameV<TKey, KxKeyPair> || IsSameV<TKey, BoxKeyPair> ||
    IsSameV<TKey, KemKeyPair> || IsSameV<TKey, SecretStreamKey> || IsSameV<TKey, SegmentStreamKey>;

// TryUnwrapKey의 결과다. id는 blob에 기록된 keyId이고 호출자가 기대한 ID와 비교한다.
template <typename TKey>
struct UnwrappedKey {
    KeyId id;
    TKey key;
};

// key를 method로 wrap한 blob 길이다. 빈 키는 StaleHandle, method가 열거값이 아니거나 wrap할 수 없는
// 키(16 byte보다 짧은 MAC 키, hazmat)는 InvalidArgument다(TryWrapKey와 같은 규칙).
template <KeyBlobKey TKey>
[[nodiscard]] Result<usize> TryGetKeyBlobBytes(const TKey& key, KeyBlobWrapMethod method) noexcept;

// KEK(wrapKey, ID wrapKeyId) 아래 key를 keyId로 wrap한다. 쓴 byte 수를 돌려준다.
// - 빈 wrapKey·key는 StaleHandle, Nil keyId·wrapKeyId, 16 byte보다 짧은 MAC 키(hazmat), key.Id()와
//   다른 keyId(SegmentStreamKey)는 InvalidArgument, blob이 TryGetKeyBlobBytes보다 작으면
//   CapacityExceeded다(모두 쓰기 전).
// - nonce용 OS 난수 실패는 DependencyFailed, 키 저장소가 찼으면 OutOfMemory다(blob을 지운다).
template <KeyBlobKey TKey>
[[nodiscard]] Result<usize> TryWrapKey(const KdfKey& wrapKey, KeyId wrapKeyId, const TKey& key,
                                       KeyId keyId, Span<uint8> blob) noexcept;

// 비밀번호로 wrap한다. cost는 명시한다(도구·데스크톱 키 파일은 MODERATE, 모바일은 INTERACTIVE 이상
// 권장). 빈 password, Argon2id 상한(2^32 - 1 byte)보다 긴 password, cost 범위 밖(0회·8 KiB 미만),
// password와 blob이 겹치면 InvalidArgument(쓰기 전), cost 상한 초과는 CapacityExceeded, Argon2 작업
// 메모리 부족은 OutOfMemory다. 메모리 비용은 1 KiB 단위로 내려 기록하고 계산도 그 값으로 한다.
// 나머지는 TryWrapKey와 같다.
template <KeyBlobKey TKey>
[[nodiscard]] Result<usize> TryWrapKeyWithPassword(Span<const uint8> password, PasswordCost cost,
                                                   const TKey& key, KeyId keyId,
                                                   Span<uint8> blob) noexcept;

// Key 방식 blob을 연다. blob은 정확한 길이여야 한다(꼬리 byte도 AuthenticationFailed).
// - 빈 wrapKey는 StaleHandle이다.
// - 형식 오류, TKey와 다른 타입, Password 방식 blob, 다른 KEK(commitment), 변조(tag)는
//   AuthenticationFailed다(어느 검사인지 알리지 않는다).
// - 모르는 버전·타입·방식·알고리즘과 이 기기에서 쓸 수 없는 AEAD 알고리즘은 Unsupported(KDF 전)다.
// - 키 저장소가 찼으면 OutOfMemory다.
// - SegmentStreamKey는 blob의 keyId를 Id()로 가진 채 나온다.
template <KeyBlobKey TKey>
[[nodiscard]] Result<UnwrappedKey<TKey>> TryUnwrapKey(const KdfKey& wrapKey,
                                                      Span<const uint8> blob) noexcept;

// Password 방식 blob을 연다. Argon2id 상한보다 긴 password는 InvalidArgument(blob을 보기 전)다.
// 기록된 비용이 상한을 넘으면 Argon2id를 돌리지 않고 CapacityExceeded(이 기기에서 쓸 수 없는 AEAD
// 알고리즘 판정보다 먼저), 작업 메모리 부족은 OutOfMemory다. Key 방식 blob은
// AuthenticationFailed다. 나머지는 TryUnwrapKey와 같다.
template <KeyBlobKey TKey>
[[nodiscard]] Result<UnwrappedKey<TKey>> TryUnwrapKeyWithPassword(Span<const uint8> password,
                                                                  Span<const uint8> blob) noexcept;

// 정의는 KeyBlob.cpp에 있고 위 9개 타입으로만 인스턴스화한다.
#define ROCKS_CRYPTO_KEYBLOB_EXTERN(TKey)                                                          \
    extern template Result<usize> TryGetKeyBlobBytes<TKey>(const TKey&,                            \
                                                           KeyBlobWrapMethod) noexcept;            \
    extern template Result<usize> TryWrapKey<TKey>(const KdfKey&, KeyId, const TKey&, KeyId,       \
                                                   Span<uint8>) noexcept;                          \
    extern template Result<usize> TryWrapKeyWithPassword<TKey>(                                    \
        Span<const uint8>, PasswordCost, const TKey&, KeyId, Span<uint8>) noexcept;                \
    extern template Result<UnwrappedKey<TKey>> TryUnwrapKey<TKey>(const KdfKey&,                   \
                                                                  Span<const uint8>) noexcept;     \
    extern template Result<UnwrappedKey<TKey>> TryUnwrapKeyWithPassword<TKey>(                     \
        Span<const uint8>, Span<const uint8>) noexcept;

ROCKS_CRYPTO_KEYBLOB_EXTERN(AeadKey)
ROCKS_CRYPTO_KEYBLOB_EXTERN(MacKey)
ROCKS_CRYPTO_KEYBLOB_EXTERN(KdfKey)
ROCKS_CRYPTO_KEYBLOB_EXTERN(Ed25519SigningKey)
ROCKS_CRYPTO_KEYBLOB_EXTERN(KxKeyPair)
ROCKS_CRYPTO_KEYBLOB_EXTERN(BoxKeyPair)
ROCKS_CRYPTO_KEYBLOB_EXTERN(KemKeyPair)
ROCKS_CRYPTO_KEYBLOB_EXTERN(SecretStreamKey)
ROCKS_CRYPTO_KEYBLOB_EXTERN(SegmentStreamKey)

#undef ROCKS_CRYPTO_KEYBLOB_EXTERN

} // namespace rocks::crypto
