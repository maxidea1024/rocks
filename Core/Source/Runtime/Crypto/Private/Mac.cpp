#include <Crypto/Mac.h>

#include <Crypto/Bytes.h>

#include "AdapterCommon.h"
#include "AeadInternal.h"
#include "Aes/AesKernels.h"
#include "Aes/AesModesInternal.h"
#include "KeyAccess.h"
#include "MacInternal.h"
#include "SecretArena.h"
#include "Sodium/SodiumRuntime.h"

#include <Platform/Fatal.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto {
namespace {

constexpr usize StateAlignment = 64;

static_assert(HmacSha256TagBytes == crypto_auth_hmacsha256_BYTES);
static_assert(HmacSha512TagBytes == crypto_auth_hmacsha512_BYTES);
static_assert(Blake2bKeyedTagBytes >= crypto_generichash_BYTES_MIN &&
              Blake2bKeyedTagBytes <= crypto_generichash_BYTES_MAX);
static_assert(AesCmacTagBytes == detail::AesCmacTagBytes);
static_assert(HmacSha256TagBytes <= MaxMacTagBytes && HmacSha512TagBytes <= MaxMacTagBytes &&
              Blake2bKeyedTagBytes <= MaxMacTagBytes && AesCmacTagBytes <= MaxMacTagBytes);
static_assert(MinMacKeyBytes == crypto_generichash_KEYBYTES_MIN);

// AES-CMAC 키 길이다(AES-128·AES-256).
constexpr usize AesCmac128KeyBytes = 16;
constexpr usize AesCmac256KeyBytes = 32;

// libsodium 세 알고리즘의 상태를 한 자리에 둔다(일회성 계산의 스택 상태, 증분 상태의 슬롯 크기).
union AnyMacState {
    crypto_auth_hmacsha256_state hmacSha256;
    crypto_auth_hmacsha512_state hmacSha512;
    crypto_generichash_state blake2b;
};

// AES-CMAC 증분 상태다. 키 스케줄 사본을 함께 둔다(MacState는 키 객체와 수명이 독립이다).
struct CmacMacState {
    detail::AesSchedule schedule;
    detail::AesCmacState cmac;
};

static_assert(alignof(AnyMacState) <= StateAlignment && alignof(CmacMacState) <= StateAlignment);
static_assert(sizeof(AnyMacState) <= detail::SecretSlotMaxBytes &&
              sizeof(CmacMacState) <= detail::SecretSlotMaxBytes);

bool IsDefined(MacAlgorithm algorithm) noexcept {
    return algorithm == MacAlgorithm::HmacSha256 || algorithm == MacAlgorithm::HmacSha512 ||
           algorithm == MacAlgorithm::Blake2bKeyed || algorithm == MacAlgorithm::AesCmac;
}

// 증분 상태 슬롯 크기다.
usize StateBytes(MacAlgorithm algorithm) noexcept {
    return algorithm == MacAlgorithm::AesCmac ? sizeof(CmacMacState) : sizeof(AnyMacState);
}

Result<void> CheckUsable(MacAlgorithm algorithm) noexcept {
    if (!IsDefined(algorithm)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // AES-CMAC은 AES 계열의 HW 조건 하나를 따른다(6.1절).
    if (algorithm == MacAlgorithm::AesCmac && !detail::IsAesAvailable()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return Result<void>::Success();
}

usize TagBytes(MacAlgorithm algorithm) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
        return HmacSha256TagBytes;
    case MacAlgorithm::HmacSha512:
        return HmacSha512TagBytes;
    case MacAlgorithm::Blake2bKeyed:
        return Blake2bKeyedTagBytes;
    case MacAlgorithm::AesCmac:
        return AesCmacTagBytes;
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
    case MacAlgorithm::AesCmac:
        return AesCmac256KeyBytes;
    }

    return 0;
}

// HMAC 키 하한은 기본 16 byte이고, hazmat 경로만 1 byte부터 받는다(minimumHmacBytes). AES-CMAC은
// 16·32 byte만 받는다(AES-192는 경량 계층에 없다).
bool IsValidKeyLength(MacAlgorithm algorithm, usize bytes, usize minimumHmacBytes) noexcept {
    switch (algorithm) {
    case MacAlgorithm::HmacSha256:
    case MacAlgorithm::HmacSha512:
        return bytes >= minimumHmacBytes && bytes <= MaxHmacKeyBytes;
    case MacAlgorithm::Blake2bKeyed:
        return bytes >= crypto_generichash_KEYBYTES_MIN && bytes <= crypto_generichash_KEYBYTES_MAX;
    case MacAlgorithm::AesCmac:
        return bytes == AesCmac128KeyBytes || bytes == AesCmac256KeyBytes;
    }

    return false;
}

// 키를 만든다. 알고리즘(InvalidArgument·Unsupported) → 길이(InvalidArgument) → 키 저장소 순이다.
Result<MacKey> TryImportKey(MacAlgorithm algorithm, Span<const uint8> keyBytes,
                            usize minimumHmacBytes) noexcept {
    if (auto usable = CheckUsable(algorithm); !usable) {
        return Result<MacKey>::Failure(usable.Error());
    }

    if (!IsValidKeyLength(algorithm, keyBytes.Size(), minimumHmacBytes)) {
        return Result<MacKey>::Failure(ErrorCode::InvalidArgument);
    }

    auto key = SecretBytes::TryCopyFrom(keyBytes);
    if (!key) {
        return Result<MacKey>::Failure(key.Error());
    }

    return detail::TryMakeMacKey(algorithm, Move(key.Value()));
}

const detail::AesSchedule& CmacScheduleOf(const MacKey& key) noexcept {
    const void* state = detail::KeyAccess::State(key);
    if (state == nullptr) {
        platform::Fatal("Crypto: AES-CMAC key has no AES schedule");
    }

    return *static_cast<const detail::AesSchedule*>(state);
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
    case MacAlgorithm::AesCmac:
        break;
    }

