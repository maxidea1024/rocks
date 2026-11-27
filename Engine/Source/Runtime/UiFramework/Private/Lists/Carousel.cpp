#include <UiFramework/Lists/Carousel.h>

namespace rocks::ui {

Carousel::Carousel(Allocator& allocator) noexcept : Switcher(allocator), progress_(*this, 1.f) {
    SetClipToBounds(true);
}

Result<void> Carousel::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

Result<void> Carousel::AddPage(const SharedPtr<CarouselPage>& page) noexcept {
    if (!page || page->Parent()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (cache_ || ChildCount() == 0) {
        auto ready = page->Materialize();
        if (!ready) {
            return ready;
        }
    }

    return Element::TryAddChild(page);
}

Result<void> Carousel::SetCacheChildren(bool cache) noexcept {
    if (cache == cache_) {
        return Result<void>::Success();
    }

    for (usize i = 0; i < ChildCount(); ++i) {
        auto& page = StaticElementCast<CarouselPage>(*ChildAt(i));
        if (cache) {
            if (auto made = page.Materialize(); !made) {
                for (usize j = 0; j < i; ++j) {
                    if (ChildAt(j) != ActiveChild() && ChildAt(j) != sliding_[0].Get()) {
                        StaticElementCast<CarouselPage>(*ChildAt(j)).ReleaseContent();
                    }
                }

                return made;
            }
        } else if (&page != ActiveChild() && &page != sliding_[0].Get()) {
            page.ReleaseContent();
        }
    }

    cache_ = cache;
    return Result<void>::Success();
}

float Carousel::Progress() const noexcept {
    // 프레임 시각이다(20.3절 — 속성 갱신·배치에서 읽어도 그 프레임 안에서는 같은 값).
    const double elapsed = math::Max(0.0, FindFrameTime().current - start_);
    return static_cast<float>(math::Min(1.0, elapsed * speed_));
}

void Carousel::Finish() noexcept {
    auto previous = Move(sliding_[0]);
    sliding_[1] = {};
    progress_.Set(1);
    if (previous && previous.Get() != ActiveChild() && !cache_) {
        StaticElementCast<CarouselPage>(*previous).ReleaseContent();
    }

    if (previous) {
        InvalidateLayout();
    }
}

Result<void> Carousel::Show(usize index, bool forward, float speed) noexcept {
    if (index >= ChildCount() || !math::IsFinite(speed) || speed < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (ActiveChild() != ChildAt(index)) {
        Finish();
    }

    auto next = ChildRefAt(index);
    if (auto ready = StaticElementCast<CarouselPage>(*next).Materialize(); !ready) {
        return ready;
    }

    if (ActiveChild() == next.Get()) {
        return Result<void>::Success();
    }

    auto previous = FindChild(ActiveChild());
    auto changed = SetActiveIndex(index);
    if (!changed) {
        return changed;
    }

    sliding_[0] = Move(previous);
    sliding_[1] = Move(next);
    if (speed > 0 && sliding_[0]) {
        start_ = FindFrameTime().current;
        speed_ = speed;
        direction_ = forward ? 1.f : -1.f;
        progress_.Set(0);
        progress_.Assign(Delegate<float()>::CreateRaw(this, &Carousel::Progress));
    } else {
        Finish();
    }

    return Result<void>::Success();
}

bool Carousel::RemoveChild(Element& page) noexcept {
    if (page.Parent() != this) {
        return false;
    }

    const bool removed = Switcher::RemoveChild(page);
    Finish();
    return removed;
}

Span<const SharedPtr<Element>> Carousel::PaintChildren() const noexcept {
    return sliding_[0] ? Span<const SharedPtr<Element>>(sliding_, 2) : ActiveChildren();
}

Result<void> Carousel::Arrange(const Geometry& geometry) noexcept {
    if (!sliding_[0]) {
        return Switcher::Arrange(geometry);
    }

    const float progress = progress_.Get();
    if (progress >= 1) {
        Finish();
        return Switcher::Arrange(geometry);
    }

    SetGeometry(geometry);
    const float distance = direction_ * geometry.size.x;
    for (usize i = 0; i < 2; ++i) {
        const float x = (static_cast<float>(i) - progress) * distance;
        auto result = sliding_[i]->ArrangeIfNeeded(
            GetGeometry().MakeChild(geometry.size, {1, 0, 0, 1, x, 0}, {}, {}));
        if (!result) {
            return result;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::ui
