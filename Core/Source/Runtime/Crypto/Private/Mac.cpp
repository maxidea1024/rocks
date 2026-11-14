#include <Crypto/Mac.h>

#include <Crypto/Bytes.h>

#include "KeyAccess.h"
#include "SecretArena.h"
#include "AdapterCommon.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

constexpr usize StateAlignment = 64;

static_assert(HmacSha256TagBytes == crypto_auth_hmacsha256_BYTES);
static_assert(HmacSha512TagBytes == crypto_auth_hmacsha512_BYTES);
static_assert(Blake2bKeyedTagBytes >= crypto_generichash_BYTES_MIN &&
              Blake2bKeyedTagBytes <= crypto_generichash_BYTES_MAX);
static_assert(HmacSha256TagBytes <= MaxMacTagBytes && HmacSha512TagBytes <= MaxMacTagBytes &&
              Blake2bKeyedTagBytes <= MaxMacTagBytes);
static_assert(MinMacKeyBytes == crypto_generichash_KEYBYTES_MIN);

// 세 알고리즘의 상태를 한 자리에 둔다(일회성 계산의 스택 상태, 증분 상태의 슬롯 크기).
union AnyMacState {
    crypto_auth_hmacsha256_state hmacSha256;
    crypto_auth_hmacsha512_state hmacSha512;
    crypto_generichash_state blake2b;
};

static_assert(alignof(AnyMacState) <= StateAlignment);
static_assert(sizeof(AnyMacState) <= detail::SecretSlotMaxBytes);

bool IsDefined(MacAlgorithm algorithm) noexcept {
    return algorithm == MacAlgorithm::HmacSha256 || algorithm == MacAlgorithm::HmacSha512 ||
           algorithm == MacAlgorithm::Blake2bKeyed;
}

usize TagBytes(MacAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        return HmacSha256TagBytes;
    case MacAlgorithm::HmacSha512:
        return HmacSha512TagBytes;
    case MacAlgorithm::Blake2bKeyed:
        return Blake2bKeyedTagBytes;
    }

    return 0;
}

usize DefaultKeyBytes(MacAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        return 32;
    case MacAlgorithm::HmacSha512:
        return 64;
    case MacAlgorithm::Blake2bKeyed:
        return crypto_generichash_KEYBYTES;
    }

    return 0;
}

// HMAC 키 하한은 기본 16 byte이고, hazmat 경로만 1 byte부터 받는다(minimumHmacBytes).
bool IsValidKeyLength(MacAlgorithm algorithm, usize bytes, usize minimumHmacBytes) noexcept {
    if (algorithm == MacAlgorithm::Blake2bKeyed) {
        return bytes >= crypto_generichash_KEYBYTES_MIN && bytes <= crypto_generichash_KEYBYTES_MAX;
    }

    return bytes >= minimumHmacBytes && bytes <= MaxHmacKeyBytes;
}

Result<SecretBytes> TryCopyKey(MacAlgorithm algorithm, Span<const uint8> keyBytes,
                               usize minimumHmacBytes) noexcept {
    if (!IsDefined(algorithm) || !IsValidKeyLength(algorithm, keyBytes.Size(), minimumHmacBytes)) {
        return Result<SecretBytes>::Failure(ErrorCode::InvalidArgument);
    }

    return SecretBytes::TryCopyFrom(keyBytes);
}

void InitState(MacAlgorithm algorithm, AnyMacState* state, Span<const uint8> key) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        detail::RequireSodium(
            crypto_auth_hmacsha256_init(&state->hmacSha256, key.Data(), key.Size()),
            "Crypto: crypto_auth_hmacsha256_init failed");
        return;
    case MacAlgorithm::HmacSha512:
        detail::RequireSodium(
            crypto_auth_hmacsha512_init(&state->hmacSha512, key.Data(), key.Size()),
            "Crypto: crypto_auth_hmacsha512_init failed");
        return;
    case MacAlgorithm::Blake2bKeyed:
        detail::RequireSodium(
            crypto_generichash_init(&state->blake2b, key.Data(), key.Size(), Blake2bKeyedTagBytes),
            "Crypto: crypto_generichash_init failed");
        return;
    }
}

