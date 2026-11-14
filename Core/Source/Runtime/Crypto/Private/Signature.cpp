#include <Crypto/Signature.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "AdapterCommon.h"
#include "KeyAccess.h"
#include "Sodium/SodiumRuntime.h"

#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

static_assert(Ed25519PublicKeyBytes == crypto_sign_ed25519_PUBLICKEYBYTES);
static_assert(Ed25519SignatureBytes == crypto_sign_ed25519_BYTES);
static_assert(Ed25519SeedBytes == crypto_sign_ed25519_SEEDBYTES);

// libsodium 비밀키는 seed ‖ 공개키다.
constexpr usize SecretKeyBytes = crypto_sign_ed25519_SECRETKEYBYTES;
static_assert(SecretKeyBytes == Ed25519SeedBytes + Ed25519PublicKeyBytes);

constexpr usize PhStateBytes = sizeof(crypto_sign_ed25519ph_state);
constexpr usize PhStateAlignment = 64;
static_assert(alignof(crypto_sign_ed25519ph_state) <= PhStateAlignment);

Result<Ed25519SigningKey> TryMakeFromSeed(Span<const uint8> seed) noexcept {
    auto secret = SecretBytes::TryCreate(SecretKeyBytes);
    if (!secret) {
        return Result<Ed25519SigningKey>::Failure(secret.Error());
    }

    uint8 publicKey[Ed25519PublicKeyBytes];
    detail::RequireSodium(crypto_sign_ed25519_seed_keypair(
                              publicKey, secret.Value().MutableView().Data(), seed.Data()),
                          "Crypto: crypto_sign_ed25519_seed_keypair failed");
    return Result<Ed25519SigningKey>::Success(
        detail::KeyAccess::Make<Ed25519SigningKey>(Move(secret.Value())));
}

// ---- Ed25519ph 상태(서명·검증 공용) ----

crypto_sign_ed25519ph_state* AsPhState(void* state) noexcept {
    return static_cast<crypto_sign_ed25519ph_state*>(state);
}

void InitPhState(void* state) noexcept {
    detail::RequireSodium(crypto_sign_ed25519ph_init(AsPhState(state)),
                          "Crypto: crypto_sign_ed25519ph_init failed");
}

void UpdatePhState(void* state, Span<const uint8> input) noexcept {
    if (input.IsEmpty()) {
        return;
    }

    detail::RequireSodium(
        crypto_sign_ed25519ph_update(AsPhState(state), input.Data(), input.Size()),
        "Crypto: crypto_sign_ed25519ph_update failed");
}

void WipePhState(void* state) noexcept {
    SecureZero(Span<uint8>(static_cast<uint8*>(state), PhStateBytes));
}

Result<void*> TryAllocatePhState(Allocator& allocator) noexcept {
    detail::EnsureSodium();
    auto memory = allocator.Allocate(PhStateBytes, PhStateAlignment);
    if (!memory) {
        return Result<void*>::Failure(memory.Error());
    }

    InitPhState(memory.Value());
    return Result<void*>::Success(memory.Value());
}

// 할당한 상태를 지우고 돌려준다. 해제 실패는 상태가 그 allocator 것이 아니라는 결함이다.
void ReleasePhState(Allocator* allocator, void* state) noexcept {
    WipePhState(state);
    if (!allocator->Deallocate(state)) {
        platform::Fatal("Crypto: Ed25519ph state was not owned by its allocator");
    }
}

// 조각 입력 공통 검사다. 이동된 객체는 StaleHandle, 끝난 상태는 InvalidArgument다.
Result<void> CheckPhUpdate(const void* state, bool finished) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

// 검증기 입력 검사다. 실패한 뒤에는 AuthenticationFailed로 남는다(sticky, 9.1절).
Result<void> CheckPhVerifier(const void* state, bool finished, bool verifyFailed) noexcept {
    if (state != nullptr && verifyFailed) {
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    return CheckPhUpdate(state, finished);
}

} // namespace

// ---- 공개키·서명 키 ----

Result<Ed25519PublicKey> Ed25519PublicKey::TryFromBytes(Span<const uint8> bytes) noexcept {
    if (bytes.Size() != Ed25519PublicKeyBytes) {
        return Result<Ed25519PublicKey>::Failure(ErrorCode::InvalidArgument);
    }

    Ed25519PublicKey key;
    platform::MemoryCopy(key.bytes, bytes.Data(), Ed25519PublicKeyBytes);
    return Result<Ed25519PublicKey>::Success(key);
}

Ed25519SigningKey::Ed25519SigningKey(SecretBytes&& key) noexcept : key_(Move(key)) {
}

