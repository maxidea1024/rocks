#include <Crypto/Aead.h>

#include <Crypto/Bytes.h>
#include <Crypto/Crypto.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "KdfInternal.h"
#include "KeyAccess.h"
#include "SecretArena.h"
#include "Sodium/SodiumRuntime.h"

#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Core/Move.h>
#include <Platform/Cpu.h>
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

// libsodium AES-256-GCM이 실제로 받는 평문 상한이다. 헤더 상수 MESSAGEBYTES_MAX는 16 × (2^32 - 2)
// (NIST SP 800-38D의 2^36 - 32 byte)이지만 aesni·armcrypto 구현의 required_blocks()는 평문 블록 수
// 2^32 - 2를 거부한다. 상수를 그대로 쓰면 마지막 한 블록 길이에서 봉인이 -1을 돌려 Fatal이 된다.
constexpr uint64 MaxAes256GcmMessageBytes = 16 * ((uint64{1} << 32) - 3);
static_assert(MaxAes256GcmMessageBytes <= crypto_aead_aes256gcm_MESSAGEBYTES_MAX);

// GCM nonce는 96 bit만 받는다. 표준은 다른 길이도 허용하므로 그 밖은 Unsupported다(5.2·11절).
constexpr usize GcmNonceBytes = 12;

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
                crypto_aead_aes256gcm_ABYTES, MaxAes256GcmMessageBytes, false};
    case AeadAlgorithm::Aes128Gcm:
        return {16, GcmNonceBytes, 16, crypto_aead_aes256gcm_MESSAGEBYTES_MAX, false};
    case AeadAlgorithm::Aegis256:
        return {crypto_aead_aegis256_KEYBYTES, crypto_aead_aegis256_NPUBBYTES,
                crypto_aead_aegis256_ABYTES, crypto_aead_aegis256_MESSAGEBYTES_MAX, true};
    case AeadAlgorithm::Aegis128L:
        return {crypto_aead_aegis128l_KEYBYTES, crypto_aead_aegis128l_NPUBBYTES,
                crypto_aead_aegis128l_ABYTES, crypto_aead_aegis128l_MESSAGEBYTES_MAX, false};
    }

    return {};
}

// AES-256-GCM·AEGIS의 HW 조건이다. 두 알고리즘이 같은 조건을 쓴다(6.1절 "HW 조건은 하나다").
// - x86-64: AES-NI·PCLMULQDQ·AVX. ARM64: AES·PMULL. PlatformBase CpuFeatures와 libsodium 실행
//   감지가 모두 참이어야 한다(6.2절). libsodium은 ARM에서 AES bit 하나만 보므로 PMULL은
//   CpuFeatures로 본다. PMULL 없이 AES-256-GCM 키를 만들면 beforenm이 PMULL 명령에서 죽는다.
// - crypto_aead_aes256gcm_is_available()은 libsodium이 HW 구현으로 빌드되었을 때만 참이다.
//   ARM에서는 HAVE_ARMCRYPTO 빌드 정의를 뜻하고, 이는 AEGIS armcrypto 구현의 컴파일 조건과
//   같다(ARM의 실행 감지는 빌드 정의와 무관해서 따로 확인해야 한다). x86의 실행 감지 값은 빌드
//   정의가 있을 때만 켜진다.
// 조건이 아니면 libsodium soft 경로로 내려가지 않고 Unsupported다(결정 D3).
bool HasAesHardware() noexcept {
    const platform::CpuFeatures& cpu = platform::QueryCpuFeatures();
    const CryptoRuntimeInfo& info = GetCryptoRuntimeInfo();
    bool hardware = false;
    if constexpr (IsX64) {
        hardware = cpu.aes && cpu.pclmulqdq && cpu.avx && info.sodiumAesni && info.sodiumPclmul &&
                   info.sodiumAvx;
    } else {
        hardware = cpu.armAes && cpu.armPmull && info.sodiumArmCrypto;
    }

    return hardware && crypto_aead_aes256gcm_is_available() != 0;
}

StringView HardwareName() noexcept {
    return IsX64 ? StringView("libsodium/aesni") : StringView("libsodium/armcrypto");
}

