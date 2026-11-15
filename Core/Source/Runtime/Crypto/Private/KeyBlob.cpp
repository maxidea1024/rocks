#include <Crypto/KeyBlob.h>

#include <Base/Endian.h>
#include <Core/Guid.h>
#include <Crypto/Bytes.h>
#include <Crypto/Labels.h>
#include <Crypto/Random.h>

#include "KeyAccess.h"
#include "MacInternal.h"
#include "PasswordInternal.h"

#include <Core/Move.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Memory.h>

namespace rocks::crypto {
namespace {

// ---- 배치(10.4절) ---- 0 magic 4 | 4 version 1 | 5 type 1 | 6 algorithm 1 | 7 method 1 | 8 keyId
// 16 | 24 L(LE16) 2 | 26 방식 매개변수 P | 26+P nonce 24 | 50+P commitment 32 | 82+P 암호문 ‖ tag
// (L + 16)
constexpr uint8 Magic[4] = {'R', 'K', 'E', 'Y'};
constexpr uint8 Version = 1;
constexpr usize VersionedPrefixBytes = 5; // magic ‖ version
constexpr usize FixedHeaderBytes = 26;
constexpr usize KeyParamsBytes = 16;      // wrapKeyId
constexpr usize PasswordParamsBytes = 24; // ops LE32 ‖ memKiB LE32 ‖ salt 16
constexpr usize NonceBytes = 24;
constexpr usize CommitmentBytes = 32;
constexpr usize TagBytes = 16;
constexpr usize OverheadBytes = FixedHeaderBytes + NonceBytes + CommitmentBytes + TagBytes;
constexpr usize MaxHeaderBytes = FixedHeaderBytes + PasswordParamsBytes; // Peek이 읽는 최대 길이
constexpr usize MaxSecretBytes = MaxHmacKeyBytes;
static_assert(KeyBlobMaxBytes == OverheadBytes + PasswordParamsBytes + MaxSecretBytes);
static_assert(PasswordSaltBytes == 16);

// 파생 id다. B = TryDeriveBoundKdfKey(M, 0, ...), K = TryDeriveAeadKey(B, 0, ...), C =
// TryDeriveSecret(B, 1, ...). K와 C는 타입 tag와 id가 모두 다르다.
constexpr uint64 BoundId = 0;
constexpr uint64 WrapKeyId = 0;
constexpr uint64 CommitmentId = 1;

usize ParamsBytes(KeyBlobWrapMethod method) noexcept {
    return method == KeyBlobWrapMethod::Key ? KeyParamsBytes : PasswordParamsBytes;
}

void StoreKeyId(uint8* out, KeyId id) noexcept {
    StoreBigEndian<uint64>(out, id.value.high);
    StoreBigEndian<uint64>(out + 8, id.value.low);
}

KeyId LoadKeyId(const uint8* in) noexcept {
    KeyId id;
    id.value.high = LoadBigEndian<uint64>(in);
    id.value.low = LoadBigEndian<uint64>(in + 8);
    return id;
}

// (type, algorithm)에 허용된 비밀 길이다. 표에 없으면 false다.
struct SecretRule {
    usize minBytes;
    usize maxBytes;
    // true면 minBytes·maxBytes 두 길이만 받는다(AES-CMAC 16·32, AES-192 없음).
    bool endsOnly;
};

bool Allows(const SecretRule& rule, usize bytes) noexcept {
    if (bytes < rule.minBytes || bytes > rule.maxBytes) {
        return false;
    }

    return !rule.endsOnly || bytes == rule.minBytes || bytes == rule.maxBytes;
}

bool TryGetSecretRule(KeyBlobType type, uint8 algorithm, SecretRule& rule) noexcept {
    switch (type) {
    case KeyBlobType::Aead: {
        auto info = TryGetAeadInfo(static_cast<AeadAlgorithm>(algorithm));
        if (!info) {
            return false;
        }

        rule = {info.Value().keyBytes, info.Value().keyBytes, false};
        return true;
    }
    case KeyBlobType::Mac:
        if (algorithm == static_cast<uint8>(MacAlgorithm::HmacSha256) ||
            algorithm == static_cast<uint8>(MacAlgorithm::HmacSha512)) {
            rule = {MinMacKeyBytes, MaxHmacKeyBytes, false};
            return true;
        }

        if (algorithm == static_cast<uint8>(MacAlgorithm::Blake2bKeyed)) {
            rule = {MinMacKeyBytes, 64, false};
            return true;
        }

        if (algorithm == static_cast<uint8>(MacAlgorithm::AesCmac)) {
            rule = {16, 32, true};
            return true;
        }

        return false;
    case KeyBlobType::Kdf:
    case KeyBlobType::Ed25519:
    case KeyBlobType::Kx:
    case KeyBlobType::Box:
    case KeyBlobType::SecretStream:
    case KeyBlobType::SegmentStream:
        rule = {32, 32, false};
        return algorithm == 0;
    case KeyBlobType::Kem: {
        auto info = TryGetKemInfo(static_cast<KemAlgorithm>(algorithm));
        if (!info) {
            return false;
        }

        rule = {info.Value().seedBytes, info.Value().seedBytes, false};
        return true;
    }
    }

    return false;
}

// secret이 (type, algorithm)에 허용된 길이인가.
bool IsAllowedSecret(KeyBlobType type, uint8 algorithm, Span<const uint8> secret) noexcept {
    SecretRule rule{};
    return TryGetSecretRule(type, algorithm, rule) && Allows(rule, secret.Size());
}

bool IsDefinedType(uint8 value) noexcept {
    return value >= static_cast<uint8>(KeyBlobType::Aead) &&
           value <= static_cast<uint8>(KeyBlobType::SegmentStream);
}

bool IsDefinedMethod(uint8 value) noexcept {
    return value == static_cast<uint8>(KeyBlobWrapMethod::Key) ||
           value == static_cast<uint8>(KeyBlobWrapMethod::Password);
}

// 헤더를 byte 순서대로 읽는다. 구조 위반은 AuthenticationFailed, 식별 실패는 Unsupported다. 버전은
// magic 다음 byte만으로 판정한다(모르는 버전은 길이와 관계없이 Unsupported, 뒤를 해석하지 않는다).
// blob은 호출자가 만든 지역 사본이다(한 번만 읽는다).
Result<KeyBlobInfo> TryParseHeader(Span<const uint8> blob) noexcept {
    if (blob.Size() < VersionedPrefixBytes ||
        !ConstantTimeEquals(blob.Left(4), Span<const uint8>(Magic))) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    const uint8* bytes = blob.Data();
    if (bytes[4] != Version) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::Unsupported);
    }