void UpdateState(MacAlgorithm algorithm, AnyMacState* state, Span<const uint8> input) noexcept {
    if (input.IsEmpty()) {
        return;
    }

    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        detail::RequireSodium(
            crypto_auth_hmacsha256_update(&state->hmacSha256, input.Data(), input.Size()),
            "Crypto: crypto_auth_hmacsha256_update failed");
        return;
    case MacAlgorithm::HmacSha512:
        detail::RequireSodium(
            crypto_auth_hmacsha512_update(&state->hmacSha512, input.Data(), input.Size()),
            "Crypto: crypto_auth_hmacsha512_update failed");
        return;
    case MacAlgorithm::Blake2bKeyed:
        detail::RequireSodium(
            crypto_generichash_update(&state->blake2b, input.Data(), input.Size()),
            "Crypto: crypto_generichash_update failed");
        return;
    }
}

// tag.Size()는 TagBytes(algorithm)와 같아야 한다(호출자가 검사).
void FinishState(MacAlgorithm algorithm, AnyMacState* state, Span<uint8> tag) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        detail::RequireSodium(crypto_auth_hmacsha256_final(&state->hmacSha256, tag.Data()),
                              "Crypto: crypto_auth_hmacsha256_final failed");
        return;
    case MacAlgorithm::HmacSha512:
        detail::RequireSodium(crypto_auth_hmacsha512_final(&state->hmacSha512, tag.Data()),
                              "Crypto: crypto_auth_hmacsha512_final failed");
        return;
    case MacAlgorithm::Blake2bKeyed:
        detail::RequireSodium(crypto_generichash_final(&state->blake2b, tag.Data(), tag.Size()),
                              "Crypto: crypto_generichash_final failed");
        return;
    }
}

// 스택 상태로 tag를 계산하고 상태를 지운다(키에서 파생된 비밀이다).
void ComputeTag(const MacKey& key, Span<const Span<const uint8>> pieces, Span<uint8> tag) noexcept {
    detail::EnsureSodium();
    AnyMacState state;
    InitState(key.Algorithm(), &state, detail::KeyAccess::Bytes(key));
    for (Span<const uint8> piece : pieces) {
        UpdateState(key.Algorithm(), &state, piece);
    }

    FinishState(key.Algorithm(), &state, tag);
    sodium_memzero(&state, sizeof(state));
}

} // namespace

Result<usize> TryGetMacTagBytes(MacAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<usize>::Success(TagBytes(algorithm));
}

// ---- MacKey ----

Result<MacKey> MacKey::TryGenerate(MacAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<MacKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryGenerate(DefaultKeyBytes(algorithm));
    if (!key) {
        return Result<MacKey>::Failure(key.Error());
    }

    return Result<MacKey>::Success(MacKey(algorithm, Move(key.Value())));
}

Result<MacKey> MacKey::TryImport(MacAlgorithm algorithm, Span<const uint8> keyBytes) noexcept {
    auto key = TryCopyKey(algorithm, keyBytes, MinMacKeyBytes);
    if (!key) {
        return Result<MacKey>::Failure(key.Error());
    }

    return Result<MacKey>::Success(MacKey(algorithm, Move(key.Value())));
}

Result<MacKey> hazmat::TryImportShortMacKey(MacAlgorithm algorithm,
                                            Span<const uint8> keyBytes) noexcept {
    auto key = TryCopyKey(algorithm, keyBytes, 1);
    if (!key) {
        return Result<MacKey>::Failure(key.Error());
    }

    return Result<MacKey>::Success(MacKey(algorithm, Move(key.Value())));
}

Result<void> MacKey::TryExport(Span<uint8> output) const noexcept {
    if (key_.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != key_.Size() || Overlaps(output, key_.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::MemoryCopy(output.Data(), key_.View().Data(), key_.Size());
    return Result<void>::Success();
}

// ---- 일회성 ----

Result<void> TryMac(const MacKey& key, Span<const uint8> message, Span<uint8> tag) noexcept {
    const Span<const uint8> pieces[1] = {message};
    return TryMac(key, Span<const Span<const uint8>>(pieces), tag);
}

Result<void> TryMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                    Span<uint8> tag) noexcept {
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (tag.Size() != TagBytes(key.Algorithm()) || detail::OutputOverlaps(pieces, tag)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ComputeTag(key, pieces, tag);
    return Result<void>::Success();
}

Result<void> TryVerifyMac(const MacKey& key, Span<const uint8> message,
                          Span<const uint8> tag) noexcept {
    const Span<const uint8> pieces[1] = {message};
    return TryVerifyMac(key, Span<const Span<const uint8>>(pieces), tag);
}

Result<void> TryVerifyMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                          Span<const uint8> tag) noexcept {
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    const usize bytes = TagBytes(key.Algorithm());
    if (tag.Size() != bytes) {
        return Result<void>::Failure(ErrorCode::AuthenticationFailed);
    }

    uint8 computed[MaxMacTagBytes];
    ComputeTag(key, pieces, Span<uint8>(computed, bytes));
    const bool equal = ConstantTimeEquals(Span<const uint8>(computed, bytes), tag);
    sodium_memzero(computed, sizeof(computed));
    return equal ? Result<void>::Success() : Result<void>::Failure(ErrorCode::AuthenticationFailed);
}

// ---- MacState ----

MacState::~MacState() noexcept {
    Release();
}

MacState::MacState(MacState&& other) noexcept
    : state_(other.state_), algorithm_(other.algorithm_), finished_(other.finished_),
      verifyFailed_(other.verifyFailed_) {
    other.state_ = nullptr;
}

MacState& MacState::operator=(MacState&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        algorithm_ = other.algorithm_;
        finished_ = other.finished_;
        verifyFailed_ = other.verifyFailed_;
        other.state_ = nullptr;
    }

    return *this;
}

