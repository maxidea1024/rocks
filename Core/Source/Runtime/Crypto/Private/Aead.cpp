#include <Crypto/Aead.h>

#include <Crypto/Bytes.h>
#include <Crypto/Crypto.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "KdfInternal.h"
#include "KeyAccess.h"
#include "SecretArena.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

struct Traits {
    usize keyBytes;
    usize nonceBytes;
    usize tagBytes;
    uint64 maxMessageBytes;
    bool randomNonceSafe;
};

static_assert(alignof(crypto_aead_aes256gcm_state) <= 64);
static_assert(sizeof(crypto_aead_aes256gcm_state) <= detail::SecretSlotMaxBytes);

bool IsDefined(AeadAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case AeadAlgorithm::XChaCha20Poly1305:
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
    case AeadAlgorithm::Aes256Gcm:
    case AeadAlgorithm::Aes128Gcm:
    case AeadAlgorithm::Aegis256:
    case AeadAlgorithm::Aegis128L:
        return true;
    }

    return false;
}

// 정의된 알고리즘만 넘긴다(IsDefined 뒤). libsodium의 MESSAGEBYTES_MAX는 모두 SIZE_MAX - ABYTES
// 이하라서 평문 + tag는 usize를 넘지 않는다.
Traits TraitsOf(AeadAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case AeadAlgorithm::XChaCha20Poly1305:
        return {crypto_aead_xchacha20poly1305_ietf_KEYBYTES,
                crypto_aead_xchacha20poly1305_ietf_NPUBBYTES,
                crypto_aead_xchacha20poly1305_ietf_ABYTES,
                crypto_aead_xchacha20poly1305_ietf_MESSAGEBYTES_MAX, true};
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
        return {crypto_aead_chacha20poly1305_ietf_KEYBYTES,
                crypto_aead_chacha20poly1305_ietf_NPUBBYTES,
                crypto_aead_chacha20poly1305_ietf_ABYTES,
                crypto_aead_chacha20poly1305_ietf_MESSAGEBYTES_MAX, false};
    case AeadAlgorithm::Aes256Gcm:
        return {crypto_aead_aes256gcm_KEYBYTES, crypto_aead_aes256gcm_NPUBBYTES,
                crypto_aead_aes256gcm_ABYTES, crypto_aead_aes256gcm_MESSAGEBYTES_MAX, false};
    case AeadAlgorithm::Aes128Gcm:
        return {16, 12, 16, crypto_aead_aes256gcm_MESSAGEBYTES_MAX, false};
    case AeadAlgorithm::Aegis256:
        return {crypto_aead_aegis256_KEYBYTES, crypto_aead_aegis256_NPUBBYTES,
                crypto_aead_aegis256_ABYTES, crypto_aead_aegis256_MESSAGEBYTES_MAX, true};
    case AeadAlgorithm::Aegis128L:
        return {crypto_aead_aegis128l_KEYBYTES, crypto_aead_aegis128l_NPUBBYTES,
                crypto_aead_aegis128l_ABYTES, crypto_aead_aegis128l_MESSAGEBYTES_MAX, false};
    }

    return {};
}

// AEGIS가 libsodium HW 경로를 타는가. libsodium의 구현 선택(_pick_best_implementation)과 같은
// 조건이다: ARM은 crypto 확장, x86은 AES-NI와 AVX. 컴파일 조건(HAVE_ARMCRYPTO·HAVE_AVXINTRIN_H·
// HAVE_WMMINTRIN_H)은 모든 대상 플랫폼에서 켜져 있다(ThirdParty.json·Windows는 common.h). 이 조건이
// 아니면 libsodium은 soft 구현으로 내려가므로 쓰지 않는다(결정 D3).
bool HasAesHardware() noexcept {
    const CryptoRuntimeInfo& info = GetCryptoRuntimeInfo();
    return info.sodiumArmCrypto || (info.sodiumAesni && info.sodiumAvx);
}

StringView HardwareName() noexcept {
    return GetCryptoRuntimeInfo().sodiumArmCrypto ? StringView("libsodium/armcrypto")
                                                  : StringView("libsodium/aesni");
}

bool IsAvailable(AeadAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case AeadAlgorithm::XChaCha20Poly1305:
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
        return true;
    case AeadAlgorithm::Aes256Gcm:
        // libsodium은 HW 구현만 있다(x86 AES-NI·PCLMUL·AVX, ARM crypto 확장).
        return crypto_aead_aes256gcm_is_available() != 0;
    case AeadAlgorithm::Aes128Gcm:
        return false;
    case AeadAlgorithm::Aegis256:
    case AeadAlgorithm::Aegis128L:
        return HasAesHardware();
    }

    return false;
}

