#include <Core/Diagnostics/Log.h>
#include <Core/ScopeExit.h>
#include <UiScreens/ScreenContainer.h>
#include <UiScreens/Screen.generated.h>

#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

namespace {

constexpr LogCategory LogUiScreens{"UiScreens"};

} // namespace

ScreenStack::ScreenStack(ObjectRegistry& objects, Application& app, uint32 userId) noexcept
    : ScreenContainer(objects, app, userId, Order::Stack) {
}

ScreenQueue::ScreenQueue(ObjectRegistry& objects, Application& app, uint32 userId) noexcept
    : ScreenContainer(objects, app, userId, Order::Queue) {
}

ScreenContainer::ScreenContainer(ObjectRegistry& objects, Application& app, uint32 userId,
                                 Order order) noexcept
    : Switcher(objects.ObjectAllocator()), objects_(objects), app_(app), userId_(userId),
      order_(order), pools_(objects.ObjectAllocator()), pages_(objects.ObjectAllocator()) {
}

// 활성 화면의 OnDeactivated(사용자 hook)는 소멸자에서 부르지 않고 앱의 해체 대기열로 미룬다
// (Docs/Design/UiToolkit 21.4절 — 명시 해제는 ScreenContainerWidget::ReleaseElements의
// ClearScreens).
ScreenContainer::~ScreenContainer() noexcept {
    changing_ = true;
    const auto access = objects_.OwnerAccess();
    for (auto& page : pages_) {
        auto borrowed = objects_.ResolveForWrite<Screen>(access, page.screen);
        if (borrowed) {
            auto* screen = borrowed.Value().Get().Value();
            screen->container_ = nullptr;
            if (screen->active_) {
                screen->active_ = false;
                auto pinned = objects_.TryPin<Screen>(access, page.screen);
                Result<void> queued =
                    pinned ? Result<void>::Success() : Result<void>::Failure(pinned.Error());
                if (queued) {
                    queued = app_.DeferTeardown(Delegate<void()>::CreateLambda(
                        [objects = &objects_, screen = Move(pinned.Value())]() noexcept {
                            auto live = objects->ResolveForWrite<Screen>(objects->OwnerAccess(),
                                                                         screen.Handle());
                            if (live) {
                                live.Value().Get().Value()->OnDeactivated();
                            }
                        }));
                }

                if (!queued) {
                    Log::Warning(LogUiScreens,
                                 "Could not queue a screen deactivation ({Error}); OnDeactivated "
                                 "is skipped",
                                 queued.Error());
                }
            }
        }

        (void)Switcher::RemoveChild(*page.root);
    }
}

usize ScreenContainer::Top() const noexcept {
    if (pages_.IsEmpty()) {
        return 0;
    }

    if (order_ == Order::Switcher) {
        return selected_;
    }

    return order_ == Order::Queue ? 0 : pages_.Size() - 1;
}

usize ScreenContainer::ScreenCount() const noexcept {
    return pages_.Size();
}

ObjectHandle ScreenContainer::ScreenAt(usize index) const noexcept {
    return index < pages_.Size() ? pages_[index].screen : ObjectHandle{};
}

Result<void> ScreenContainer::ClearScreens() noexcept {
    while (!pages_.IsEmpty()) {
        // 비활성 화면을 먼저 비워 중간 화면에 불필요한 활성화 알림을 보내지 않는다.
        const usize index = Top() == 0 ? pages_.Size() - 1 : 0;
        auto removed = RemoveScreen(pages_[index].screen);
        if (!removed) {
            return removed;
        }
    }

    return Result<void>::Success();
}

Result<void> ScreenContainer::ActivateAt(usize index) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (order_ != Order::Switcher || index >= pages_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (index == selected_) {
        return Result<void>::Success();
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    Deactivate(pages_[selected_]);
    selected_ = index;
    (void)SetActiveIndex(selected_);
    Activate(pages_[selected_]);
    return Result<void>::Success();
}

ObjectHandle ScreenContainer::ActiveScreen() const noexcept {
    return pages_.IsEmpty() ? ObjectHandle{} : pages_[Top()].screen;
}

uint32 ScreenContainer::UserId() const noexcept {
    return userId_;
}

ScreenInputConfig ScreenContainer::InputConfig() const noexcept {
    if (!pages_.IsEmpty()) {
        auto screen = objects_.Resolve<Screen>(objects_.OwnerAccess(), ActiveScreen());
        if (screen) {
            return screen.Value().Get().Value()->InputConfig();
        }
    }

    return {ScreenInputMode::Game, false, false, false, false};
}

SharedPtr<Element> ScreenContainer::ActiveRoot() const noexcept {
    return pages_.IsEmpty() ? SharedPtr<Element>{} : pages_[Top()].root;
}

Result<bool> ScreenContainer::DispatchAction(Guid domain, Guid action) noexcept {
    if (pages_.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    auto screen = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), ActiveScreen());
    if (!screen) {
        return Result<bool>::Failure(screen.Error());
    }

    auto* value = screen.Value().Get().Value();
    return Result<bool>::Success((domain == Guid{} || domain == value->ActionDomain()) &&
                                 value->DispatchBoundAction(action));
}

