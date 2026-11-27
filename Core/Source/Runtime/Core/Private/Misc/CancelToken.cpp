#include <Core/CancelToken.h>

#include <Core/Move.h>
#include <Platform/Atomics.h>

namespace rocks {

namespace detail {

// 취소 공유 상태다: 참조 계수(Source·Token·연결된 자식 수), 표시(0·1), 연결된 부모(붙잡는다)와
// 사슬 깊이(자기 자신 포함).
struct CancelState {
    Allocator* allocator = nullptr;
    uint32 references = 1;
    uint32 cancelled = 0;
    CancelState* parent = nullptr;
    uint32 depth = 1;
};

} // namespace detail

namespace {

void RetainCancelState(detail::CancelState* state) noexcept {
    if (state != nullptr) {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&state->references, 1u);
    }
}

void ReleaseCancelState(detail::CancelState* state) noexcept {
    // 부모 사슬을 따라 놓는다(마지막 계수를 놓은 상태만 지우고, 그 상태가 붙잡던 부모로 이어 간다 —
    // 재귀 없이).
    while (state != nullptr) {
        // 마지막 계수를 놓는 쪽이 지운다(앞선 쓰기가 보이도록 acquire-release).
        if (platform::AtomicFetchSub<platform::MemoryOrder::AcquireRelease>(&state->references,
                                                                            1u) != 1u) {
            return;
        }

        detail::CancelState* parent = state->parent;
        Allocator* allocator = state->allocator;
        state->~CancelState();
        (void)allocator->Deallocate(state);
        state = parent;
    }
}

bool IsChainCancelled(const detail::CancelState* state) noexcept {
    for (; state != nullptr; state = state->parent) {
        if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&state->cancelled) != 0) {
            return true;
        }
    }

    return false;
}

} // namespace

CancelToken::CancelToken(detail::CancelState* state) noexcept : state_(state) {
    RetainCancelState(state_);
}

CancelToken::CancelToken(const CancelToken& other) noexcept : state_(other.state_) {
    RetainCancelState(state_);
}

CancelToken::CancelToken(CancelToken&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

CancelToken& CancelToken::operator=(const CancelToken& other) noexcept {
    if (this != &other) {
        RetainCancelState(other.state_);
        Release();
        state_ = other.state_;
    }

    return *this;
}

CancelToken& CancelToken::operator=(CancelToken&& other) noexcept {
    if (this != &other) {
        Release();
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

CancelToken::~CancelToken() noexcept {
    Release();
}

void CancelToken::Release() noexcept {
    ReleaseCancelState(state_);
    state_ = nullptr;
}

bool CancelToken::IsCancellationRequested() const noexcept {
    return IsChainCancelled(state_);
}

Result<void> CancelToken::CheckCancelled() const noexcept {
    return IsChainCancelled(state_) ? Result<void>::Failure(ErrorCode::Cancelled)
                                    : Result<void>::Success();
}

CancelSource::CancelSource(CancelSource&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

CancelSource& CancelSource::operator=(CancelSource&& other) noexcept {
    if (this != &other) {
        ReleaseCancelState(state_);
        state_ = other.state_;
        other.state_ = nullptr;
    }

    return *this;
}

CancelSource::~CancelSource() noexcept {
    ReleaseCancelState(state_);
}

Result<CancelSource> CancelSource::Create(Allocator& allocator) noexcept {
    return CreateLinked(allocator, CancelToken());
}

Result<CancelSource> CancelSource::CreateLinked(Allocator& allocator,
                                                const CancelToken& parent) noexcept {
    detail::CancelState* parentState = parent.state_;
    const uint32 depth = parentState != nullptr ? parentState->depth + 1 : 1;
    if (depth > MaxCancelChainDepth) {
        return Result<CancelSource>::Failure(ErrorCode::CapacityExceeded);
    }

    auto memory = allocator.Allocate(sizeof(detail::CancelState), alignof(detail::CancelState));
    if (!memory) {
        return Result<CancelSource>::Failure(memory.Error());
    }

    auto* state = InitializeAt<detail::CancelState>(memory.Value());
    state->allocator = &allocator;
    state->parent = parentState;
    state->depth = depth;
    RetainCancelState(parentState);
    CancelSource source;
    source.state_ = state;
    return Result<CancelSource>::Success(Move(source));
}

CancelToken CancelSource::Token() const noexcept {
    return CancelToken(state_);
}

void CancelSource::Cancel() noexcept {
    if (state_ != nullptr) {
        platform::AtomicStore<platform::MemoryOrder::Release>(&state_->cancelled, 1u);
    }
}

bool CancelSource::IsCancellationRequested() const noexcept {
    return IsChainCancelled(state_);
}

} // namespace rocks