    if (blob.Size() < FixedHeaderBytes) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (!IsDefinedType(bytes[5])) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::Unsupported);
    }

    KeyBlobInfo info;
    info.type = static_cast<KeyBlobType>(bytes[5]);
    info.algorithm = bytes[6];
    SecretRule rule{};
    if (!TryGetSecretRule(info.type, info.algorithm, rule) || !IsDefinedMethod(bytes[7])) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::Unsupported);
    }

    info.method = static_cast<KeyBlobWrapMethod>(bytes[7]);
    info.keyId = LoadKeyId(bytes + 8);
    info.secretBytes = LoadLittleEndian<uint16>(bytes + 24);
    if (info.keyId.IsNil() || !Allows(rule, info.secretBytes)) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    const usize params = ParamsBytes(info.method);
    if (blob.Size() < FixedHeaderBytes + params) {
        return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (info.method == KeyBlobWrapMethod::Key) {
        info.wrapKeyId = LoadKeyId(bytes + FixedHeaderBytes);
        if (info.wrapKeyId.IsNil()) {
            return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
        }
    } else {
        const uint32 operations = LoadLittleEndian<uint32>(bytes + FixedHeaderBytes);
        const uint32 memoryKiB = LoadLittleEndian<uint32>(bytes + FixedHeaderBytes + 4);
        info.passwordCost.operations = operations;
        info.passwordCost.memoryBytes = static_cast<usize>(memoryKiB) * 1024;
        // 하한 위반은 blob에서 온 값의 형식 오류다(InvalidArgument로 새지 않게 여기서 거른다).
        if (operations < 1 || info.passwordCost.memoryBytes < MinPasswordMemoryBytes) {
            return Result<KeyBlobInfo>::Failure(ErrorCode::AuthenticationFailed);
        }
    }

    info.blobBytes = OverheadBytes + params + info.secretBytes;
    return Result<KeyBlobInfo>::Success(info);
}