bool ScreenContainer::IsModal() const noexcept {
    if (pages_.IsEmpty()) {
        return false;
    }

    auto screen = objects_.Resolve<Screen>(objects_.OwnerAccess(), ActiveScreen());
    return screen && screen.Value().Get().Value()->IsModal();
}

Result<void> ScreenContainer::AttachPopupLayer(const PopupRequest& popup) noexcept {
    // 화면 포인터 입력은 ActiveRoot 안에서만 라우팅한다. 가장 바깥 화면 Overlay가 직접
    // 메뉴를 받도록 탐색을 여기서 끝내면 분할 화면이나 모달 뒤로 팝업이 새지 않는다.
    auto root = ActiveRoot();
    return Result<void>::Failure(root && popup.anchor && popup.anchor->IsWithin(*root)
                                     ? ErrorCode::NotFound
                                     : ErrorCode::InvalidArgument);
}

Result<void> ScreenContainer::TryAddChild(const SharedPtr<Element>&) noexcept {
    return Result<void>::Failure(ErrorCode::Unsupported);
}

bool ScreenContainer::RemoveChild(Element&) noexcept {
    return false;
}

Result<ObjectHandle> ScreenContainer::AddScreen(ClassRef<Screen> type) noexcept {
    if (changing_) {
        return Result<ObjectHandle>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    auto valid = type.Resolve(objects_.Types());
    if (!valid) {
        return Result<ObjectHandle>::Failure(valid.Error());
    }

    WidgetPool* pool = nullptr;
    for (const auto& item : pools_) {
        if (item.type == type.Id()) {
            pool = item.widgets.Get();
            break;
        }
    }

    if (!pool) {
        auto made = UniquePtr<WidgetPool>::TryMake(objects_.ObjectAllocator(), objects_, type.Id());
        if (!made) {
            return Result<ObjectHandle>::Failure(made.Error());
        }

        pool = made.Value().Get();
        auto added = pools_.TryPushBack({type.Id(), Move(made.Value())});
        if (!added) {
            return Result<ObjectHandle>::Failure(added.Error());
        }
    }

    auto entry = pool->Acquire();
    if (!entry) {
        return Result<ObjectHandle>::Failure(entry.Error());
    }

    const auto handle = entry.Value().widget;
    auto attached = Attach(handle, pool);
    if (!attached) {
        (void)pool->Release(handle, false);
        return Result<ObjectHandle>::Failure(attached.Error());
    }

    return Result<ObjectHandle>::Success(handle);
}

Result<void> ScreenContainer::AddScreen(ObjectHandle handle) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    return Attach(handle, nullptr);
}

Result<void> ScreenContainer::Attach(ObjectHandle handle, WidgetPool* pool) noexcept {
    auto borrowed = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto* screen = borrowed.Value().Get().Value();
    if (screen->container_) {
        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    auto root = screen->TakeElement(objects_);
    if (!root) {
        return Result<void>::Failure(root.Error());
    }

    auto reserved = pages_.TryEnsureCapacity(pages_.Size() + 1);
    if (!reserved) {
        return reserved;
    }

    auto attached = Switcher::TryAddChild(root.Value());
    if (!attached) {
        return attached;
    }

    const bool first = pages_.IsEmpty();
    if (first) {
        previousFocus_ = WeakPtr<Element>(app_.GetFocus(userId_));
        previousCause_ = app_.GetFocusCause(userId_);
        lastCause_ = previousCause_;
    } else if (order_ == Order::Stack) {
        Deactivate(pages_[Top()]);
    }

    screen->container_ = this;
    (void)pages_.TryPushBack({handle, Move(root.Value()), {}, pool, FocusCause::Direct});
    if (first || order_ == Order::Stack) {
        (void)SetActiveIndex(Top());
        Activate(pages_[Top()]);
    }

    return Result<void>::Success();
}

void ScreenContainer::Deactivate(Page& page) noexcept {
    page.root->DismissHostedPopups();
    auto focus = app_.GetFocus(userId_);
    if (focus && focus->IsWithin(*page.root)) {
        page.focus = WeakPtr<Element>(focus);
        page.focusCause = app_.GetFocusCause(userId_);
        lastCause_ = page.focusCause;
        (void)app_.SetFocus(userId_, {});
    }

    auto screen = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), page.screen);
    if (screen) {
        auto* value = screen.Value().Get().Value();
        value->active_ = false;
        value->OnDeactivated();
    }
}

