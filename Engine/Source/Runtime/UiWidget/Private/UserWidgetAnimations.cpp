#include "UserWidgetAnimationSet.h"

#include <UiWidget/UserWidget.h>

namespace rocks::ui {

UserWidgetAnimationSet::UserWidgetAnimationSet(Allocator& allocator) noexcept : players(allocator) {
}

void UserWidgetAnimationSet::Remove(const UserWidgetAnimation* player) noexcept {
    for (usize i = 0; i < players.Size(); ++i) {
        if (players[i].Get() == player) {
            (void)players.TryRemoveAt(i);
            return;
        }
    }
}

Result<SharedPtr<UserWidgetAnimation>>
UserWidget::StartAnimation(ObjectRegistry& objects, Application& app, const WidgetAnimation& clip,
                           WidgetAnimationPlayback&& options, bool queued) noexcept {
    if (!CachedElement()) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(ErrorCode::NotReady);
    }
    auto& allocator = objects.ObjectAllocator();
    if (!animationPlayers_) {
        auto set = SharedPtr<UserWidgetAnimationSet>::TryMake(allocator, allocator);
        if (!set) {
            return Result<SharedPtr<UserWidgetAnimation>>::Failure(set.Error());
        }
        animationPlayers_ = Move(set.Value());
    }
    auto made = SharedPtr<UserWidgetAnimation>::TryMake(allocator, allocator);
    if (!made) {
        return made;
    }
    auto& player = *made.Value();
    auto bound = player.Bind(objects, app, *this, clip, Move(options), made.Value());
    if (!bound) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(bound.Error());
    }
    bound = player.Schedule();
    if (!bound) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(bound.Error());
    }
    const auto active = animationPlayers_;
    bound = active->players.TryPushBack(made.Value());
    if (!bound) {
        return Result<SharedPtr<UserWidgetAnimation>>::Failure(bound.Error());
    }
    player.set_ = WeakPtr<UserWidgetAnimationSet>(active);
    if (!queued) {
        bound = player.Advance(0);
        if (!bound) {
            active->Remove(&player);
            return Result<SharedPtr<UserWidgetAnimation>>::Failure(bound.Error());
        }
    }
    return made;
}

Result<SharedPtr<UserWidgetAnimation>>
UserWidget::PlayAnimation(ObjectRegistry& objects, Application& app, const WidgetAnimation& clip,
                          WidgetAnimationPlayback options) noexcept {
    return StartAnimation(objects, app, clip, Move(options), false);
}

Result<SharedPtr<UserWidgetAnimation>>
UserWidget::QueuePlayAnimation(ObjectRegistry& objects, Application& app,
                               const WidgetAnimation& clip,
                               WidgetAnimationPlayback options) noexcept {
    return StartAnimation(objects, app, clip, Move(options), true);
}

void UserWidget::StopAllAnimations() noexcept {
    auto set = Move(animationPlayers_);
    if (!set) {
        return;
    }
    while (!set->players.IsEmpty()) {
        const usize last = set->players.Size() - 1;
        auto player = set->players[last];
        (void)set->players.TryRemoveAt(last);
        player->Stop();
    }
}

bool UserWidget::IsAnyAnimationPlaying() const noexcept {
    if (animationPlayers_) {
        for (const auto& player : animationPlayers_->players) {
            if (player->Status() == WidgetAnimationStatus::Playing ||
                player->Status() == WidgetAnimationStatus::Queued) {
                return true;
            }
        }
    }
    return false;
}

} // namespace rocks::ui