// ---- 키 타입별 비밀·생성 ----

template <typename TKey>
struct TypeTag {};

uint8 AlgorithmOf(const AeadKey& key) noexcept {
    return static_cast<uint8>(key.Algorithm());
}

uint8 AlgorithmOf(const MacKey& key) noexcept {
    return static_cast<uint8>(key.Algorithm());
}

uint8 AlgorithmOf(const KemKeyPair& key) noexcept {
    return static_cast<uint8>(key.Algorithm());
}

template <typename TKey>
uint8 AlgorithmOf(const TKey&) noexcept {
    return 0;
}

template <typename TKey>
Span<const uint8> SecretOf(const TKey& key) noexcept {
    return detail::KeyAccess::Bytes(key);
}

// 서명 키의 libsodium 비밀키는 seed ‖ 공개키다. seed만 담는다(공개키는 seed에서 다시 계산한다).
Span<const uint8> SecretOf(const Ed25519SigningKey& key) noexcept {
    return detail::KeyAccess::Bytes(key).Left(Ed25519SeedBytes);
}

Result<AeadKey> TryMakeKey(TypeTag<AeadKey>, uint8 algorithm, KeyId,
                           Span<const uint8> secret) noexcept {
    return AeadKey::TryImport(static_cast<AeadAlgorithm>(algorithm), secret);
}

Result<MacKey> TryMakeKey(TypeTag<MacKey>, uint8 algorithm, KeyId,
                          Span<const uint8> secret) noexcept {
    return MacKey::TryImport(static_cast<MacAlgorithm>(algorithm), secret);
}

Result<KdfKey> TryMakeKey(TypeTag<KdfKey>, uint8, KeyId, Span<const uint8> secret) noexcept {
    return KdfKey::TryImport(secret);
}

Result<Ed25519SigningKey> TryMakeKey(TypeTag<Ed25519SigningKey>, uint8, KeyId,
                                     Span<const uint8> secret) noexcept {
    return Ed25519SigningKey::TryFromSeed(secret);
}

Result<KxKeyPair> TryMakeKey(TypeTag<KxKeyPair>, uint8, KeyId, Span<const uint8> secret) noexcept {
    return KxKeyPair::TryImport(secret);
}

Result<BoxKeyPair> TryMakeKey(TypeTag<BoxKeyPair>, uint8, KeyId,
                              Span<const uint8> secret) noexcept {
    return BoxKeyPair::TryImport(secret);
}

Result<KemKeyPair> TryMakeKey(TypeTag<KemKeyPair>, uint8 algorithm, KeyId,
                              Span<const uint8> secret) noexcept {
    return KemKeyPair::TryFromSeed(static_cast<KemAlgorithm>(algorithm), secret);
}

Result<SecretStreamKey> TryMakeKey(TypeTag<SecretStreamKey>, uint8, KeyId,
                                   Span<const uint8> secret) noexcept {
    return SecretStreamKey::TryImport(secret);
}