// 빈 Span의 Data()는 null일 수 있다. libsodium 일부 경로는 길이 0이어도 포인터를 쓰므로 유효한
// 주소를 준다.
const uint8* InputPointer(Span<const uint8> bytes) noexcept {
    static const uint8 empty = 0;
    return bytes.IsEmpty() ? &empty : bytes.Data();
}

uint8* OutputPointer(Span<uint8> bytes, uint8* scratch) noexcept {
    return bytes.IsEmpty() ? scratch : bytes.Data();
}

// first가 second의 offset byte 뒤에서 시작하는가. 범위를 벗어난 포인터 산술을 피하려고 주소로
// 비교한다.
bool StartsAt(const void* first, const void* second, usize offset) noexcept {
    return reinterpret_cast<uptr>(first) == reinterpret_cast<uptr>(second) + offset;
}

// 키·nonce·평문 길이와 겹침은 호출 전에 검사한다. libsodium 실패는 어댑터 결함이다.
void SealDetachedRaw(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                     Span<const uint8> plaintext, Span<uint8> ciphertext,
                     Span<uint8> tag) noexcept {
    uint8 scratch = 0;
    const Span<const uint8> k = detail::KeyAccess::Bytes(key);
    const uint8* m = InputPointer(plaintext);
    const usize mlen = plaintext.Size();
    uint8* c = OutputPointer(ciphertext, &scratch);
    const uint8* ad = InputPointer(associatedData);
    const usize adlen = associatedData.Size();
    switch (key.Algorithm()) {
    case AeadAlgorithm::XChaCha20Poly1305:
        detail::RequireSodium(
            crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
                c, tag.Data(), nullptr, m, mlen, ad, adlen, nullptr, nonce.Data(), k.Data()),
            "Crypto: crypto_aead_xchacha20poly1305_ietf_encrypt_detached failed");
        return;
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
        detail::RequireSodium(
            crypto_aead_chacha20poly1305_ietf_encrypt_detached(
                c, tag.Data(), nullptr, m, mlen, ad, adlen, nullptr, nonce.Data(), k.Data()),
            "Crypto: crypto_aead_chacha20poly1305_ietf_encrypt_detached failed");
        return;
    case AeadAlgorithm::Aes256Gcm:
        detail::RequireSodium(
            crypto_aead_aes256gcm_encrypt_detached_afternm(
                c, tag.Data(), nullptr, m, mlen, ad, adlen, nullptr, nonce.Data(),
                static_cast<const crypto_aead_aes256gcm_state*>(detail::KeyAccess::State(key))),
            "Crypto: crypto_aead_aes256gcm_encrypt_detached_afternm failed");
        return;
    case AeadAlgorithm::Aegis256:
        detail::RequireSodium(crypto_aead_aegis256_encrypt_detached(c, tag.Data(), nullptr, m, mlen,
                                                                    ad, adlen, nullptr,
                                                                    nonce.Data(), k.Data()),
                              "Crypto: crypto_aead_aegis256_encrypt_detached failed");
        return;
    case AeadAlgorithm::Aegis128L:
        detail::RequireSodium(crypto_aead_aegis128l_encrypt_detached(c, tag.Data(), nullptr, m,
                                                                     mlen, ad, adlen, nullptr,
                                                                     nonce.Data(), k.Data()),
                              "Crypto: crypto_aead_aegis128l_encrypt_detached failed");
        return;
    case AeadAlgorithm::Aes128Gcm:
        break;
    }

    platform::Fatal("Crypto: AEAD seal reached an unavailable algorithm");
}

