#include <Crypto/AesModes.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "Aes/AesKernels.h"
#include "Aes/AesModesInternal.h"
#include "AeadInternal.h"
#include "KeyAccess.h"
#include "SecretArena.h"

#include <Core/Move.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 저수준 AES 모드(Crypto/AesModes.h)의 어댑터다: 인자·단계·한도 검사, 키 저장소 슬롯 수명, 오류
// 변환. 계산은 AES 경량 계층(Aes/AesModesInternal.h)이 한다.
namespace rocks::crypto {
namespace {

static_assert(hazmat::AesBlockBytes == detail::AesBlockBytes);
static_assert(hazmat::AesGcmNonceBytes == detail::AesGcmNonceBytes &&
              hazmat::AesGcmTagBytes == detail::AesGcmTagBytes);
static_assert(hazmat::AesGcmMaxMessageBytes == detail::AesGcmMaxMessageBytes &&
              hazmat::AesGcmMaxAssociatedBytes == detail::AesGcmMaxAssociatedBytes);
static_assert(sizeof(detail::AesGcmStream) <= detail::SecretSlotMaxBytes &&
              sizeof(detail::AesCtrStream) <= detail::SecretSlotMaxBytes);

const detail::AesSchedule& AesHazmatScheduleOf(const hazmat::AesKey& key) noexcept {
    return *static_cast<const detail::AesSchedule*>(detail::KeyAccess::State(key));
}

// 열거값이면 커널 폭으로 바꾼다.
bool TryMapWidth(hazmat::AesCtrCounterWidth width, detail::AesCounterWidth& mapped) noexcept {
    switch (width) {
    case hazmat::AesCtrCounterWidth::Bits32:
        mapped = detail::AesCounterWidth::Bits32;
        return true;
    case hazmat::AesCtrCounterWidth::Bits64:
        mapped = detail::AesCounterWidth::Bits64;
        return true;
    case hazmat::AesCtrCounterWidth::Bits128:
        mapped = detail::AesCounterWidth::Bits128;
        return true;
    }

    return false;
}

// 출력이 입력과 같은 주소에서 시작하는 제자리가 아닌데 겹치는가(7절).
bool BadOverlap(Span<const uint8> input, Span<uint8> output) noexcept {
    return Overlaps(input, output) && input.Data() != output.Data();
}

// 블록 수(올림)다.
uint64 AesHazmatCountBlocks(usize bytes) noexcept {
    return static_cast<uint64>(bytes / detail::AesBlockBytes) +
           (bytes % detail::AesBlockBytes != 0 ? 1 : 0);
}

// 키 저장소 슬롯을 지우고 돌려준다(슬롯은 해제 때 전체가 지워진다, SecretArena).
void FreeState(void*& state) noexcept {
    if (state != nullptr) {
        detail::FreeSecretSlot(state);
        state = nullptr;
    }
}

// GCM 멀티파트 상태를 만든다. 빈 키 StaleHandle → AES-GCM이 아닌 키 InvalidArgument → 96 bit가
// 아닌 nonce Unsupported → 키 저장소 OutOfMemory 순이다.
Result<void*> TryStartGcm(const AeadKey& key, Span<const uint8> nonce) noexcept {
    if (key.IsEmpty()) {
        return Result<void*>::Failure(ErrorCode::StaleHandle);
    }

    if (key.Algorithm() != AeadAlgorithm::Aes128Gcm &&
        key.Algorithm() != AeadAlgorithm::Aes256Gcm) {
        return Result<void*>::Failure(ErrorCode::InvalidArgument);
    }

    if (nonce.Size() != detail::AesGcmNonceBytes) {
        return Result<void*>::Failure(ErrorCode::Unsupported);
    }

    const void* schedule = detail::KeyAccess::Schedule(key);
    if (schedule == nullptr) {
        platform::Fatal("Crypto: AES-GCM key has no AES schedule");
    }

    auto slot =
        detail::TryAllocateSecretSlot(sizeof(detail::AesGcmStream), alignof(detail::AesGcmStream));
    if (!slot) {
        return slot;
    }

    auto* stream = InitializeAt<detail::AesGcmStream>(slot.Value());
    detail::AesGcmStreamStart(detail::GetAesKernels(),
                              *static_cast<const detail::AesSchedule*>(schedule), nonce, *stream);
    return slot;
}

detail::AesGcmStream& GcmOf(void* state) noexcept {
    return *static_cast<detail::AesGcmStream*>(state);
}

// AD 흡수의 공통 검사다(단계·한도). 상태는 바꾸지 않는다.
Result<void> CheckGcmAssociated(const detail::AesGcmStream& stream,
                                Span<const uint8> associatedData) noexcept {
    if (stream.dataStarted) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (static_cast<uint64>(associatedData.Size()) >
        detail::AesGcmMaxAssociatedBytes - stream.associatedBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

// 데이터 처리의 공통 검사다(겹침 → 출력 길이 → 한도). 상태는 바꾸지 않는다.
Result<void> CheckGcmData(const detail::AesGcmStream& stream, Span<const uint8> input,
                          Span<uint8> output) noexcept {
    if (BadOverlap(input, output.Left(input.Size()))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (output.Size() < input.Size() ||
        static_cast<uint64>(input.Size()) > detail::AesGcmMaxMessageBytes - stream.messageBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace

// ---- AesKey ----

hazmat::AesKey::~AesKey() noexcept {
    Release();
}

hazmat::AesKey::AesKey(AesKey&& other) noexcept : state_(other.state_), keyBytes_(other.keyBytes_) {
    other.state_ = nullptr;
    other.keyBytes_ = 0;
}

hazmat::AesKey& hazmat::AesKey::operator=(AesKey&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        keyBytes_ = other.keyBytes_;
        other.state_ = nullptr;
        other.keyBytes_ = 0;
    }

    return *this;
}

void hazmat::AesKey::Release() noexcept {
    detail::DestroyAesSchedule(static_cast<detail::AesSchedule*>(state_));
    state_ = nullptr;
    keyBytes_ = 0;
}

Result<hazmat::AesKey> hazmat::AesKey::TryGenerate(usize keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    if (keyBytes != Aes128KeyBytes && keyBytes != Aes256KeyBytes) {
        return Result<AesKey>::Failure(ErrorCode::InvalidArgument);
    }

    if (!detail::IsAesAvailable()) {
        return Result<AesKey>::Failure(ErrorCode::Unsupported);
    }

    uint8 bytes[Aes256KeyBytes];
    const Span<uint8> view(bytes, keyBytes);
    if (auto filled = TryFillRandom(view); !filled) {
        SecureZero(Span<uint8>(bytes));
        return Result<AesKey>::Failure(filled.Error());
    }

    auto key = TryImport(view);
    SecureZero(Span<uint8>(bytes));
    return key;
}

Result<hazmat::AesKey> hazmat::AesKey::TryImport(Span<const uint8> keyBytes) noexcept {
    platform::DataIndependentTimingScope dit;
    if (keyBytes.Size() != Aes128KeyBytes && keyBytes.Size() != Aes256KeyBytes) {
        return Result<AesKey>::Failure(ErrorCode::InvalidArgument);
    }

    if (!detail::IsAesAvailable()) {
        return Result<AesKey>::Failure(ErrorCode::Unsupported);
    }

    auto schedule = detail::TryCreateAesSchedule(detail::GetAesKernels(), keyBytes);
    if (!schedule) {
        return Result<AesKey>::Failure(schedule.Error());
    }

    AesKey key;
    key.state_ = schedule.Value();
    key.keyBytes_ = keyBytes.Size();
    return Result<AesKey>::Success(Move(key));
}

// ---- CTR ----

hazmat::AesCtr::~AesCtr() noexcept {
    Reset();
}

hazmat::AesCtr::AesCtr(AesCtr&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

hazmat::AesCtr& hazmat::AesCtr::operator=(AesCtr&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

void hazmat::AesCtr::Reset() noexcept {
    FreeState(state_);
}

Result<hazmat::AesCtr> hazmat::AesCtr::TryCreate(const AesKey& key,
                                                 Span<const uint8> initialCounterBlock,
                                                 AesCtrCounterWidth width) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<AesCtr>::Failure(ErrorCode::StaleHandle);
    }

    detail::AesCounterWidth mapped{};
    if (initialCounterBlock.Size() != AesBlockBytes || !TryMapWidth(width, mapped)) {
        return Result<AesCtr>::Failure(ErrorCode::InvalidArgument);
    }

    auto slot =
        detail::TryAllocateSecretSlot(sizeof(detail::AesCtrStream), alignof(detail::AesCtrStream));
    if (!slot) {
        return Result<AesCtr>::Failure(slot.Error());
    }

    detail::AesBlock counter;
    platform::MemoryCopy(counter.bytes, initialCounterBlock.Data(), AesBlockBytes);
    auto* stream = InitializeAt<detail::AesCtrStream>(slot.Value());
    detail::AesCtrStreamStart(detail::GetAesKernels(), AesHazmatScheduleOf(key), counter, mapped,
                              *stream);
    AesCtr ctr;
    ctr.state_ = slot.Value();
    return Result<AesCtr>::Success(Move(ctr));
}

Result<void> hazmat::AesCtr::TryProcess(Span<const uint8> input, Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (output.Size() != input.Size() || BadOverlap(input, output)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto& stream = *static_cast<detail::AesCtrStream*>(state_);
    const uint64 blocks = detail::AesCtrStreamBlocksFor(stream, input.Size());
    if (!detail::AesCtrHasCapacity(stream.counter, stream.width, stream.exhausted, blocks)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    detail::AesCtrStreamProcess(stream, input, output);
    return Result<void>::Success();
}

Result<void> hazmat::TryAesCtrXor(const AesKey& key, Span<const uint8> initialCounterBlock,
                                  Span<const uint8> input, Span<uint8> output,
                                  AesCtrCounterWidth width) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    detail::AesCounterWidth mapped{};
    if (initialCounterBlock.Size() != AesBlockBytes || !TryMapWidth(width, mapped) ||
        output.Size() != input.Size() || BadOverlap(input, output)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    detail::AesBlock counter;
    platform::MemoryCopy(counter.bytes, initialCounterBlock.Data(), AesBlockBytes);
    if (!detail::AesCtrHasCapacity(counter, mapped, false, AesHazmatCountBlocks(input.Size()))) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (!input.IsEmpty()) {
        detail::GetAesKernels().ctrXor(AesHazmatScheduleOf(key), mapped, counter, input.Data(),
                                       output.Data(), input.Size());
    }

    return Result<void>::Success();
}

// ---- AES-GCM 멀티파트 ----

hazmat::AesGcmEncryptor::~AesGcmEncryptor() noexcept {
    Reset();
}

hazmat::AesGcmEncryptor::AesGcmEncryptor(AesGcmEncryptor&& other) noexcept
    : state_(other.state_), finished_(other.finished_) {
    other.state_ = nullptr;
    other.finished_ = false;
}

hazmat::AesGcmEncryptor& hazmat::AesGcmEncryptor::operator=(AesGcmEncryptor&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        finished_ = other.finished_;
        other.state_ = nullptr;
        other.finished_ = false;
    }

    return *this;
}

void hazmat::AesGcmEncryptor::Reset() noexcept {
    FreeState(state_);
    finished_ = false;
}

Result<hazmat::AesGcmEncryptor>
hazmat::AesGcmEncryptor::TryCreate(const AeadKey& key, Span<const uint8> nonce) noexcept {
    platform::DataIndependentTimingScope dit;
    auto state = TryStartGcm(key, nonce);
    if (!state) {
        return Result<AesGcmEncryptor>::Failure(state.Error());
    }

    AesGcmEncryptor encryptor;
    encryptor.state_ = state.Value();
    return Result<AesGcmEncryptor>::Success(Move(encryptor));
}

Result<void> hazmat::AesGcmEncryptor::CheckActive() const noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void>
hazmat::AesGcmEncryptor::TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    detail::AesGcmStream& stream = GcmOf(state_);
    if (auto valid = CheckGcmAssociated(stream, associatedData); !valid) {
        return valid;
    }

    detail::AesGcmStreamAbsorbAssociated(stream, associatedData);
    return Result<void>::Success();
}

Result<usize> hazmat::AesGcmEncryptor::TryUpdate(Span<const uint8> plaintext,
                                                 Span<uint8> ciphertext) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return Result<usize>::Failure(active.Error());
    }

    detail::AesGcmStream& stream = GcmOf(state_);
    if (auto valid = CheckGcmData(stream, plaintext, ciphertext); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    detail::AesGcmStreamProcess(stream, true, plaintext, ciphertext.Left(plaintext.Size()));
    return Result<usize>::Success(plaintext.Size());
}

Result<void> hazmat::AesGcmEncryptor::TryFinish(Span<uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    if (tag.Size() != AesGcmTagBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 끝나면 상태(스케줄 사본 포함)를 지운다. 슬롯은 Reset·소멸 때 돌려준다.
    detail::AesGcmStreamFinish(GcmOf(state_), tag);
    finished_ = true;
    return Result<void>::Success();
}

hazmat::AesGcmDecryptor::~AesGcmDecryptor() noexcept {
    Reset();
}

hazmat::AesGcmDecryptor::AesGcmDecryptor(AesGcmDecryptor&& other) noexcept
    : state_(other.state_), finished_(other.finished_), failed_(other.failed_) {
    other.state_ = nullptr;
    other.finished_ = false;
    other.failed_ = false;
}

hazmat::AesGcmDecryptor& hazmat::AesGcmDecryptor::operator=(AesGcmDecryptor&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        finished_ = other.finished_;
        failed_ = other.failed_;
        other.state_ = nullptr;
        other.finished_ = false;
        other.failed_ = false;
    }

    return *this;
}

void hazmat::AesGcmDecryptor::Reset() noexcept {
    FreeState(state_);
    finished_ = false;
    failed_ = false;
}

Result<hazmat::AesGcmDecryptor>
hazmat::AesGcmDecryptor::TryCreate(const AeadKey& key, Span<const uint8> nonce) noexcept {
    platform::DataIndependentTimingScope dit;
    auto state = TryStartGcm(key, nonce);
    if (!state) {
        return Result<AesGcmDecryptor>::Failure(state.Error());
    }

    AesGcmDecryptor decryptor;
    decryptor.state_ = state.Value();
    return Result<AesGcmDecryptor>::Success(Move(decryptor));
}

// 끝난 뒤에는 검증 실패를 sticky로 돌려준다(9.1절, MacState와 같다).
Result<void> hazmat::AesGcmDecryptor::CheckActive() const noexcept {
    if (state_ == nullptr) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (finished_) {
        return Result<void>::Failure(failed_ ? ErrorCode::AuthenticationFailed
                                             : ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void>
hazmat::AesGcmDecryptor::TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    detail::AesGcmStream& stream = GcmOf(state_);
    if (auto valid = CheckGcmAssociated(stream, associatedData); !valid) {
        return valid;
    }

    detail::AesGcmStreamAbsorbAssociated(stream, associatedData);
    return Result<void>::Success();
}

Result<usize> hazmat::AesGcmDecryptor::TryUpdateUnverified(Span<const uint8> ciphertext,
                                                           Span<uint8> plaintext) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return Result<usize>::Failure(active.Error());
    }

    detail::AesGcmStream& stream = GcmOf(state_);
    if (auto valid = CheckGcmData(stream, ciphertext, plaintext); !valid) {
        return Result<usize>::Failure(valid.Error());
    }

    detail::AesGcmStreamProcess(stream, false, ciphertext, plaintext.Left(ciphertext.Size()));
    return Result<usize>::Success(ciphertext.Size());
}

Result<void> hazmat::AesGcmDecryptor::TryFinishVerify(Span<const uint8> tag) noexcept {
    platform::DataIndependentTimingScope dit;
    if (auto active = CheckActive(); !active) {
        return active;
    }

    // 검증은 결과와 관계없이 끝낸다(같은 상태로 다시 시도하지 않는다). tag는 메시지에서 온 byte라
    // 길이가 틀려도 AuthenticationFailed다(2절).
    finished_ = true;
    uint8 computed[AesGcmTagBytes];
    detail::AesGcmStreamFinish(GcmOf(state_), Span<uint8>(computed));
    const bool matched =
        tag.Size() == AesGcmTagBytes && ConstantTimeEquals(Span<const uint8>(computed), tag);
    SecureZero(Span<uint8>(computed));
    failed_ = !matched;
    return matched ? Result<void>::Success()
                   : Result<void>::Failure(ErrorCode::AuthenticationFailed);
}

} // namespace rocks::crypto