// 세그먼트 스트림 키는 Id()를 품으므로 blob의 keyId로 만든다(헤더 keyId와 어긋날 수 없다).
Result<SegmentStreamKey> TryMakeKey(TypeTag<SegmentStreamKey>, uint8, KeyId keyId,
                                    Span<const uint8> secret) noexcept {
    return SegmentStreamKey::TryImport(keyId, secret);
}

// key가 Id()를 품는 타입이면 keyId가 그 값과 같아야 한다(다르면 호출자 인자 오류).
template <typename TKey>
bool MatchesKeyId(const TKey&, KeyId) noexcept {
    return true;
}

bool MatchesKeyId(const SegmentStreamKey& key, KeyId keyId) noexcept {
    return key.Id() == keyId;
}

// ---- 파생 ----

// M과 헤더 transcript T(nonce까지)에서 wrap AEAD 키 K와 commitment C를 뽑는다.
struct WrapKeys {
    AeadKey key;
    SecretBytes commitment;
};

Result<WrapKeys> TryDeriveWrapKeys(const KdfKey& master, Span<const uint8> transcript) noexcept {
    const Span<const uint8> pieces[] = {transcript};
    auto bound = TryDeriveBoundKdfKey(master, BoundId, labels::KeyBlob,
                                      Span<const Span<const uint8>>(pieces));
    if (!bound) {
        return Result<WrapKeys>::Failure(bound.Error());
    }

    auto key = TryDeriveAeadKey(bound.Value(), WrapKeyId, labels::KeyBlob,
                                AeadAlgorithm::XChaCha20Poly1305);
    if (!key) {
        return Result<WrapKeys>::Failure(key.Error());
    }

    auto commitment =
        TryDeriveSecret(bound.Value(), CommitmentId, labels::KeyBlob, CommitmentBytes);
    if (!commitment) {
        return Result<WrapKeys>::Failure(commitment.Error());
    }

    return Result<WrapKeys>::Success(WrapKeys{Move(key.Value()), Move(commitment.Value())});
}

// ---- wrap·unwrap 공통 ----

// 방식별 입력이다. Key 방식은 wrapKey·wrapKeyId, Password 방식은 password·cost를 쓴다.
struct WrapInput {
    KeyBlobWrapMethod method;
    const KdfKey* wrapKey;
    KeyId wrapKeyId;
    Span<const uint8> password;
    PasswordCost cost;
};

