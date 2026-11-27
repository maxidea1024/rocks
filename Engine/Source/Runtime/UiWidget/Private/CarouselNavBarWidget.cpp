#include <UiWidget/CarouselNavBarWidget.generated.h>
#include <UiWidget/CarouselWidget.generated.h>
#include <UiFramework/Layout/HorizontalStack.h>
#include <UiFramework/Layout/SizeBox.h>

namespace rocks::ui {

namespace {

// 기본 탐색 버튼 크기다(factory가 없을 때).
constexpr Vec2 NavButtonDefaultSize{20, 16};

} // namespace

ObjectHandle CarouselNavBarWidget::LinkedCarousel() const noexcept {
    return linkedCarousel_.Handle();
}

void CarouselNavBarWidget::Disconnect() noexcept {
    if (registry_ && subscription_.IsValid()) {
        auto old =
            registry_->ResolveForWrite<CarouselWidget>(registry_->OwnerAccess(), LinkedCarousel());
        if (old) {
            (void)old.Value().Get().Value()->OnPagesChanged().Remove(subscription_);
        }
    }

    subscription_.Reset();
    registry_ = nullptr;
}

Result<void> CarouselNavBarWidget::Refresh(ObjectRegistry& objects, Element& host,
                                           bool rebuild) noexcept {
    CarouselWidget* carousel = nullptr;
    if (!LinkedCarousel().IsNull()) {
        auto value =
            objects.ResolveForWrite<CarouselWidget>(objects.OwnerAccess(), LinkedCarousel());
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        carousel = value.Value().Get().Value();
    }

    const usize count = carousel ? carousel->ChildCount() : 0;
    const usize selected = carousel ? carousel->ActiveIndex() : 0;
    auto old = host.ChildRefAt(0);
    if (!rebuild && old && old->ChildCount() == count) {
        for (usize i = 0; i < count; ++i) {
            auto* button = StaticElementCast<Button>(old->ChildAt(i));
            button->SetColor(i == selected ? LinearColor{0.14f, 0.4f, 0.8f, 1}
                                           : LinearColor{0.2f, 0.2f, 0.2f, 1});
        }

        return Result<void>::Success();
    }

    auto& allocator = objects.ObjectAllocator();
    auto row = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    if (!row) {
        return Result<void>::Failure(row.Error());
    }

    for (usize i = 0; i < count; ++i) {
        auto button =
            factory_ ? factory_.Execute(i) : SharedPtr<Button>::TryMake(allocator, allocator);
        if (!button || !button.Value() || button.Value()->Parent()) {
            return Result<void>::Failure(button ? ErrorCode::InvalidArgument : button.Error());
        }

        if (!factory_) {
            button.Value()->SetDesiredSize(NavButtonDefaultSize);
        }

        button.Value()->SetColor(i == selected ? LinearColor{0.14f, 0.4f, 0.8f, 1}
                                               : LinearColor{0.2f, 0.2f, 0.2f, 1});
        button.Value()->SetOnClicked(
            Delegate<void()>::CreateLambda([&objects, linked = LinkedCarousel(), i]() noexcept {
                auto page = objects.ResolveForWrite<CarouselWidget>(objects.OwnerAccess(), linked);
                if (page) {
                    auto* target = page.Value().Get().Value();
                    if (target->ActiveIndex() != i && i < target->ChildCount()) {
                        target->EndAutoScrolling();
                        (void)target->SetActiveIndex(static_cast<uint32>(i));
                    }
                }
            }));
        // 단추 여백은 단추의 slot 배치다(20.10절 — wrapper를 끼우지 않는다).
        auto added = button.Value()->SetSlotPlacement(SlotPlacement{.padding = buttonPadding_});
        if (added) {
            added = row.Value()->TryAddChild(button.Value());
        }

        if (!added) {
            return added;
        }
    }

    if (old) {
        (void)host.RemoveChild(*old);
    }

    auto attached = host.TryAddChild(row.Value());
    if (!attached && old) {
        (void)host.TryAddChild(old);
    }

    return attached;
}

Result<void> CarouselNavBarWidget::Connect(ObjectRegistry& objects,
                                           const SharedPtr<Element>& native) noexcept {
    auto refreshed = Refresh(objects, *native, true);
    if (!refreshed) {
        return refreshed;
    }

    registry_ = &objects;
    if (LinkedCarousel().IsNull()) {
        return refreshed;
    }

    auto carousel =
        objects.ResolveForWrite<CarouselWidget>(objects.OwnerAccess(), LinkedCarousel());
    if (!carousel) {
        return Result<void>::Failure(carousel.Error());
    }

    auto added = carousel.Value().Get().Value()->OnPagesChanged().TryAddWeakLambda(
        native, [&objects, owner = Handle()]() noexcept {
            auto widget =
                objects.ResolveForWrite<CarouselNavBarWidget>(objects.OwnerAccess(), owner);
            if (widget) {
                auto* bar = widget.Value().Get().Value();
                if (auto view = bar->CachedElement()) {
                    (void)bar->Refresh(objects, *view);
                }
            }
        });
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    subscription_ = added.Value();
    return Result<void>::Success();
}

Result<void> CarouselNavBarWidget::SetLinkedCarousel(ObjectRegistry& objects,
                                                     ObjectHandle carousel) noexcept {
    if (!carousel.IsNull()) {
        auto valid = objects.Resolve<CarouselWidget>(objects.OwnerAccess(), carousel);
        if (!valid) {
            return Result<void>::Failure(valid.Error());
        }
    }

    auto old = LinkedCarousel();
    if (old == carousel) {
        return Result<void>::Success();
    }

    auto live = CachedElement();
    Disconnect();
    auto linked = objects.SetReference(objects.OwnerAccess(), Handle(), linkedCarousel_, carousel);
    if (linked && live) {
        linked = Connect(objects, live);
    }

    if (!linked) {
        Disconnect();
        (void)objects.SetReference(objects.OwnerAccess(), Handle(), linkedCarousel_, old);
        if (live) {
            (void)Connect(objects, live);
        }
    }

    return linked;
}

Result<void> CarouselNavBarWidget::SetButtonPadding(Vec4 padding) noexcept {
    if (!SlotPlacement{.padding = padding}.IsValid()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto previous = buttonPadding_;
    buttonPadding_ = padding;
    if (auto live = CachedElement(); live && registry_) {
        auto changed = Refresh(*registry_, *live, true);
        if (!changed) {
            buttonPadding_ = previous;
        }

        return changed;
    }

    return Result<void>::Success();
}

Result<void> CarouselNavBarWidget::SetButtonFactory(ButtonFactory&& factory) noexcept {
    auto previous = Move(factory_);
    factory_ = Move(factory);
    if (auto live = CachedElement(); live && registry_) {
        auto changed = Refresh(*registry_, *live, true);
        if (!changed) {
            factory_ = Move(previous);
        }

        return changed;
    }

    return Result<void>::Success();
}

Result<SharedPtr<Element>> CarouselNavBarWidget::BuildElement(ObjectRegistry& objects) noexcept {
    Disconnect();
    auto& allocator = objects.ObjectAllocator();
    auto host = SharedPtr<SizeBox>::TryMake(allocator, allocator);
    if (!host) {
        return Result<SharedPtr<Element>>::Failure(host.Error());
    }

    auto linked = Connect(objects, host.Value());
    return linked ? Result<SharedPtr<Element>>::Success(Move(host.Value()))
                  : Result<SharedPtr<Element>>::Failure(linked.Error());
}

void CarouselNavBarWidget::ReleaseElements() noexcept {
    Disconnect();
    Widget::ReleaseElements();
}

} // namespace rocks::ui