bool IsAvailable(AeadAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case AeadAlgorithm::XChaCha20Poly1305:
    case AeadAlgorithm::ChaCha20Poly1305Ietf:
        return true;
    case AeadAlgorithm::Aes256Gcm:
    case AeadAlgorithm::Aegis256:
    case AeadAlgorithm::Aegis128L:
        return HasAesHardware();
    case AeadAlgorithm::Aes128Gcm:
        return false;
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

// hazmat 공통 검사다. 빈 키는 StaleHandle, GCM의 96 bit가 아닌 nonce는 Unsupported, 그 밖의 nonce
// 길이 오류는 InvalidArgument다.
Result<void> CheckHazmat(const AeadKey& key, Span<const uint8> nonce) noexcept {
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize expected = TraitsOf(key.Algorithm()).nonceBytes;
    if (nonce.Size() != expected) {
        const bool gcm = key.Algorithm() == AeadAlgorithm::Aes256Gcm ||
                         key.Algorithm() == AeadAlgorithm::Aes128Gcm;
        return Result<void>::Failure(gcm ? ErrorCode::Unsupported : ErrorCode::InvalidArgument);
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

// ---- 순번 봉인기 ----

namespace {

constexpr usize SequenceCounterBytes = 8;
constexpr usize MaxSequencePrefixBytes = 24;
// 마지막으로 쓸 수 있는 카운터다. 이 값으로 봉인·열기한 뒤에는 소진이다.
constexpr uint64 LastSequence = static_cast<uint64>(-1);

static_assert(crypto_aead_aegis256_NPUBBYTES - SequenceCounterBytes == MaxSequencePrefixBytes);

// 빈 키는 StaleHandle, prefix 길이(nonce 길이 - 8)가 틀리면 InvalidArgument다.
Result<void> CheckSequenceArguments(const AeadKey& key, Span<const uint8> prefix) noexcept {
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (prefix.Size() != TraitsOf(key.Algorithm()).nonceBytes - SequenceCounterBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// nonce = prefix ‖ BE64(sequence)를 nonce에 쓰고 그 view를 돌려준다.
Span<const uint8> MakeSequenceNonce(const AeadKey& key, const uint8* prefix, uint64 sequence,
                                    Span<uint8> nonce) noexcept {
    const usize prefixBytes = TraitsOf(key.Algorithm()).nonceBytes - SequenceCounterBytes;
    platform::MemoryCopy(nonce.Data(), prefix, prefixBytes);
    StoreBigEndian<uint64>(nonce.Data() + prefixBytes, sequence);
    return nonce.Left(prefixBytes + SequenceCounterBytes);
}

} // namespace

hazmat::AeadSequenceSealer::AeadSequenceSealer(AeadKey&& key, Span<const uint8> prefix,
                                               uint64 firstSequence) noexcept
    : key_(Move(key)), next_(firstSequence) {
    platform::MemoryCopy(prefix_, prefix.Data(), prefix.Size());
}

Result<hazmat::AeadSequenceSealer>
hazmat::AeadSequenceSealer::TryCreate(AeadKey&& key, Span<const uint8> prefix,
                                      uint64 firstSequence) noexcept {
    if (auto valid = CheckSequenceArguments(key, prefix); !valid) {
        return Result<AeadSequenceSealer>::Failure(valid.Error());
    }

    return Result<AeadSequenceSealer>::Success(
        AeadSequenceSealer(Move(key), prefix, firstSequence));
}

Result<usize> hazmat::AeadSequenceSealer::TrySeal(Span<const uint8> associatedData,
                                                  Span<const uint8> plaintext,
                                                  Span<uint8> sealed) noexcept {
    if (key_.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    if (exhausted_) {
        return Result<usize>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 nonceBytes[MaxSequencePrefixBytes + SequenceCounterBytes];
    const Span<const uint8> nonce =
        MakeSequenceNonce(key_, prefix_, next_, Span<uint8>(nonceBytes));
    auto written = TrySealWithNonce(key_, nonce, associatedData, plaintext, sealed);
    if (written) {
        // 실패한 호출은 쓰기 전에 돌아오므로 카운터를 쓰지 않은 것으로 본다.
        if (next_ == LastSequence) {
            exhausted_ = true;
        } else {
            ++next_;
        }
    }

    return written;
}

hazmat::AeadSequenceOpener::AeadSequenceOpener(AeadKey&& key, Span<const uint8> prefix,
                                               uint64 firstSequence) noexcept
    : key_(Move(key)), next_(firstSequence) {
    platform::MemoryCopy(prefix_, prefix.Data(), prefix.Size());
}

Result<hazmat::AeadSequenceOpener>
hazmat::AeadSequenceOpener::TryCreate(AeadKey&& key, Span<const uint8> prefix,
                                      uint64 firstSequence) noexcept {
    if (auto valid = CheckSequenceArguments(key, prefix); !valid) {
        return Result<AeadSequenceOpener>::Failure(valid.Error());
    }

    return Result<AeadSequenceOpener>::Success(
        AeadSequenceOpener(Move(key), prefix, firstSequence));
}

Result<usize> hazmat::AeadSequenceOpener::TryOpen(Span<const uint8> associatedData,
                                                  Span<const uint8> sealed,
                                                  Span<uint8> plaintext) noexcept {
    if (key_.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::StaleHandle);
    }

    // 인증 실패 뒤와 카운터를 다 쓴 뒤의 메시지는 받아들이지 않는다(sticky).
    if (failed_ || exhausted_) {
        failed_ = true;
        SecureZero(plaintext);
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    uint8 nonceBytes[MaxSequencePrefixBytes + SequenceCounterBytes];
    const Span<const uint8> nonce =
        MakeSequenceNonce(key_, prefix_, next_, Span<uint8>(nonceBytes));
    auto opened = TryOpenWithNonce(key_, nonce, associatedData, sealed, plaintext);
    if (opened) {
        if (next_ == LastSequence) {
            exhausted_ = true;
        } else {
            ++next_;
        }
    } else if (opened.Error() == ErrorCode::AuthenticationFailed) {
        failed_ = true;
    }

    return opened;
}

} // namespace rocks::crypto