    platform::Fatal("Crypto: MAC init reached an algorithm without a libsodium state");
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
    case MacAlgorithm::AesCmac:
        break;
    }

    platform::Fatal("Crypto: MAC update reached an algorithm without a libsodium state");
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
    case MacAlgorithm::AesCmac:
        break;
    }

    platform::Fatal("Crypto: MAC finish reached an algorithm without a libsodium state");
}

// 증분 상태(키 저장소 슬롯)에 입력을 넣는다.
void UpdateSlot(MacAlgorithm algorithm, void* slot, Span<const uint8> input) noexcept {
    if (algorithm == MacAlgorithm::AesCmac) {
        auto* state = static_cast<CmacMacState*>(slot);
        detail::AesCmacUpdate(detail::GetAesKernels(), state->schedule, state->cmac, input);
        return;
    }

    UpdateState(algorithm, static_cast<AnyMacState*>(slot), input);
}

// 증분 상태로 tag를 쓰고 슬롯 내용을 지운다(키에서 파생된 내부 상태가 남지 않게).
void FinishSlot(MacAlgorithm algorithm, void* slot, Span<uint8> tag) noexcept {
    if (algorithm == MacAlgorithm::AesCmac) {
        auto* state = static_cast<CmacMacState*>(slot);
        detail::AesCmacFinish(detail::GetAesKernels(), state->schedule, state->cmac, tag);
    } else {
        FinishState(algorithm, static_cast<AnyMacState*>(slot), tag);
    }

    sodium_memzero(slot, StateBytes(algorithm));
}

