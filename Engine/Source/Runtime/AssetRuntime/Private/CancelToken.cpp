#include <AssetRuntime/CancelToken.h>

#include <Core/Move.h>
#include <Platform/Atomics.h>

namespace rocks {

namespace detail {

// 취소 공유 상태다: 참조 계수(Source·Token 수)와 표시(0·1).
struct CancelState {
    Allocator* allocator = nullptr;
    uint32 references = 1;
    uint32 cancelled = 0;
};

} // namespace detail

namespace {

void RetainCancelState(detail::CancelState* state) noexcept {
    if (state != nullptr) {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&state->references, 1u);
    }
}

void ReleaseCancelState(detail::CancelState* state) noexcept {
    if (state == nullptr) {
        return;
    }

    // 마지막 계수를 놓는 쪽이 지운다(앞선 쓰기가 보이도록 acquire-release).
    if (platform::AtomicFetchSub<platform::MemoryOrder::AcquireRelease>(&state->references, 1u) !=
        1u) {
        return;
    }

    Allocator* allocator = state->allocator;
    state->~CancelState();
    (void)allocator->Deallocate(state);
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
    return state_ != nullptr &&
           platform::AtomicLoad<platform::MemoryOrder::Acquire>(&state_->cancelled) != 0;
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
    auto memory = allocator.Allocate(sizeof(detail::CancelState), alignof(detail::CancelState));
    if (!memory) {
        return Result<CancelSource>::Failure(memory.Error());
    }

    auto* state = InitializeAt<detail::CancelState>(memory.Value());
    state->allocator = &allocator;
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
    return state_ != nullptr &&
           platform::AtomicLoad<platform::MemoryOrder::Acquire>(&state_->cancelled) != 0;
}

} // namespace rocks