Result<Ed25519SigningKey> Ed25519SigningKey::TryGenerate() noexcept {
    uint8 seed[Ed25519SeedBytes];
    if (auto filled = TryFillRandom(Span<uint8>(seed)); !filled) {
        return Result<Ed25519SigningKey>::Failure(filled.Error());
    }

    detail::EnsureSodium();
    auto key = TryMakeFromSeed(Span<const uint8>(seed));
    SecureZero(Span<uint8>(seed));
    return key;
}

Result<Ed25519SigningKey> Ed25519SigningKey::TryFromSeed(Span<const uint8> seed) noexcept {
    if (seed.Size() != Ed25519SeedBytes) {
        return Result<Ed25519SigningKey>::Failure(ErrorCode::InvalidArgument);
    }

    detail::EnsureSodium();
    return TryMakeFromSeed(seed);
}

Ed25519PublicKey Ed25519SigningKey::PublicKey() const noexcept {
    Ed25519PublicKey key;
    if (!key_.IsEmpty()) {
        platform::MemoryCopy(key.bytes, key_.View().Data() + Ed25519SeedBytes,
                             Ed25519PublicKeyBytes);
    }

    return key;
}

Result<void> hazmat::TryExportSeed(const Ed25519SigningKey& key, Span<uint8> output) noexcept {
    const Span<const uint8> bytes = detail::KeyAccess::Bytes(key);
    if (bytes.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const Span<const uint8> seed = bytes.Left(Ed25519SeedBytes);
    if (output.Size() != Ed25519SeedBytes || Overlaps(output, bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), seed.Data(), Ed25519SeedBytes);
    return Result<void>::Success();
}

// ---- 일회성 ----

Result<Ed25519Signature> TrySign(const Ed25519SigningKey& key, Span<const uint8> message) noexcept {
    const Span<const uint8> secret = detail::KeyAccess::Bytes(key);
    if (secret.IsEmpty()) {
        return Result<Ed25519Signature>::Failure(ErrorCode::StaleHandle);
    }

    Ed25519Signature signature;
    detail::RequireSodium(crypto_sign_ed25519_detached(signature.bytes, nullptr,
                                                       detail::InputPointer(message),
                                                       message.Size(), secret.Data()),
                          "Crypto: crypto_sign_ed25519_detached failed");
    return Result<Ed25519Signature>::Success(signature);
}

Result<void> TryVerify(const Ed25519PublicKey& key, Span<const uint8> message,
                       Span<const uint8> signature) noexcept {
    // 서명 길이는 메시지에서 온 값이다. 틀리면 잘린 서명으로 본다(결정 D5).
    if (signature.Size() != Ed25519SignatureBytes) {
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    detail::EnsureSodium();
    if (crypto_sign_ed25519_verify_detached(signature.Data(), detail::InputPointer(message),
                                            message.Size(), key.bytes) != 0) {
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<void>::Success();
}

// ---- Ed25519phSigner ----

Ed25519phSigner::~Ed25519phSigner() noexcept {
    Release();
}

Ed25519phSigner::Ed25519phSigner(Ed25519phSigner&& other) noexcept
    : allocator_(other.allocator_), state_(other.state_), finished_(other.finished_) {
    other.allocator_ = nullptr;
    other.state_ = nullptr;
}

Ed25519phSigner& Ed25519phSigner::operator=(Ed25519phSigner&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        state_ = other.state_;
        finished_ = other.finished_;
        other.allocator_ = nullptr;
        other.state_ = nullptr;
    }

    return *this;
}

Result<Ed25519phSigner> Ed25519phSigner::TryCreate(Allocator& allocator) noexcept {
    auto state = TryAllocatePhState(allocator);
    if (!state) {
        return Result<Ed25519phSigner>::Failure(state.Error());
    }

    Ed25519phSigner signer;
    signer.allocator_ = &allocator;
    signer.state_ = state.Value();
    return Result<Ed25519phSigner>::Success(Move(signer));
}

Result<void> Ed25519phSigner::TryUpdate(Span<const uint8> input) noexcept {
    if (auto valid = CheckPhUpdate(state_, finished_); !valid) {
        return valid;
    }

    UpdatePhState(state_, input);
    return Result<void>::Success();
}

Result<void> Ed25519phSigner::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    if (auto valid = CheckPhUpdate(state_, finished_); !valid) {
        return valid;
    }

    for (Span<const uint8> piece : pieces) {
        UpdatePhState(state_, piece);
    }

    return Result<void>::Success();
}

Result<Ed25519Signature> Ed25519phSigner::TryFinish(const Ed25519SigningKey& key) noexcept {
    if (auto valid = CheckPhUpdate(state_, finished_); !valid) {
        return Result<Ed25519Signature>::Failure(valid.Error());
    }

    const Span<const uint8> secret = detail::KeyAccess::Bytes(key);
    if (secret.IsEmpty()) {
        return Result<Ed25519Signature>::Failure(ErrorCode::StaleHandle);
    }

    Ed25519Signature signature;
    detail::RequireSodium(crypto_sign_ed25519ph_final_create(AsPhState(state_), signature.bytes,
                                                             nullptr, secret.Data()),
                          "Crypto: crypto_sign_ed25519ph_final_create failed");
    finished_ = true;
    return Result<Ed25519Signature>::Success(signature);
}

void Ed25519phSigner::Reset() noexcept {
    if (state_ != nullptr) {
        WipePhState(state_);
        InitPhState(state_);
        finished_ = false;
    }
}

void Ed25519phSigner::Release() noexcept {
    if (state_ != nullptr) {
        ReleasePhState(allocator_, state_);
        state_ = nullptr;
        allocator_ = nullptr;
    }
}

// ---- Ed25519phVerifier ----

Ed25519phVerifier::~Ed25519phVerifier() noexcept {
    Release();
}

Ed25519phVerifier::Ed25519phVerifier(Ed25519phVerifier&& other) noexcept
    : allocator_(other.allocator_), state_(other.state_), finished_(other.finished_),
      verifyFailed_(other.verifyFailed_) {
    other.allocator_ = nullptr;
    other.state_ = nullptr;
}

Ed25519phVerifier& Ed25519phVerifier::operator=(Ed25519phVerifier&& other) noexcept {
    if (this != &other) {
        Release();
        allocator_ = other.allocator_;
        state_ = other.state_;
        finished_ = other.finished_;
        verifyFailed_ = other.verifyFailed_;
        other.allocator_ = nullptr;
        other.state_ = nullptr;
    }

    return *this;
}

Result<Ed25519phVerifier> Ed25519phVerifier::TryCreate(Allocator& allocator) noexcept {
    auto state = TryAllocatePhState(allocator);
    if (!state) {
        return Result<Ed25519phVerifier>::Failure(state.Error());
    }

    Ed25519phVerifier verifier;
    verifier.allocator_ = &allocator;
    verifier.state_ = state.Value();
    return Result<Ed25519phVerifier>::Success(Move(verifier));
}

Result<void> Ed25519phVerifier::TryUpdate(Span<const uint8> input) noexcept {
    if (auto valid = CheckPhVerifier(state_, finished_, verifyFailed_); !valid) {
        return valid;
    }

    UpdatePhState(state_, input);
    return Result<void>::Success();
}

Result<void> Ed25519phVerifier::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    if (auto valid = CheckPhVerifier(state_, finished_, verifyFailed_); !valid) {
        return valid;
    }

    for (Span<const uint8> piece : pieces) {
        UpdatePhState(state_, piece);
    }

    return Result<void>::Success();
}