// tag가 맞으면 true다. 실패하면 평문 출력은 호출자가 지운다.
bool OpenDetachedRaw(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                     Span<const uint8> ciphertext, Span<const uint8> tag,
                     Span<uint8> plaintext) noexcept {
    uint8 scratch = 0;
    const Span<const uint8> k = detail::KeyAccess::Bytes(key);
    const uint8* c = InputPointer(ciphertext);
    const usize clen = ciphertext.Size();
    uint8* m = OutputPointer(plaintext, &scratch);
    const uint8* ad = InputPointer(associatedData);
    const usize adlen = associatedData.Size();
    switch (key.Algorithm()) {
    case AeadAlgorithm::XChaCha20Poly1305:
        return crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
                   m, nullptr, c, clen, tag.Data(), ad, adlen, nonce.Data(), k.Data()) == 0;
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
        return crypto_aead_chacha20poly1305_ietf_decrypt_detached(
                   m, nullptr, c, clen, tag.Data(), ad, adlen, nonce.Data(), k.Data()) == 0;
    case AeadAlgorithm::Aes256Gcm:
        return crypto_aead_aes256gcm_decrypt_detached_afternm(
                   m, nullptr, c, clen, tag.Data(), ad, adlen, nonce.Data(),
                   static_cast<const crypto_aead_aes256gcm_state*>(
                       detail::KeyAccess::State(key))) == 0;
    case AeadAlgorithm::Aegis256:
        return crypto_aead_aegis256_decrypt_detached(m, nullptr, c, clen, tag.Data(), ad, adlen,
                                                     nonce.Data(), k.Data()) == 0;
    case AeadAlgorithm::Aegis128L:
        return crypto_aead_aegis128l_decrypt_detached(m, nullptr, c, clen, tag.Data(), ad, adlen,
                                                      nonce.Data(), k.Data()) == 0;
    case AeadAlgorithm::Aes128Gcm:
        break;
    }

    platform::Fatal("Crypto: AEAD open reached an unavailable algorithm");
}