void ScreenContainer::Activate(Page& page) noexcept {
    auto borrowed = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), page.screen);
    if (!borrowed) {
        return;
    }

    auto* screen = borrowed.Value().Get().Value();
    screen->active_ = true;
    screen->OnActivated();
    (void)ApplyDesiredFocus(page, *screen);
}

bool ScreenContainer::FocusDesiredTarget() noexcept {
    if (pages_.IsEmpty()) {
        return false;
    }

    Page& page = pages_[Top()];
    auto screen = objects_.Resolve<Screen>(objects_.OwnerAccess(), page.screen);
    return screen && ApplyDesiredFocus(page, *screen.Value().Get().Value());
}

bool ScreenContainer::ApplyDesiredFocus(Page& page, const Screen& screen) noexcept {
    if (screen.restoreFocus_) {
        auto focus = page.focus.Lock();
        if (focus && focus->IsWithin(*page.root) &&
            app_.SetFocus(userId_, focus, page.focusCause)) {
            return true;
        }
    }

    if (!screen.FocusTarget().IsNull()) {
        auto widget = objects_.Resolve<Widget>(objects_.OwnerAccess(), screen.FocusTarget());
        if (widget) {
            auto focus = widget.Value().Get().Value()->CachedElement();
            if (focus && focus->IsWithin(*page.root) && app_.SetFocus(userId_, focus, lastCause_)) {
                return true;
            }
        }
    }

    auto moved = app_.MoveFocusWithin(userId_, page.root, true, false, lastCause_);
    return moved && moved.Value();
}

Result<void> ScreenContainer::RemoveScreen(ObjectHandle handle) noexcept {
    if (changing_) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    changing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        changing_ = false;
    };
    for (usize i = 0; i < pages_.Size(); ++i) {
        if (pages_[i].screen != handle) {
            continue;
        }

        const bool active = i == Top();
        if (active) {
            Deactivate(pages_[i]);
        }

        auto screen = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), handle);
        if (screen) {
            screen.Value().Get().Value()->container_ = nullptr;
        }

        Page removed = Move(pages_[i]);
        (void)Switcher::RemoveChild(*removed.root);
        (void)pages_.TryRemoveAt(i);
        if (order_ == Order::Switcher && i <= selected_ && selected_ > 0) {
            --selected_;
        }

        if (!pages_.IsEmpty()) {
            (void)SetActiveIndex(Top());
            if (active) {
                Activate(pages_[Top()]);
            }
        } else {
            if (auto previous = previousFocus_.Lock()) {
                (void)app_.SetFocus(userId_, previous, previousCause_);
            }

            previousFocus_ = {};
        }

        return removed.pool ? removed.pool->Release(handle, true) : Result<void>::Success();
    }

    return Result<void>::Failure(ErrorCode::NotFound);
}

Result<bool> ScreenContainer::Back() noexcept {
    if (changing_ || backing_) {
        return Result<bool>::Failure(ErrorCode::Busy);
    }

    backing_ = true;
    ROCKS_ON_SCOPE_EXIT {
        backing_ = false;
    };
    const auto handle = ActiveScreen();
    if (handle.IsNull()) {
        return Result<bool>::Success(false);
    }

    auto screen = objects_.ResolveForWrite<Screen>(objects_.OwnerAccess(), handle);
    if (!screen) {
        return Result<bool>::Failure(screen.Error());
    }

    if (screen.Value().Get().Value()->OnBack() || ActiveScreen() != handle) {
        return Result<bool>::Success(true);
    }

    auto removed = RemoveScreen(handle);
    return removed ? Result<bool>::Success(true) : Result<bool>::Failure(removed.Error());
}

} // namespace rocks::ui