Result<void> Ed25519phVerifier::TryFinish(const Ed25519PublicKey& key,
                                          Span<const uint8> signature) noexcept {
    auto index = TryFinishAny(Span<const Ed25519PublicKey>(&key, 1), signature);
    if (!index) {
        return Result<void>::Failure(index.Error());
    }

    return Result<void>::Success();
}

Result<usize> Ed25519phVerifier::TryFinishAny(Span<const Ed25519PublicKey> trusted,
                                              Span<const uint8> signature) noexcept {
    if (auto valid = CheckPhVerifier(state_, finished_, verifyFailed_); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    if (trusted.IsEmpty()) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    // 여기부터는 결과와 관계없이 끝난다. 서명 길이는 메시지에서 온 값이다(결정 D5).
    finished_ = true;
    bool found = false;
    usize index = 0;
    if (signature.Size() == Ed25519SignatureBytes) {
        for (usize i = 0; i < trusted.Size() && !found; ++i) {
            // final_verify가 상태를 끝내므로 사본으로 확인한다. 사본은 메시지 해시 상태라 비밀이
            // 아니지만 끝나면 지운다.
            crypto_sign_ed25519ph_state copy;
            platform::MemoryCopy(&copy, state_, PhStateBytes);
            found =
                crypto_sign_ed25519ph_final_verify(&copy, signature.Data(), trusted[i].bytes) == 0;
            index = i;
            SecureZero(Span<uint8>(reinterpret_cast<uint8*>(&copy), PhStateBytes));
        }
    }

    WipePhState(state_);
    if (!found) {
        verifyFailed_ = true;
        return Result<usize>::Failure(ErrorCode::AuthenticationFailed);
    }

    return Result<usize>::Success(index);
}

void Ed25519phVerifier::Reset() noexcept {
    if (state_ != nullptr) {
        WipePhState(state_);
        InitPhState(state_);
        finished_ = false;
        verifyFailed_ = false;
    }
}

void Ed25519phVerifier::Release() noexcept {
    if (state_ != nullptr) {
        ReleasePhState(allocator_, state_);
        state_ = nullptr;
        allocator_ = nullptr;
    }
}

} // namespace rocks::crypto