// 정의·사용 가능 여부를 검사한다. 정의되지 않으면 InvalidArgument, 이 기기에서 쓸 수 없으면
// Unsupported다.
Result<void> CheckUsable(AeadAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    if (!IsAvailable(algorithm)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

// 키 저장소 상태(AES-256-GCM 키 확장)를 만들어 키를 완성한다. 알고리즘·key 길이는 호출자가
// 검사했다.
Result<AeadKey> TryMakeKey(AeadAlgorithm algorithm, SecretBytes&& key) noexcept {
    void* state = nullptr;
    if (algorithm == AeadAlgorithm::Aes256Gcm) {
        auto slot = detail::TryAllocateSecretSlot(sizeof(crypto_aead_aes256gcm_state),
                                                  alignof(crypto_aead_aes256gcm_state));
        if (!slot) {
            return Result<AeadKey>::Failure(slot.Error());
        }

        state = slot.Value();
        detail::RequireSodium(
            crypto_aead_aes256gcm_beforenm(static_cast<crypto_aead_aes256gcm_state*>(state),
                                           key.View().Data()),
            "Crypto: crypto_aead_aes256gcm_beforenm failed");
    }

    return Result<AeadKey>::Success(detail::KeyAccess::Make<AeadKey>(algorithm, Move(key), state));
}

// hazmat 공통 검사다. 빈 키는 StaleHandle, nonce 길이가 틀리면 InvalidArgument다.
Result<void> CheckHazmat(const AeadKey& key, Span<const uint8> nonce) noexcept {
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (nonce.Size() != TraitsOf(key.Algorithm()).nonceBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace

// ---- 정보 ----

Result<AeadInfo> TryGetAeadInfo(AeadAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<AeadInfo>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    const Traits traits = TraitsOf(algorithm);
    AeadInfo info;
    info.keyBytes = traits.keyBytes;
    info.nonceBytes = traits.nonceBytes;
    info.tagBytes = traits.tagBytes;
    info.maxMessageBytes = traits.maxMessageBytes;
    info.randomNonceSafe = traits.randomNonceSafe;
    info.available = IsAvailable(algorithm);
    if (info.available) {
        const bool hardware = algorithm != AeadAlgorithm::XChaCha20Poly1305 &&
                              algorithm != AeadAlgorithm::ChaCha20Poly1305Ietf;
        info.implementation = hardware ? HardwareName() : StringView("libsodium");
    }

    return Result<AeadInfo>::Success(info);
}

AeadAlgorithm GetPreferredAead() noexcept {
    return AeadAlgorithm::XChaCha20Poly1305;
}

Result<usize> TryGetSealedBytes(AeadAlgorithm algorithm, usize plaintextBytes) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const Traits traits = TraitsOf(algorithm);
    const usize overhead = traits.nonceBytes + traits.tagBytes;
    if (plaintextBytes > MaxSize - overhead) {
        return Result<usize>::Failure(ErrorCode::Overflow);
    }

    return Result<usize>::Success(plaintextBytes + overhead);
}

// ---- AeadKey ----

AeadKey::~AeadKey() noexcept {
    Release();
}

AeadKey::AeadKey(AeadKey&& other) noexcept
    : algorithm_(other.algorithm_), key_(Move(other.key_)), state_(other.state_) {
    other.state_ = nullptr;
}

AeadKey& AeadKey::operator=(AeadKey&& other) noexcept {
    if (this != &other) {
        Release();
        algorithm_ = other.algorithm_;
        key_ = Move(other.key_);
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

void AeadKey::Release() noexcept {
    if (state_ != nullptr) {
        // 키 저장소 슬롯은 해제 때 지워진다(SecretArena).
        detail::FreeSecretSlot(state_);
        state_ = nullptr;
    }

    key_.Reset();
}

Result<AeadKey> AeadKey::TryGenerate(AeadAlgorithm algorithm) noexcept {
    if (auto usable = CheckUsable(algorithm); !usable) {
        return Result<AeadKey>::Failure(usable.Error());
    }

    auto key = SecretBytes::TryGenerate(TraitsOf(algorithm).keyBytes);
    if (!key) {
        return Result<AeadKey>::Failure(key.Error());
    }

    return TryMakeKey(algorithm, Move(key.Value()));
}

Result<AeadKey> AeadKey::TryImport(AeadAlgorithm algorithm, Span<const uint8> keyBytes) noexcept {
    if (auto usable = CheckUsable(algorithm); !usable) {
        return Result<AeadKey>::Failure(usable.Error());
    }

    if (keyBytes.Size() != TraitsOf(algorithm).keyBytes) {
        return Result<AeadKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryCopyFrom(keyBytes);
    if (!key) {
        return Result<AeadKey>::Failure(key.Error());
    }

    return TryMakeKey(algorithm, Move(key.Value()));
}

Result<AeadKey> TryDeriveAeadKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                 AeadAlgorithm algorithm) noexcept {
    if (auto usable = CheckUsable(algorithm); !usable) {
        return Result<AeadKey>::Failure(usable.Error());
    }

    auto key = detail::TryDeriveTypedSecret(master, subkeyId, context,
                                            detail::AeadKeyTypeTag(static_cast<uint8>(algorithm)),
                                            TraitsOf(algorithm).keyBytes);
    if (!key) {
        return Result<AeadKey>::Failure(key.Error());
    }

    return TryMakeKey(algorithm, Move(key.Value()));
}

// ---- 고수준 ----

Result<usize> TrySeal(const AeadKey& key, Span<const uint8> associatedData,
                      Span<const uint8> plaintext, Span<uint8> output) noexcept {
    if (key.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    const Traits traits = TraitsOf(key.Algorithm());
    if (!traits.randomNonceSafe) {
        return Result<usize>::Failure(ErrorCode::Unsupported);
    }

    if (plaintext.Size() > traits.maxMessageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    auto sealedBytes = TryGetSealedBytes(key.Algorithm(), plaintext.Size());
    if (!sealedBytes) {
        return sealedBytes;
    }

    if (output.Size() < sealedBytes.Value()) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const Span<uint8> sealed = output.Left(sealedBytes.Value());
    const bool inPlace = StartsAt(plaintext.Data(), sealed.Data(), traits.nonceBytes);
    if ((Overlaps(plaintext, sealed) && !inPlace) || Overlaps(associatedData, sealed)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const Span<uint8> nonce = sealed.Left(traits.nonceBytes);
    if (auto filled = TryFillRandom(nonce); !filled) {
        return Result<usize>::Failure(filled.Error());
    }

    SealDetachedRaw(key, nonce, associatedData, plaintext,
                    sealed.Mid(traits.nonceBytes, plaintext.Size()), sealed.Right(traits.tagBytes));
    return sealedBytes;
}

Result<usize> TryOpen(const AeadKey& key, Span<const uint8> associatedData,
                      Span<const uint8> sealed, Span<uint8> plaintext) noexcept {
    if (key.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    const Traits traits = TraitsOf(key.Algorithm());
    if (!traits.randomNonceSafe) {
        return Result<usize>::Failure(ErrorCode::Unsupported);
    }

    // 겹침은 길이 검사 전에 본다. 실패 때 plaintext를 지우므로 입력을 덮지 않게 한다.
    const bool inPlace = StartsAt(plaintext.Data(), sealed.Data(), traits.nonceBytes);
    if ((Overlaps(sealed, plaintext) && !inPlace) || Overlaps(associatedData, plaintext)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const usize overhead = traits.nonceBytes + traits.tagBytes;
    if (sealed.Size() < overhead || sealed.Size() - overhead > traits.maxMessageBytes) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    const usize messageBytes = sealed.Size() - overhead;
    if (plaintext.Size() < messageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (!OpenDetachedRaw(key, sealed.Left(traits.nonceBytes), associatedData,
                         sealed.Mid(traits.nonceBytes, messageBytes), sealed.Right(traits.tagBytes),
                         plaintext.Left(messageBytes))) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(messageBytes);
}

// ---- hazmat ----

Result<void> hazmat::TryExportKey(const AeadKey& key, Span<uint8> output) noexcept {
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != bytes.Size() || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), bytes.Data(), bytes.Size());
    return Result<void>::Success();
}

Result<usize> hazmat::TrySealWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                       Span<const uint8> associatedData,
                                       Span<const uint8> plaintext, Span<uint8> sealed) noexcept {
    if (auto valid = CheckHazmat(key, nonce); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    const Traits traits = TraitsOf(key.Algorithm());
    if (plaintext.Size() > traits.maxMessageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize sealedBytes = plaintext.Size() + traits.tagBytes;
    if (sealed.Size() < sealedBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    const Span<uint8> output = sealed.Left(sealedBytes);
    const bool inPlace = plaintext.Data() == output.Data();
    if ((Overlaps(plaintext, output) && !inPlace) || Overlaps(nonce, output) ||
        Overlaps(associatedData, output)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    SealDetachedRaw(key, nonce, associatedData, plaintext, output.Left(plaintext.Size()),
                    output.Right(traits.tagBytes));
    return Result<usize>::Success(sealedBytes);
}

Result<usize> hazmat::TryOpenWithNonce(const AeadKey& key, Span<const uint8> nonce,
                                       Span<const uint8> associatedData, Span<const uint8> sealed,
                                       Span<uint8> plaintext) noexcept {
    if (auto valid = CheckHazmat(key, nonce); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    const bool inPlace = plaintext.Data() == sealed.Data();
    if ((Overlaps(sealed, plaintext) && !inPlace) || Overlaps(nonce, plaintext) ||
        Overlaps(associatedData, plaintext)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    const Traits traits = TraitsOf(key.Algorithm());
    if (sealed.Size() < traits.tagBytes ||
        sealed.Size() - traits.tagBytes > traits.maxMessageBytes) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    const usize messageBytes = sealed.Size() - traits.tagBytes;
    if (plaintext.Size() < messageBytes) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    if (!OpenDetachedRaw(key, nonce, associatedData, sealed.Left(messageBytes),
                         sealed.Right(traits.tagBytes), plaintext.Left(messageBytes))) {
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(messageBytes);
}

Result<void> hazmat::TrySealDetached(const AeadKey& key, Span<const uint8> nonce,
                                     Span<const uint8> associatedData, Span<const uint8> plaintext,
                                     Span<uint8> ciphertext, Span<uint8> tag) noexcept {
    if (auto valid = CheckHazmat(key, nonce); !valid) {
        return valid;
    }

    const Traits traits = TraitsOf(key.Algorithm());
    if (plaintext.Size() > traits.maxMessageBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (ciphertext.Size() != plaintext.Size() || tag.Size() != traits.tagBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool inPlace = plaintext.Data() == ciphertext.Data();
    if ((Overlaps(plaintext, ciphertext) && !inPlace) || Overlaps(plaintext, tag) ||
        Overlaps(ciphertext, tag) || Overlaps(nonce, ciphertext) || Overlaps(nonce, tag) ||
        Overlaps(associatedData, ciphertext) || Overlaps(associatedData, tag)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    SealDetachedRaw(key, nonce, associatedData, plaintext, ciphertext, tag);
    return Result<void>::Success();
}

Result<void> hazmat::TryOpenDetached(const AeadKey& key, Span<const uint8> nonce,
                                     Span<const uint8> associatedData, Span<const uint8> ciphertext,
                                     Span<const uint8> tag, Span<uint8> plaintext) noexcept {
    if (auto valid = CheckHazmat(key, nonce); !valid) {
        return valid;
    }

    if (plaintext.Size() != ciphertext.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const bool inPlace = plaintext.Data() == ciphertext.Data();
    if ((Overlaps(ciphertext, plaintext) && !inPlace) || Overlaps(tag, plaintext) ||
        Overlaps(nonce, plaintext) || Overlaps(associatedData, plaintext)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // tag와 암호문 길이는 메시지에서 온 값이다. tag 길이가 틀리면 잘린 tag로 보고
    // AuthenticationFailed다(결정 D5).
    const Traits traits = TraitsOf(key.Algorithm());
    if (tag.Size() != traits.tagBytes || ciphertext.Size() > traits.maxMessageBytes) {
        SecureZero(plaintext);
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    if (!OpenDetachedRaw(key, nonce, associatedData, ciphertext, tag, plaintext)) {
        SecureZero(plaintext);
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<void>::Success();
}

} // namespace rocks::crypto
