#include <Crypto/SegmentStream.h>

#include <Crypto/Bytes.h>
#include <Crypto/Random.h>

#include "KeyAccess.h"
#include "SegmentStreamInternal.h"

#include <Core/Move.h>
#include <Platform/DataIndependentTiming.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// 순차 세그먼트 암·복호(SegmentStreamEncryptor·SegmentStreamDecryptor)다. 상태와 버퍼는 호출자가 준
// 할당기에 두고, 세그먼트 키는 키 저장소의 AeadKey다. 형식 규칙은 SegmentFormat.cpp에 있다.
namespace rocks::crypto {
namespace {

using EncryptorState = detail::SegmentEncryptorState;
using DecryptorState = detail::SegmentDecryptorState;

constexpr usize BufferAlignment = 64;

usize Smaller(usize left, usize right) noexcept {
    return left < right ? left : right;
}

// 할당기에서 T 하나를 만든다. 실패하면 할당기의 오류다.
template <typename T>
Result<T*> TryNewState(Allocator& allocator) noexcept {
    auto memory = allocator.Allocate(sizeof(T), alignof(T));
    if (!memory) {
        return Result<T*>::Failure(memory.Error());
    }

    return Result<T*>::Success(InitializeAt<T>(memory.Value()));
}

// 버퍼를 지우고 돌려준다. 해제 실패는 버퍼가 그 할당기 것이 아니라는 결함이다.
void FreeBuffer(Allocator& allocator, uint8*& buffer, usize bytes) noexcept {
    if (buffer == nullptr) {
        return;
    }

    SecureZero(Span<uint8>(buffer, bytes));
    if (!allocator.Deallocate(buffer)) {
        platform::Fatal("Crypto: segment buffer was not owned by its allocator");
    }

    buffer = nullptr;
}

template <typename T>
void SegmentStreamSequentialFreeState(T* state) noexcept {
    Allocator* allocator = state->allocator;
    DestroyAt(state);
    if (!allocator->Deallocate(state)) {
        platform::Fatal("Crypto: segment stream state was not owned by its allocator");
    }
}

usize EncryptorBufferBytes(usize segmentBytes) noexcept {
    return segmentBytes - SegmentTagBytes + segmentBytes;
}

// 내보내지 않은 출력을 output 앞에 복사하고 그 수를 돌려준다.
usize Drain(const uint8* source, usize& begin, usize end, Span<uint8> output) noexcept {
    const usize bytes = Smaller(end - begin, output.Size());
    if (bytes != 0) {
        platform::MemoryCopy(output.Data(), source + begin, bytes);
        begin += bytes;
    }

    return bytes;
}

// 모은 평문을 세그먼트 하나로 봉인해 staged에 둔다. staged가 비어 있어야 한다.
void SealBuffered(EncryptorState& state, bool last) noexcept {
    const usize bytes = state.plaintextFill;
    const Span<uint8> plaintext = state.Plaintext().Left(bytes);
    detail::SealSegment(state.key, state.index, last, plaintext,
                        state.Staged().Left(bytes + SegmentTagBytes));
    SecureZero(plaintext);
    state.plaintextFill = 0;
    state.stagedBegin = 0;
    state.stagedEnd = bytes + SegmentTagBytes;
    if (!last) {
        ++state.index;
    }
}

void DestroyEncryptor(void*& memory) noexcept {
    if (memory == nullptr) {
        return;
    }

    auto* state = static_cast<EncryptorState*>(memory);
    FreeBuffer(*state->allocator, state->buffer, EncryptorBufferBytes(state->segmentBytes));
    SegmentStreamSequentialFreeState(state);
    memory = nullptr;
}

void DestroyDecryptor(void*& memory) noexcept {
    if (memory == nullptr) {
        return;
    }

    auto* state = static_cast<DecryptorState*>(memory);
    FreeBuffer(*state->allocator, state->buffer, state->segmentBytes);
    SegmentStreamSequentialFreeState(state);
    memory = nullptr;
}

// 복호기를 sticky 실패로 만든다. 버퍼와 이번 호출의 출력 전체를 지운다.
Result<StreamProgress> Fail(DecryptorState& state, ErrorCode error, Span<uint8> output) noexcept {
    state.failed = true;
    state.failure = error;
    state.pendingBegin = 0;
    state.pendingEnd = 0;
    state.master.Reset();
    if (state.buffer != nullptr) {
        SecureZero(Span<uint8>(state.buffer, state.segmentBytes));
    }

    SecureZero(output);
    return Result<StreamProgress>::Failure(error);
}

// 다 모인 헤더를 확인하고 세그먼트 키와 버퍼를 만든다.
Result<void> TryAcceptHeader(DecryptorState& state) noexcept {
    auto key = detail::TryOpenSegmentStream(state.master.View(), state.keyId,
                                            Span<const uint8>(state.associatedHash),
                                            Span<const uint8>(state.header), state.params);
    if (!key) {
        return Result<void>::Failure(key.Error());
    }

    const usize segmentBytes = usize{1} << state.params.segmentSizeLog2;
    auto buffer = state.allocator->Allocate(segmentBytes, BufferAlignment);
    if (!buffer) {
        return Result<void>::Failure(buffer.Error());
    }

    state.key = Move(key.Value());
    state.buffer = static_cast<uint8*>(buffer.Value());
    state.segmentBytes = segmentBytes;
    state.master.Reset();
    return Result<void>::Success();
}

bool IsAes(const DecryptorState& state) noexcept {
    return state.params.algorithm == SegmentStreamAlgorithm::Aes256Gcm;
}

} // namespace

// ---- 생성 경로 ----

Result<SegmentStreamEncryptor> detail::SegmentStreamFactory::TryCreateEncryptor(
    const SegmentStreamKey& key, const SegmentStreamParams& params,
    Span<const uint8> associatedData, Span<const uint8> salt, Allocator& bufferAllocator) noexcept {
    platform::DataIndependentTimingScope dit;
    uint8 header[SegmentHeaderBytes];
    auto segmentKey = TryStartSegmentStream(key, params, associatedData, salt, Span<uint8>(header));
    if (!segmentKey) {
        return Result<SegmentStreamEncryptor>::Failure(segmentKey.Error());
    }

    const usize segmentBytes = usize{1} << params.segmentSizeLog2;
    auto state = TryNewState<EncryptorState>(bufferAllocator);
    if (!state) {
        return Result<SegmentStreamEncryptor>::Failure(state.Error());
    }

    EncryptorState& created = *state.Value();
    created.allocator = &bufferAllocator;
    auto buffer = bufferAllocator.Allocate(EncryptorBufferBytes(segmentBytes), BufferAlignment);
    if (!buffer) {
        SegmentStreamSequentialFreeState(state.Value());
        return Result<SegmentStreamEncryptor>::Failure(buffer.Error());
    }

    created.key = Move(segmentKey.Value());
    created.buffer = static_cast<uint8*>(buffer.Value());
    created.segmentBytes = segmentBytes;
    created.maxPlaintextBytes = SegmentMaxPlaintextBytes(params);
    platform::MemoryCopy(created.Staged().Data(), header, SegmentHeaderBytes);
    created.stagedEnd = SegmentHeaderBytes;

    SegmentStreamEncryptor encryptor;
    encryptor.state_ = state.Value();
    return Result<SegmentStreamEncryptor>::Success(Move(encryptor));
}

// ---- SegmentStreamEncryptor ----

SegmentStreamEncryptor::~SegmentStreamEncryptor() noexcept {
    Reset();
}

SegmentStreamEncryptor::SegmentStreamEncryptor(SegmentStreamEncryptor&& other) noexcept
    : state_(other.state_) {
    other.state_ = nullptr;
}

SegmentStreamEncryptor& SegmentStreamEncryptor::operator=(SegmentStreamEncryptor&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

void SegmentStreamEncryptor::Reset() noexcept {
    DestroyEncryptor(state_);
}

Result<SegmentStreamEncryptor>
SegmentStreamEncryptor::TryCreate(const SegmentStreamKey& key, const SegmentStreamParams& params,
                                  Span<const uint8> associatedData,
                                  Allocator& bufferAllocator) noexcept {
    platform::DataIndependentTimingScope dit;
    uint8 salt[detail::SegmentSaltBytes] = {};
    if (!key.IsEmpty() && detail::IsValidSegmentParams(params)) {
        if (auto filled = TryFillRandom(Span<uint8>(salt)); !filled) {
            return Result<SegmentStreamEncryptor>::Failure(filled.Error());
        }
    }

    return detail::SegmentStreamFactory::TryCreateEncryptor(
        key, params, associatedData, Span<const uint8>(salt), bufferAllocator);
}

Result<StreamProgress> SegmentStreamEncryptor::TryProcess(Span<const uint8> plaintext,
                                                          Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }

    EncryptorState& state = *static_cast<EncryptorState*>(state_);
    if (state.finishing || Overlaps(plaintext, output)) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    if (static_cast<uint64>(plaintext.Size()) > state.maxPlaintextBytes - state.plaintextBytes) {
        return Result<StreamProgress>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize capacity = state.segmentBytes - SegmentTagBytes;
    StreamProgress progress;
    for (;;) {
        progress.produced += Drain(state.Staged().Data(), state.stagedBegin, state.stagedEnd,
                                   output.RightChop(progress.produced));
        if (progress.consumed == plaintext.Size()) {
            break;
        }

        if (state.plaintextFill < capacity) {
            const usize bytes =
                Smaller(plaintext.Size() - progress.consumed, capacity - state.plaintextFill);
            platform::MemoryCopy(state.Plaintext().Data() + state.plaintextFill,
                                 plaintext.Data() + progress.consumed, bytes);
            state.plaintextFill += bytes;
            state.plaintextBytes += bytes;
            progress.consumed += bytes;
            continue;
        }

        // 꽉 찬 세그먼트 뒤에 입력이 더 있으므로 마지막이 아니다. 출력이 밀려 있으면(역압) 멈춘다.
        if (state.stagedBegin != state.stagedEnd) {
            break;
        }

        SealBuffered(state, false);
    }

    progress.complete =
        progress.consumed == plaintext.Size() && state.stagedBegin == state.stagedEnd;
    return Result<StreamProgress>::Success(progress);
}

Result<StreamProgress> SegmentStreamEncryptor::TryFinish(Span<uint8> output) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }

    EncryptorState& state = *static_cast<EncryptorState*>(state_);
    StreamProgress progress;
    if (state.finished) {
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }

    state.finishing = true;
    for (;;) {
        progress.produced += Drain(state.Staged().Data(), state.stagedBegin, state.stagedEnd,
                                   output.RightChop(progress.produced));
        if (state.stagedBegin != state.stagedEnd) {
            return Result<StreamProgress>::Success(progress);
        }

        if (!state.lastSealed) {
            SealBuffered(state, true);
            state.lastSealed = true;
            continue;
        }

        state.finished = true;
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }
}

// ---- SegmentStreamDecryptor ----

SegmentStreamDecryptor::~SegmentStreamDecryptor() noexcept {
    Reset();
}

SegmentStreamDecryptor::SegmentStreamDecryptor(SegmentStreamDecryptor&& other) noexcept
    : state_(other.state_) {
    other.state_ = nullptr;
}

SegmentStreamDecryptor& SegmentStreamDecryptor::operator=(SegmentStreamDecryptor&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

void SegmentStreamDecryptor::Reset() noexcept {
    DestroyDecryptor(state_);
}

Result<SegmentStreamDecryptor>
SegmentStreamDecryptor::TryCreate(const SegmentStreamKey& key, Span<const uint8> associatedData,
                                  Allocator& bufferAllocator) noexcept {
    platform::DataIndependentTimingScope dit;
    if (key.IsEmpty()) {
        return Result<SegmentStreamDecryptor>::Failure(ErrorCode::StaleHandle);
    }

    auto master = SecretBytes::TryCopyFrom(detail::KeyAccess::Bytes(key));
    if (!master) {
        return Result<SegmentStreamDecryptor>::Failure(master.Error());
    }

    auto state = TryNewState<DecryptorState>(bufferAllocator);
    if (!state) {
        return Result<SegmentStreamDecryptor>::Failure(state.Error());
    }

    DecryptorState& created = *state.Value();
    created.allocator = &bufferAllocator;
    created.master = Move(master.Value());
    created.keyId = key.Id();
    detail::HashSegmentAssociatedData(associatedData, Span<uint8>(created.associatedHash));

    SegmentStreamDecryptor decryptor;
    decryptor.state_ = state.Value();
    return Result<SegmentStreamDecryptor>::Success(Move(decryptor));
}

Result<StreamProgress> SegmentStreamDecryptor::TryProcess(Span<const uint8> ciphertext,
                                                          Span<uint8> plaintext) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }

    DecryptorState& state = *static_cast<DecryptorState*>(state_);
    if (Overlaps(ciphertext, plaintext)) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    if (state.failed) {
        SecureZero(plaintext);
        return Result<StreamProgress>::Failure(state.failure);
    }

    if (state.finishing) {
        return Result<StreamProgress>::Failure(ErrorCode::InvalidArgument);
    }

    StreamProgress progress;
    for (;;) {
        progress.produced += Drain(state.buffer, state.pendingBegin, state.pendingEnd,
                                   plaintext.RightChop(progress.produced));
        if (state.pendingBegin != state.pendingEnd || progress.consumed == ciphertext.Size()) {
            break;
        }

        const usize remaining = ciphertext.Size() - progress.consumed;
        if (state.headerFill < SegmentHeaderBytes) {
            const usize bytes = Smaller(remaining, SegmentHeaderBytes - state.headerFill);
            platform::MemoryCopy(state.header + state.headerFill,
                                 ciphertext.Data() + progress.consumed, bytes);
            state.headerFill += bytes;
            progress.consumed += bytes;
            if (state.headerFill == SegmentHeaderBytes) {
                if (auto accepted = TryAcceptHeader(state); !accepted) {
                    return Fail(state, accepted.Error(), plaintext);
                }
            }

            continue;
        }

        if (state.bufferFill < state.segmentBytes) {
            const usize bytes = Smaller(remaining, state.segmentBytes - state.bufferFill);
            platform::MemoryCopy(state.buffer + state.bufferFill,
                                 ciphertext.Data() + progress.consumed, bytes);
            state.bufferFill += bytes;
            progress.consumed += bytes;
            continue;
        }

        // 꽉 찬 세그먼트 뒤에 byte가 더 왔으므로 last = 0으로 연다. 그 뒤에 마지막 세그먼트(17 byte
        // 이상)가 와야 하므로 세그먼트 수·AES 한도를 넘으면 열기 전에 거부한다.
        const uint64 segmentBytes = state.segmentBytes;
        if (state.index >= SegmentStreamMaxSegments - 1 ||
            (IsAes(state) &&
             state.bodyBytes + segmentBytes + SegmentTagBytes + 1 > SegmentStreamAesMaxBodyBytes)) {
            return Fail(state, ErrorCode::AuthenticationFailed, plaintext);
        }

        const Span<uint8> segment(state.buffer, state.segmentBytes);
        if (!detail::OpenSegment(state.key, state.index, false, segment,
                                 segment.Left(state.segmentBytes - SegmentTagBytes))) {
            return Fail(state, ErrorCode::AuthenticationFailed, plaintext);
        }

        ++state.index;
        state.bodyBytes += segmentBytes;
        state.bufferFill = 0;
        state.pendingBegin = 0;
        state.pendingEnd = state.segmentBytes - SegmentTagBytes;
    }

    progress.complete =
        progress.consumed == ciphertext.Size() && state.pendingBegin == state.pendingEnd;
    return Result<StreamProgress>::Success(progress);
}