Result<usize> TryWrapSecret(const WrapInput& input, KeyBlobType type, uint8 algorithm,
                            Span<const uint8> secret, KeyId keyId, bool keyIdMatches,
                            Span<uint8> blob) noexcept {
    if ((input.method == KeyBlobWrapMethod::Key && input.wrapKey->IsEmpty()) || secret.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (keyId.IsNil() || !keyIdMatches || !IsAllowedSecret(type, algorithm, secret)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    PasswordCost cost = input.cost;
    if (input.method == KeyBlobWrapMethod::Key) {
        if (input.wrapKeyId.IsNil()) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }
    } else {
        if (input.password.IsEmpty() || !detail::IsPasswordLengthValid(input.password) ||
            Overlaps(input.password, blob)) {
            return Result<usize>::Failure(ErrorCode::InvalidArgument);
        }

        if (auto valid = detail::CheckPasswordCost(cost); !valid) {
            return Result<usize>::Failure(valid.Error());
        }

        // 기록은 KiB 단위다. 계산도 기록한 값으로 해서 같은 비용에 인코딩이 하나만 있게 한다.
        cost.memoryBytes = cost.memoryBytes / 1024 * 1024;
    }

    const usize params = ParamsBytes(input.method);
    const usize total = OverheadBytes + params + secret.Size();
    if (blob.Size() < total) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    // 헤더를 쓴다. 여기부터 실패하면 blob을 지운다.
    const Span<uint8> out = blob.Left(total);
    uint8* bytes = out.Data();
    platform::MemoryCopy(bytes, Magic, sizeof(Magic));
    bytes[4] = Version;
    bytes[5] = static_cast<uint8>(type);
    bytes[6] = algorithm;
    bytes[7] = static_cast<uint8>(input.method);
    StoreKeyId(bytes + 8, keyId);
    StoreLittleEndian<uint16>(bytes + 24, static_cast<uint16>(secret.Size()));
    const auto fail = [&](ErrorCode error) {
        SecureZero(out);
        return Result<usize>::Failure(error);
    };

    const Span<uint8> nonce = out.Mid(FixedHeaderBytes + params, NonceBytes);
    if (auto filled = TryFillRandom(nonce); !filled) {
        return fail(filled.Error());
    }

    Result<KdfKey> passwordKey = Result<KdfKey>::Failure(ErrorCode::InvalidArgument);
    const KdfKey* master = input.wrapKey;
    if (input.method == KeyBlobWrapMethod::Key) {
        StoreKeyId(bytes + FixedHeaderBytes, input.wrapKeyId);
    } else {
        StoreLittleEndian<uint32>(bytes + FixedHeaderBytes, cost.operations);
        StoreLittleEndian<uint32>(bytes + FixedHeaderBytes + 4,
                                  static_cast<uint32>(cost.memoryBytes / 1024));
        const Span<uint8> salt = out.Mid(FixedHeaderBytes + 8, PasswordSaltBytes);
        if (auto filled = TryFillRandom(salt); !filled) {
            return fail(filled.Error());
        }

        passwordKey = TryDeriveKdfKeyFromPassword(input.password, salt, cost);
        if (!passwordKey) {
            return fail(passwordKey.Error());
        }

        master = &passwordKey.Value();
    }

    const Span<const uint8> transcript = out.Left(FixedHeaderBytes + params + NonceBytes);
    auto keys = TryDeriveWrapKeys(*master, transcript);
    if (!keys) {
        return fail(keys.Error());
    }

    platform::MemoryCopy(bytes + FixedHeaderBytes + params + NonceBytes,
                         keys.Value().commitment.View().Data(), CommitmentBytes);
    auto sealed = hazmat::TrySealWithNonce(
        keys.Value().key, nonce, transcript, secret,
        out.RightChop(FixedHeaderBytes + params + NonceBytes + CommitmentBytes));
    if (!sealed) {
        return fail(sealed.Error());
    }

    return Result<usize>::Success(total);
}

// blob 앞부분(최대 capacity byte)을 지역 버퍼에 한 번 복사한다. 검사·파생·복호는 이 사본만 보므로
// 호출 중 다른 주체가 원본(공유 메모리, 쓰기 가능한 매핑)을 바꿔도 한 시점의 byte로 판정한다.
Span<const uint8> CopyPrefix(Span<const uint8> blob, Span<uint8> buffer) noexcept {
    const usize bytes = blob.Size() < buffer.Size() ? blob.Size() : buffer.Size();
    if (bytes != 0) {
        platform::MemoryCopy(buffer.Data(), blob.Data(), bytes);
    }

    return Span<const uint8>(buffer.Data(), bytes);
}

template <typename TKey>
Result<UnwrappedKey<TKey>> TryUnwrapSecret(KeyBlobWrapMethod method, const KdfKey* wrapKey,
                                           Span<const uint8> password,
                                           Span<const uint8> source) noexcept {
    using Unwrapped = UnwrappedKey<TKey>;
    if (method == KeyBlobWrapMethod::Key && wrapKey->IsEmpty()) {
        return Result<Unwrapped>::Failure(ErrorCode::StaleHandle);
    }

    if (method == KeyBlobWrapMethod::Password && !detail::IsPasswordLengthValid(password)) {
        return Result<Unwrapped>::Failure(ErrorCode::InvalidArgument);
    }

    // 상한보다 긴 blob은 길이 비교에서 거부되므로 상한까지만 복사한다(헤더 판정은 그 전에 끝난다).
    uint8 buffer[KeyBlobMaxBytes];
    const Span<const uint8> blob = CopyPrefix(source, Span<uint8>(buffer));
    auto header = TryParseHeader(blob);
    if (!header) {
        return Result<Unwrapped>::Failure(header.Error());
    }

    const KeyBlobInfo& info = header.Value();
    // 길이·타입·방식은 blob에서 온 값이다. 기대와 다르면 AuthenticationFailed다.
    if (source.Size() != info.blobBytes || info.type != KeyBlobTraits<TKey>::Type ||
        info.method != method) {
        return Result<Unwrapped>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (method == KeyBlobWrapMethod::Password && detail::IsPasswordCostOverCap(info.passwordCost)) {
        return Result<Unwrapped>::Failure(ErrorCode::CapacityExceeded);
    }

    if (info.type == KeyBlobType::Aead &&
        !TryGetAeadInfo(static_cast<AeadAlgorithm>(info.algorithm)).Value().available) {
        return Result<Unwrapped>::Failure(ErrorCode::Unsupported);
    }

    // 이 기기에서 쓸 수 없는 MAC 알고리즘(AES HW가 없는 AES-CMAC)도 KDF 전에 거부한다.
    if (info.type == KeyBlobType::Mac &&
        !detail::CheckMacUsable(static_cast<MacAlgorithm>(info.algorithm))) {
        return Result<Unwrapped>::Failure(ErrorCode::Unsupported);
    }

    const usize params = ParamsBytes(method);
    Result<KdfKey> passwordKey = Result<KdfKey>::Failure(ErrorCode::InvalidArgument);
    const KdfKey* master = wrapKey;
    if (method == KeyBlobWrapMethod::Password) {
        passwordKey = TryDeriveKdfKeyFromPassword(
            password, blob.Mid(FixedHeaderBytes + 8, PasswordSaltBytes), info.passwordCost);
        if (!passwordKey) {
            return Result<Unwrapped>::Failure(passwordKey.Error());
        }

        master = &passwordKey.Value();
    }

    const Span<const uint8> transcript = blob.Left(FixedHeaderBytes + params + NonceBytes);
    auto keys = TryDeriveWrapKeys(*master, transcript);
    if (!keys) {
        return Result<Unwrapped>::Failure(keys.Error());
    }

    // 다른 KEK·비밀번호와 헤더 변조는 AEAD 전에 commitment에서 걸린다.
    if (!ConstantTimeEquals(keys.Value().commitment.View(),
                            blob.Mid(FixedHeaderBytes + params + NonceBytes, CommitmentBytes))) {
        return Result<Unwrapped>::Failure(ErrorCode::AuthenticationFailed);
    }

    auto secret = SecretBytes::TryCreate(info.secretBytes);
    if (!secret) {
        return Result<Unwrapped>::Failure(secret.Error());
    }

    auto opened = hazmat::TryOpenWithNonce(
        keys.Value().key, blob.Mid(FixedHeaderBytes + params, NonceBytes), transcript,
        blob.RightChop(FixedHeaderBytes + params + NonceBytes + CommitmentBytes),
        secret.Value().MutableView());
    if (!opened) {
        return Result<Unwrapped>::Failure(opened.Error());
    }

    auto key = TryMakeKey(TypeTag<TKey>{}, info.algorithm, info.keyId, secret.Value().View());
    if (!key) {
        return Result<Unwrapped>::Failure(key.Error());
    }

    return Result<Unwrapped>::Success(Unwrapped{info.keyId, Move(key.Value())});
}

} // namespace

Result<KeyId> TryNewKeyId() noexcept {
    auto guid = TryNewGuid();
    if (!guid) {
        return Result<KeyId>::Failure(ErrorCode::DependencyFailed);
    }

    return Result<KeyId>::Success(KeyId{guid.Value()});
}

Result<KeyBlobInfo> TryPeekKeyBlob(Span<const uint8> blob) noexcept {
    uint8 buffer[MaxHeaderBytes];
    return TryParseHeader(CopyPrefix(blob, Span<uint8>(buffer)));
}

template <KeyBlobKey TKey>
Result<usize> TryGetKeyBlobBytes(const TKey& key, KeyBlobWrapMethod method) noexcept {
    const Span<const uint8> secret = SecretOf(key);
    if (secret.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    // wrap과 같은 규칙이다(16 byte보다 짧은 MAC 키는 크기도 주지 않는다).
    if (!IsDefinedMethod(static_cast<uint8>(method)) ||
        !IsAllowedSecret(KeyBlobTraits<TKey>::Type, AlgorithmOf(key), secret)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(OverheadBytes + ParamsBytes(method) + secret.Size());
}

template <KeyBlobKey TKey>
Result<usize> TryWrapKey(const KdfKey& wrapKey, KeyId wrapKeyId, const TKey& key, KeyId keyId,
                         Span<uint8> blob) noexcept {
    platform::DataIndependentTimingScope dit;
    const WrapInput input{KeyBlobWrapMethod::Key, &wrapKey, wrapKeyId, Span<const uint8>(), {}};
    return TryWrapSecret(input, KeyBlobTraits<TKey>::Type, AlgorithmOf(key), SecretOf(key), keyId,
                         MatchesKeyId(key, keyId), blob);
}

template <KeyBlobKey TKey>
Result<usize> TryWrapKeyWithPassword(Span<const uint8> password, PasswordCost cost, const TKey& key,
                                     KeyId keyId, Span<uint8> blob) noexcept {
    platform::DataIndependentTimingScope dit;
    const WrapInput input{KeyBlobWrapMethod::Password, nullptr, KeyId(), password, cost};
    return TryWrapSecret(input, KeyBlobTraits<TKey>::Type, AlgorithmOf(key), SecretOf(key), keyId,
                         MatchesKeyId(key, keyId), blob);
}

template <KeyBlobKey TKey>
Result<UnwrappedKey<TKey>> TryUnwrapKey(const KdfKey& wrapKey, Span<const uint8> blob) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryUnwrapSecret<TKey>(KeyBlobWrapMethod::Key, &wrapKey, Span<const uint8>(), blob);
}

template <KeyBlobKey TKey>
Result<UnwrappedKey<TKey>> TryUnwrapKeyWithPassword(Span<const uint8> password,
                                                    Span<const uint8> blob) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryUnwrapSecret<TKey>(KeyBlobWrapMethod::Password, nullptr, password, blob);
}

#define ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(TKey)                                                     \
    template Result<usize> TryGetKeyBlobBytes<TKey>(const TKey&, KeyBlobWrapMethod) noexcept;      \
    template Result<usize> TryWrapKey<TKey>(const KdfKey&, KeyId, const TKey&, KeyId,              \
                                            Span<uint8>) noexcept;                                 \
    template Result<usize> TryWrapKeyWithPassword<TKey>(Span<const uint8>, PasswordCost,           \
                                                        const TKey&, KeyId, Span<uint8>) noexcept; \
    template Result<UnwrappedKey<TKey>> TryUnwrapKey<TKey>(const KdfKey&,                          \
                                                           Span<const uint8>) noexcept;            \
    template Result<UnwrappedKey<TKey>> TryUnwrapKeyWithPassword<TKey>(                            \
        Span<const uint8>, Span<const uint8>) noexcept;

ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(AeadKey)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(MacKey)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(KdfKey)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(Ed25519SigningKey)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(KxKeyPair)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(BoxKeyPair)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(KemKeyPair)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(SecretStreamKey)
ROCKS_CRYPTO_KEYBLOB_INSTANTIATE(SegmentStreamKey)

#undef ROCKS_CRYPTO_KEYBLOB_INSTANTIATE

} // namespace rocks::crypto
