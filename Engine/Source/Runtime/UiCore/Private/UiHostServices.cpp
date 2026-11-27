#include <UiCore/UiHostServices.h>

namespace rocks::ui {

UiHostServices::UiHostServices(Allocator& allocator) noexcept
    : appStyle_(allocator), teardown_(allocator), frameActions_(allocator) {
}

Result<void> UiHostServices::DeferTeardown(Delegate<void()>&& teardown) noexcept {
    if (!teardown.IsBound()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return teardown_.TryPushBack(Move(teardown));
}

usize UiHostServices::RunDeferredTeardown() noexcept {
    if (runningTeardown_) {
        return 0;
    }

    runningTeardown_ = true;
    usize ran = 0;
    for (usize round = 0; round < MaxTeardownRounds && !teardown_.IsEmpty(); ++round) {
        // 통지가 새 항목을 넣을 수 있으므로 이번 바퀴의 목록을 떼어 돌린다.
        // 옮긴 뒤 teardown_은 같은 allocator의 빈 배열이다.
        Array<Delegate<void()>> batch = Move(teardown_);
        for (auto& item : batch) {
            item.ExecuteIfBound();
            ++ran;
        }
    }

    runningTeardown_ = false;
    return ran;
}

usize UiHostServices::PendingTeardownCount() const noexcept {
    return teardown_.Size();
}

usize UiHostServices::DiscardDeferredTeardown() noexcept {
    const usize count = teardown_.Size();
    teardown_.Clear();
    return count;
}

Result<void> UiHostServices::DeferFrameAction(Delegate<void()>&& action) noexcept {
    if (!action.IsBound()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return frameActions_.TryPushBack(Move(action));
}

usize UiHostServices::RunFrameActions() noexcept {
    if (runningFrameActions_) {
        return 0;
    }

    runningFrameActions_ = true;
    // 도는 중 넣은 항목은 다음 프레임 몫이다 — 이번 목록을 떼어 돌린다(옮긴 뒤 frameActions_은 같은
    // allocator의 빈 배열이다).
    Array<Delegate<void()>> batch = Move(frameActions_);
    for (auto& item : batch) {
        item.ExecuteIfBound();
    }

    runningFrameActions_ = false;
    return batch.Size();
}

usize UiHostServices::PendingFrameActionCount() const noexcept {
    return frameActions_.Size();
}

usize UiHostServices::DiscardFrameActions() noexcept {
    const usize count = frameActions_.Size();
    frameActions_.Clear();
    ++frameActionEpoch_;
    return count;
}

uint64 UiHostServices::FrameActionEpoch() const noexcept {
    return frameActionEpoch_;
}

void UiHostServices::SetOwner(const void* kind, void* owner) noexcept {
    ownerKind_ = owner ? kind : nullptr;
    owner_ = kind ? owner : nullptr;
}

void* UiHostServices::FindOwner(const void* kind) const noexcept {
    return kind && kind == ownerKind_ ? owner_ : nullptr;
}

void UiHostServices::SetPopupWorkArea(PopupWorkArea&& provider) noexcept {
    popupWorkArea_ = Move(provider);
}

bool UiHostServices::QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept {
    return popupWorkArea_.IsBound() && popupWorkArea_.Execute(window, area);
}

void UiHostServices::SetTextInputService(SharedPtr<TextInputService> service) noexcept {
    textInput_ = Move(service);
}

const SharedPtr<TextInputService>& UiHostServices::TextInput() const noexcept {
    return textInput_;
}

void UiHostServices::SetFrameTime(const FrameTime& time) noexcept {
    time_ = time;
}

const FrameTime& UiHostServices::Time() const noexcept {
    return time_;
}

Result<void> UiHostServices::SetStyles(SharedPtr<StyleRegistry> registry,
                                       StringView appStyle) noexcept {
    if (auto assigned = appStyle_.TryAssign(appStyle); !assigned) {
        return assigned;
    }

    // 빠지는 등록부의 revision을 접어 넣어 합이 줄지 않게 한다.
    styleChanges_ += 1 + (registry_ ? registry_->Revision() : 0);
    registry_ = Move(registry);
    return Result<void>::Success();
}

uint64 UiHostServices::StyleRevision() const noexcept {
    return styleChanges_ + (registry_ ? registry_->Revision() : 0);
}

} // namespace rocks::ui