Result<StreamProgress> SegmentStreamDecryptor::TryFinish(Span<uint8> plaintext) noexcept {
    platform::DataIndependentTimingScope dit;
    if (state_ == nullptr) {
        return Result<StreamProgress>::Failure(ErrorCode::StaleHandle);
    }

    DecryptorState& state = *static_cast<DecryptorState*>(state_);
    if (state.failed) {
        SecureZero(plaintext);
        return Result<StreamProgress>::Failure(state.failure);
    }

    StreamProgress progress;
    if (state.finished) {
        progress.complete = true;
        return Result<StreamProgress>::Success(progress);
    }

    state.finishing = true;
    for (;;) {
        progress.produced += Drain(state.buffer, state.pendingBegin, state.pendingEnd,
                                   plaintext.RightChop(progress.produced));
        if (state.pendingBegin != state.pendingEnd) {
            return Result<StreamProgress>::Success(progress);
        }

        if (state.lastOpened) {
            state.finished = true;
            progress.complete = true;
            return Result<StreamProgress>::Success(progress);
        }

        // 헤더가 끝나지 않았으면 절단이다. 받은 앞부분으로 버전을 판정할 수 있으면 Unsupported다.
        if (state.headerFill < SegmentHeaderBytes) {
            auto info = TryPeekSegmentHeader(Span<const uint8>(state.header, state.headerFill));
            return Fail(state, info.Error(), plaintext);
        }

        const usize last = state.bufferFill;
        if (last < SegmentTagBytes || (state.index != 0 && last == SegmentTagBytes) ||
            (IsAes(state) && state.bodyBytes + last > SegmentStreamAesMaxBodyBytes)) {
            return Fail(state, ErrorCode::AuthenticationFailed, plaintext);
        }

        const Span<uint8> segment(state.buffer, last);
        if (!detail::OpenSegment(state.key, state.index, true, segment,
                                 segment.Left(last - SegmentTagBytes))) {
            return Fail(state, ErrorCode::AuthenticationFailed, plaintext);
        }

        state.lastOpened = true;
        state.bufferFill = 0;
        state.pendingBegin = 0;
        state.pendingEnd = last - SegmentTagBytes;
    }
}

} // namespace rocks::crypto