Result<MacState> MacState::TryCreate(const MacKey& key) noexcept {
    if (key.IsEmpty()) {
        return Result<MacState>::Failure(ErrorCode::StaleHandle);
    }

    detail::EnsureSodium();
    auto slot = detail::TryAllocateSecretSlot(sizeof(AnyMacState), StateAlignment);
    if (!slot) {
        return Result<MacState>::Failure(slot.Error());
    }

    MacState state;
    state.state_ = slot.Value();
    state.algorithm_ = key.Algorithm();
    InitState(state.algorithm_, static_cast<AnyMacState*>(state.state_),
              detail::KeyAccess::Bytes(key));
    return Result<MacState>::Success(Move(state));
}

usize MacState::TagBytes() const noexcept {
    return rocks::crypto::TagBytes(algorithm_);
}

// 쓸 수 있는 상태인가. 끝난 뒤에는 검증 실패를 sticky로 돌려준다(9.1절).
Result<void> MacState::CheckActive() const noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_) {
        return Result<void>::Failure(verifyFailed_ ? ErrorCode::AuthenticationFailed
                                                   : ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> MacState::TryUpdate(Span<const uint8> input) noexcept {
    const Span<const uint8> pieces[1] = {input};
    return TryUpdate(Span<const Span<const uint8>>(pieces));
}

Result<void> MacState::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    if (auto active = CheckActive(); !active) {
        return active;
    }

    for (Span<const uint8> piece : pieces) {
        UpdateState(algorithm_, static_cast<AnyMacState*>(state_), piece);
    }

    return Result<void>::Success();
}

Result<void> MacState::TryFinish(Span<uint8> tag) noexcept {
    if (auto active = CheckActive(); !active) {
        return active;
    }

    // 길이 오류는 상태를 바꾸지 않는다(맞는 버퍼로 다시 부를 수 있다).
    if (tag.Size() != TagBytes()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝난 상태는 다시 쓰지 않으므로 바로 지운다(키에서 파생된 내부 상태가 남지 않게).
    FinishState(algorithm_, static_cast<AnyMacState*>(state_), tag);
    sodium_memzero(state_, sizeof(AnyMacState));
    finished_ = true;
    return Result<void>::Success();
}

Result<void> MacState::TryFinishVerify(Span<const uint8> expectedTag) noexcept {
    if (auto active = CheckActive(); !active) {
        return active;
    }

    // 검증은 성공·실패와 관계없이 상태를 끝낸다(같은 상태로 다시 시도하지 않는다).
    finished_ = true;
    const usize bytes = TagBytes();
    uint8 computed[MaxMacTagBytes];
    FinishState(algorithm_, static_cast<AnyMacState*>(state_), Span<uint8>(computed, bytes));
    sodium_memzero(state_, sizeof(AnyMacState));
    const bool equal = expectedTag.Size() == bytes &&
                       ConstantTimeEquals(Span<const uint8>(computed, bytes), expectedTag);
    sodium_memzero(computed, sizeof(computed));
    verifyFailed_ = !equal;
    return equal ? Result<void>::Success() : Result<void>::Failure(ErrorCode::AuthenticationFailed);
}

void MacState::Release() noexcept {
    if (state_ != nullptr) {
        detail::FreeSecretSlot(state_);
        state_ = nullptr;
    }
}

} // namespace rocks::crypto
