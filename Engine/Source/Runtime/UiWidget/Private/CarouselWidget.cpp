#include <UiWidget/CarouselWidget.generated.h>
#include <UiWidget/WidgetSlot.generated.h>
#include <UiWidget/Widget.generated.h>
#include <UiFramework/Lists/Carousel.h>
#include "MakeWidgetElement.h"

namespace rocks::ui {

CarouselWidget::CarouselWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), pagesChanged_(allocator) {
}

uint32 CarouselWidget::ActiveIndex() const noexcept {
    return activeIndex_;
}

float CarouselWidget::MoveSpeed() const noexcept {
    return moveSpeed_;
}

bool CarouselWidget::CacheChildren() const noexcept {
    return cacheChildren_;
}

MulticastDelegate<void()>& CarouselWidget::OnPagesChanged() noexcept {
    return pagesChanged_;
}

Result<void> CarouselWidget::SetMoveSpeed(float speed) noexcept {
    if (!math::IsFinite(speed) || speed < 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    moveSpeed_ = speed;
    return Result<void>::Success();
}

Result<void> CarouselWidget::SetCacheChildren(bool cache) noexcept {
    if (auto live = CachedElement()) {
        auto changed = StaticElementCast<Carousel>(*live).SetCacheChildren(cache);
        if (!changed) {
            return changed;
        }
    }

    cacheChildren_ = cache;
    return Result<void>::Success();
}

ObjectHandle CarouselWidget::WidgetAt(ObjectRegistry& objects, usize index) const noexcept {
    auto slot = objects.Resolve<WidgetSlot>(objects.OwnerAccess(), SlotAt(index));
    return slot ? slot.Value().Get().Value()->Content() : ObjectHandle{};
}

Result<void> CarouselWidget::SetActiveWidget(ObjectRegistry& objects, ObjectHandle page) noexcept {
    if (!page.IsNull()) {
        for (usize i = 0; i < ChildCount(); ++i) {
            if (WidgetAt(objects, i) == page) {
                return SetActiveIndex(static_cast<uint32>(i));
            }
        }
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<void> CarouselWidget::ChangePage(uint32 index, bool forward) noexcept {
    if (index >= ChildCount()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto changed = StaticElementCast<Carousel>(*live).Show(index, forward, moveSpeed_);
        if (!changed) {
            return changed;
        }
    }

    if (index != activeIndex_) {
        activeIndex_ = index;
        pagesChanged_.Broadcast();
    }

    return Result<void>::Success();
}

Result<void> CarouselWidget::SetActiveIndex(uint32 index) noexcept {
    return ChangePage(index, index > activeIndex_);
}

Result<void> CarouselWidget::NextPage() noexcept {
    return ChildCount() < 2
               ? Result<void>::Success()
               : ChangePage(activeIndex_ + 1 < ChildCount() ? activeIndex_ + 1 : 0, true);
}

Result<void> CarouselWidget::PreviousPage() noexcept {
    return ChildCount() < 2 ? Result<void>::Success()
                            : ChangePage(activeIndex_ > 0 ? activeIndex_ - 1
                                                          : static_cast<uint32>(ChildCount() - 1),
                                         false);
}

Result<void> CarouselWidget::BeginAutoScrolling(Application& app, float interval) noexcept {
    auto live = CachedElement();
    if (!live) {
        return Result<void>::Failure(ErrorCode::NotReady);
    }

    auto timer = app.StartTimer(
        live, interval, Delegate<void()>::CreateRaw(this, &CarouselWidget::AdvanceAutoPage));
    if (!timer) {
        return Result<void>::Failure(timer.Error());
    }

    autoTimer_ = Move(timer.Value());
    return Result<void>::Success();
}

void CarouselWidget::AdvanceAutoPage() noexcept {
    (void)PreviousPage();
}

void CarouselWidget::EndAutoScrolling() noexcept {
    autoTimer_.Reset();
}

Result<ObjectHandle> CarouselWidget::AddChild(ObjectRegistry& objects, ObjectHandle page) noexcept {
    auto added = PanelWidget::AddChild(objects, page);
    if (added) {
        pagesChanged_.Broadcast();
    }

    return added;
}

Result<void> CarouselWidget::RemoveChild(ObjectRegistry& objects, ObjectHandle page) noexcept {
    usize index = 0;
    while (index < ChildCount() && WidgetAt(objects, index) != page) {
        ++index;
    }

    if (index == ChildCount()) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    auto live = CachedElement();
    auto removed = live ? live->ChildRefAt(index) : SharedPtr<Element>();
    // 비캐시 모드의 다음 페이지가 생성되지 않으면 기존 페이지를 그대로 유지한다.
    if (live && index == activeIndex_ && ChildCount() > 1) {
        auto* next = StaticElementCast<CarouselPage>(live->ChildAt(index ? index - 1 : 1));
        auto ready = next->Materialize();
        if (!ready) {
            return ready;
        }
    }

    auto status = PanelWidget::RemoveChild(objects, page);
    if (status) {
        if (removed) {
            (void)live->RemoveChild(*removed);
        }

        if (index <= activeIndex_ && activeIndex_) {
            --activeIndex_;
        }

        pagesChanged_.Broadcast();
    }

    return status;
}

Result<SharedPtr<Element>> CarouselWidget::CreatePanel(Allocator& allocator) noexcept {
    auto panel = MakeWidgetElement<Carousel>(allocator);
    if (panel) {
        (void)StaticElementCast<Carousel>(*panel.Value()).SetCacheChildren(cacheChildren_);
    }

    return panel;
}

Result<void> CarouselWidget::AttachChild(ObjectRegistry& objects, ObjectHandle id, Element& panel,
                                         const SharedPtr<Element>& child) noexcept {
    auto slot = objects.Resolve<WidgetSlot>(objects.OwnerAccess(), id);
    if (!slot) {
        return Result<void>::Failure(slot.Error());
    }

    const auto handle = slot.Value().Get().Value()->Content();
    auto factory = CarouselPage::Factory::CreateLambda([&objects, handle]() noexcept {
        auto widget = objects.ResolveForWrite<Widget>(objects.OwnerAccess(), handle);
        return widget ? widget.Value().Get().Value()->TakeElement(objects)
                      : Result<SharedPtr<Element>>::Failure(widget.Error());
    });
    auto& allocator = objects.ObjectAllocator();
    auto page = SharedPtr<CarouselPage>::TryMake(allocator, allocator, Move(factory));
    if (!page) {
        return Result<void>::Failure(page.Error());
    }

    if (child && (cacheChildren_ || panel.ChildCount() == 0)) {
        auto ready = page.Value()->TryAddChild(child);
        if (!ready) {
            return ready;
        }
    }

    return StaticElementCast<Carousel>(panel).AddPage(page.Value());
}

Result<SharedPtr<Element>> CarouselWidget::BuildElement(ObjectRegistry& objects) noexcept {
    auto panel = CreatePanel(objects.ObjectAllocator());
    if (!panel) {
        return panel;
    }

    for (usize i = 0; i < ChildCount(); ++i) {
        auto added = AttachChild(objects, SlotAt(i), *panel.Value(), {});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    if (ChildCount()) {
        auto shown = StaticElementCast<Carousel>(*panel.Value()).Show(activeIndex_, true, 0);
        if (!shown) {
            return Result<SharedPtr<Element>>::Failure(shown.Error());
        }
    }

    registry_ = &objects;
    return panel;
}

void CarouselWidget::ReleaseElements() noexcept {
    EndAutoScrolling();
    if (registry_) {
        for (usize i = 0; i < ChildCount(); ++i) {
            auto page = registry_->ResolveForWrite<Widget>(registry_->OwnerAccess(),
                                                           WidgetAt(*registry_, i));
            if (page) {
                page.Value().Get().Value()->ReleaseElements();
            }
        }
    }

    registry_ = nullptr;
    PanelWidget::ReleaseElements();
}

} // namespace rocks::ui