// 스택 상태로 tag를 계산하고 상태를 지운다(키에서 파생된 비밀이다). AES-CMAC은 키의 스케줄을 그대로
// 쓴다.
void ComputeTag(const MacKey& key, Span<const Span<const uint8>> pieces, Span<uint8> tag) noexcept {
    detail::EnsureSodium();
    if (key.Algorithm() == MacAlgorithm::AesCmac) {
        const detail::AesKernels& kernels = detail::GetAesKernels();
        const detail::AesSchedule& schedule = CmacScheduleOf(key);
        detail::AesCmacState state = {};
        for (Span<const uint8> piece : pieces) {
            detail::AesCmacUpdate(kernels, schedule, state, piece);
        }

        detail::AesCmacFinish(kernels, schedule, state, tag);
        return;
    }

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

MacKey::~MacKey() noexcept {
    Release();
}

MacKey::MacKey(MacKey&& other) noexcept
    : algorithm_(other.algorithm_), key_(Move(other.key_)), state_(other.state_) {
    other.state_ = nullptr;
}

MacKey& MacKey::operator=(MacKey&& other) noexcept {
    if (this != &other) {
        Release();
        algorithm_ = other.algorithm_;
        key_ = Move(other.key_);
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

void MacKey::Release() noexcept {
    // AES-CMAC 스케줄 슬롯은 해제 때 지워진다(SecretArena).
    detail::DestroyAesSchedule(static_cast<detail::AesSchedule*>(state_));
    state_ = nullptr;
    key_.Reset();
}

Result<MacKey> MacKey::TryGenerate(MacAlgorithm algorithm) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto usable = CheckUsable(algorithm); !usable) {
        return Result<MacKey>::Failure(usable.Error());
    }

    auto key = SecretBytes::TryGenerate(DefaultKeyBytes(algorithm));
    if (!key) {
        return Result<MacKey>::Failure(key.Error());
    }

    return detail::TryMakeMacKey(algorithm, Move(key.Value()));
}

Result<MacKey> MacKey::TryImport(MacAlgorithm algorithm, Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryImportKey(algorithm, keyBytes, MinMacKeyBytes);
}

Result<MacKey> hazmat::TryImportShortMacKey(MacAlgorithm algorithm,
                                            Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    return TryImportKey(algorithm, keyBytes, 1);
}

Result<void> hazmat::TryExportKey(const MacKey& key, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
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

// ---- 모듈 내부(다른 키 파생 경로가 쓴다) ----

bool detail::IsDefinedMacAlgorithm(MacAlgorithm algorithm) noexcept {
    return IsDefined(algorithm);
}

Result<void> detail::CheckMacUsable(MacAlgorithm algorithm) noexcept {
    return CheckUsable(algorithm);
}

usize detail::DefaultMacKeyBytes(MacAlgorithm algorithm) noexcept {
    return DefaultKeyBytes(algorithm);
}

bool detail::IsValidMacKeyBytes(MacAlgorithm algorithm, usize bytes) noexcept {
    return IsValidKeyLength(algorithm, bytes, MinMacKeyBytes);
}

Result<MacKey> detail::TryMakeMacKey(MacAlgorithm algorithm, SecretBytes&& key) noexcept {
    platform::DataIndependentTimingScope dit;
    // 다른 TU가 부르는 진입점이다. 전제(정의·사용 가능, 키 길이)가 깨지면 HW 없는 기기에서 AES
    // 명령으로 죽거나 길이 규칙이 우회되므로 결함으로 멈춘다. 짧은 HMAC 키(hazmat)는 1 byte부터다.
    if (!IsDefined(algorithm) || !CheckUsable(algorithm) ||
        !IsValidKeyLength(algorithm, key.Size(), 1)) {
        platform::Fatal("Crypto: TryMakeMacKey precondition violated");
    }

    void* state = nullptr;
    if (algorithm == MacAlgorithm::AesCmac) {
        auto schedule = TryCreateAesSchedule(GetAesKernels(), key.View());
        if (!schedule) {
            return Result<MacKey>::Failure(schedule.Error());
        }

        state = schedule.Value();
    }

    return Result<MacKey>::Success(KeyAccess::Make<MacKey>(algorithm, Move(key), state));
}

// ---- 일회성 ----

Result<void> TryMac(const MacKey& key, Span<const uint8> message, Span<uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {message};
    return TryMac(key, Span<const Span<const uint8>>(pieces), tag);
}

Result<void> TryMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                    Span<uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {message};
    return TryVerifyMac(key, Span<const Span<const uint8>>(pieces), tag);
}

Result<void> TryVerifyMac(const MacKey& key, Span<const Span<const uint8>> pieces,
                          Span<const uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
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
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<MacState>::Failure(ErrorCode::StaleHandle);
    }

    detail::EnsureSodium();
    auto slot = detail::TryAllocateSecretSlot(StateBytes(key.Algorithm()), StateAlignment);
    if (!slot) {
        return Result<MacState>::Failure(slot.Error());
    }

    MacState state;
    state.state_ = slot.Value();
    state.algorithm_ = key.Algorithm();
    if (state.algorithm_ == MacAlgorithm::AesCmac) {
        auto* cmac = ConstructAt<CmacMacState>(state.state_);
        platform::MemoryZero(cmac, sizeof(CmacMacState));
        platform::MemoryCopy(&cmac->schedule, &CmacScheduleOf(key), sizeof(detail::AesSchedule));
    } else {
        InitState(state.algorithm_, static_cast<AnyMacState*>(state.state_),
                  detail::KeyAccess::Bytes(key));
    }

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
    platform::DataIndependentTimingScope dit;
    const Span<const uint8> pieces[1] = {input};
    return TryUpdate(Span<const Span<const uint8>>(pieces));
}

Result<void> MacState::TryUpdate(Span<const Span<const uint8>> pieces) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    for (Span<const uint8> piece : pieces) {
        UpdateSlot(algorithm_, state_, piece);
    }

    return Result<void>::Success();
}

Result<void> MacState::TryFinish(Span<uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    // 길이 오류는 상태를 바꾸지 않는다(맞는 버퍼로 다시 부를 수 있다).
    if (tag.Size() != TagBytes()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝난 상태는 다시 쓰지 않으므로 바로 지운다(키에서 파생된 내부 상태가 남지 않게).
    FinishSlot(algorithm_, state_, tag);
    finished_ = true;
    return Result<void>::Success();
}

Result<void> MacState::TryFinishVerify(Span<const uint8> expectedTag) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    // 검증은 성공·실패와 관계없이 상태를 끝낸다(같은 상태로 다시 시도하지 않는다).
    finished_ = true;
    const usize bytes = TagBytes();
    uint8 computed[MaxMacTagBytes];
    FinishSlot(algorithm_, state_, Span<uint8>(computed, bytes));
    const bool equal = expectedTag.Size() == bytes &&
                       ConstantTimeEquals(Span<const uint8>(computed, bytes), expectedTag);
    sodium_memzero(computed, sizeof(computed));
    verifyFailed_ = !equal;
    return equal ? Result<void>::Success() : Result<void>::Failure(ErrorCode::AuthenticationFailed);
}

void MacState::Release() noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ != nullptr) {
        detail::FreeSecretSlot(state_);
        state_ = nullptr;
    }
}

} // namespace rocks::crypto
