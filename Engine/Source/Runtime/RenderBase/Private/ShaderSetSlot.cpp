#include <RenderBase/ShaderSetSlot.h>

#include <Core/Assert.h>
#include <stdio.h>

// 셰이더 집합 교체 자리다(Docs/Design/RenderBase 9.8절). 규칙은 공개 헤더 주석을 따른다.
namespace rocks::render {

ShaderSetSlot::ShaderSetSlot(Allocator& allocator) noexcept : retired_(allocator) {
}

ShaderSetSlot::~ShaderSetSlot() noexcept {
    ROCKS_ASSERT(current_.Get() == nullptr && retired_.IsEmpty());
}

void ShaderSetSlot::RejectLocked(const ShaderSet& set, const char* reason) noexcept {
    ++rejections_;
    const StringView detail = set.ProblemDetail();
    snprintf(rejection_, sizeof(rejection_), "%s: %.*s", reason,
             static_cast<int>(detail.ByteCount()), detail.Data());
}

Result<void> ShaderSetSlot::Install(UniquePtr<ShaderSet>&& set) noexcept {
    if (set.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (current_.Get() != nullptr) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (set->State() != ShaderSetState::Ready) {
        RejectLocked(*set, "the first shader set is not ready");
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    current_ = Move(set);
    return Result<void>::Success();
}

Result<void> ShaderSetSlot::Propose(UniquePtr<ShaderSet>&& candidate) noexcept {
    if (candidate.Get() == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const ShaderSetState state = candidate->State();
    platform::LockGuard lock(gate_);
    if (state != ShaderSetState::Ready && state != ShaderSetState::WaitingForProvider) {
        RejectLocked(*candidate, "the replacement shader set failed its checks");
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    // 옛 후보는 아직 쓰인 적이 없으므로(lease 없음) 바로 버린다. 공급 대기 중인 후보는 공급자가
    // 끝날 때까지 기다린 뒤 버린다(CompleteProvision이 파괴된 집합을 만지지 않게).
    if (pending_.Get() != nullptr) {
        (void)pending_->WaitUntilSettled();
        pending_.Reset();
    }

    pending_ = Move(candidate);
    return Result<void>::Success();
}

bool ShaderSetSlot::ApplyPending(PipelineCache* cache) noexcept {
    UniquePtr<ShaderSet> previous;
    {
        platform::LockGuard lock(gate_);
        if (pending_.Get() == nullptr) {
            return false;
        }

        const ShaderSetState state = pending_->State();
        if (state == ShaderSetState::WaitingForProvider) {
            return false;
        }

        if (state != ShaderSetState::Ready) {
            RejectLocked(*pending_, "the replacement shader set failed its checks");
            pending_.Reset();
            return false;
        }

        previous = Move(current_);
        current_ = Move(pending_);
        ++swaps_;
        if (previous.Get() == nullptr) {
            return true;
        }

        if (!retired_.TryReserve(retired_.Size() + 1)) {
            // 목록에 넣지 못하면 교체를 되돌린다(옛 집합을 잃지 않는다 — 다음 경계에서 다시 한다).
            pending_ = Move(current_);
            current_ = Move(previous);
            --swaps_;
            return false;
        }
    }

    // 옛 집합의 파이프라인 캐시 항목과 장치 셰이더를 회수한다(epoch가 달라 새 항목과 섞이지
    // 않는다).
    if (cache != nullptr) {
        cache->RetireSet(*previous);
    }

    previous->Retire();
    platform::LockGuard lock(gate_);
    (void)retired_.TryPushBack(Move(previous));
    return true;
}

usize ShaderSetSlot::CollectRetired() noexcept {
    platform::LockGuard lock(gate_);
    usize destroyed = 0;
    for (usize index = 0; index < retired_.Size();) {
        if (retired_[index]->LeaseCount() == 0) {
            (void)retired_.TryRemoveAt(index);
            ++destroyed;
        } else {
            ++index;
        }
    }

    return destroyed;
}

Result<void> ShaderSetSlot::Shutdown(PipelineCache* cache) noexcept {
    UniquePtr<ShaderSet> current;
    {
        platform::LockGuard lock(gate_);
        if (pending_.Get() != nullptr) {
            (void)pending_->WaitUntilSettled();
            pending_.Reset();
        }

        current = Move(current_);
        if (current.Get() != nullptr && !retired_.TryReserve(retired_.Size() + 1)) {
            current_ = Move(current);
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    if (current.Get() != nullptr) {
        if (cache != nullptr) {
            cache->RetireSet(*current);
        }

        current->Retire();
        platform::LockGuard lock(gate_);
        (void)retired_.TryPushBack(Move(current));
    }

    (void)CollectRetired();
    return RetiredCount() == 0 ? Result<void>::Success() : Result<void>::Failure(ErrorCode::Busy);
}

void ShaderSetSlot::Abandon() noexcept {
    platform::LockGuard lock(gate_);
    if (pending_.Get() != nullptr) {
        (void)pending_->WaitUntilSettled();
        pending_.Reset();
    }

    current_.Reset();
    retired_.Clear();
}

ShaderSet* ShaderSetSlot::Current() const noexcept {
    platform::LockGuard lock(gate_);
    return current_.Get();
}

uint64 ShaderSetSlot::Swaps() const noexcept {
    platform::LockGuard lock(gate_);
    return swaps_;
}

uint64 ShaderSetSlot::Rejections() const noexcept {
    platform::LockGuard lock(gate_);
    return rejections_;
}

usize ShaderSetSlot::RetiredCount() const noexcept {
    platform::LockGuard lock(gate_);
    return retired_.Size();
}

StringView ShaderSetSlot::LastRejection() const noexcept {
    platform::LockGuard lock(gate_);
    return StringView(rejection_);
}

} // namespace rocks::render
