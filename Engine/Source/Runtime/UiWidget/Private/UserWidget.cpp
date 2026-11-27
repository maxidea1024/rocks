#include <Core/Diagnostics/Log.h>
#include <UiFramework/Layout/Overlay.h>

#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

// UserWidgetRoot(이 파일의 익명 이름공간)가 UserWidget의 미룬 해체 번호를 다루는 통로다.
struct UserWidgetTeardown {
    // 새 미룬 해체 번호를 매기고 돌려준다.
    static uint64 Begin(UserWidget& widget) noexcept {
        widget.pendingTeardown_ = ++widget.teardownSerial_;
        return widget.pendingTeardown_;
    }

    // token이 아직 남은 해체이면 끝낸다(그 사이 다시 구성해 이미 끝났으면 아무 일도 없다).
    static void Finish(UserWidget& widget, ObjectRegistry& objects, uint64 token) noexcept {
        if (widget.pendingTeardown_ == token) {
            widget.FinishPendingTeardown(objects);
        }
    }
};

namespace {

constexpr LogCategory LogUiWidget{"UiWidget"};

} // namespace

UserWidget::UserWidget(Allocator& allocator) noexcept : animations_(allocator) {
}

namespace {

// 화면 트리가 UserWidget을 붙잡고, UserWidget은 이 루트를 약하게 참조한다.
class UserWidgetRoot final : public Overlay {
    ROCKS_UI_ELEMENT(UserWidgetRoot, Overlay)

public:
    UserWidgetRoot(Allocator& allocator, ObjectRegistry& objects,
                   RootRef<UserWidget> owner) noexcept
        : Overlay(allocator), objects_(objects), owner_(Move(owner)) {
    }

    // 사용자 hook(OnDestruct·애니메이션 정지 통지)을 소멸자에서 부르지 않는다. 명시 해제 없이
    // 버려지면 마지막으로 붙은 앱의 해체 대기열로 미룬다(Docs/Design/UiToolkit 21.4절).
    ~UserWidgetRoot() noexcept override {
        DetachDeferred();
    }

    void OnUiHostServicesChanged() noexcept override {
        host_ = WeakPtr<UiHostServices>(FindUiHostServicesRef());
    }

    bool SupportsKeyboardFocus() const noexcept override {
        auto* owner = LiveOwner();
        return owner && owner->IsFocusable();
    }

    bool OnPreviewKeyDown(uint32 user, const input::InputEvent& event) noexcept override {
        auto* owner = LiveOwner();
        return owner && owner->OnPreviewKey(user, event);
    }

    KeyReply OnKeyEvent(const KeyEvent& key) noexcept override {
        auto* owner = LiveOwner();
        if (owner && owner->OnKey(key.user, key.input)) {
            return KeyReply::Handled();
        }

        return Element::OnKeyEvent(key);
    }

    PointerReply OnPreviewPointerDown(const PointerEvent& event) noexcept override {
        auto* owner = LiveOwner();
        return owner ? owner->OnPreviewPointer(event) : PointerReply{};
    }

    PointerReply OnPointerEvent(const PointerEvent& event) noexcept override {
        auto* owner = LiveOwner();
        return owner ? owner->OnPointer(event) : PointerReply{};
    }

    void OnFocusReceived(uint32 user) noexcept override {
        if (auto* owner = LiveOwner()) {
            owner->OnFocusReceived(user);
        }
    }

    void OnFocusLost(uint32 user) noexcept override {
        if (auto* owner = LiveOwner()) {
            owner->OnFocusLost(user);
        }
    }

    void OnPointerCaptureLost(PointerSource source, uint32 user, uint32 pointer) noexcept override {
        if (auto* owner = LiveOwner()) {
            owner->OnPointerCaptureLost(source, user, pointer);
        }
    }

    Result<void> OnPaintOverlay(PaintList& list, int32 layer) const noexcept override {
        auto* owner = LiveOwner();
        return owner ? owner->OnPaintOverlay(list, GetGeometry(), layer) : Result<void>::Success();
    }

    void Activate(ObjectHandle tree) noexcept {
        tree_ = tree;
    }

    void Detach() noexcept {
        if (tree_.IsNull()) {
            return;
        }

        const auto handle = tree_;
        tree_ = {};
        const auto access = objects_.OwnerAccess();
        // UE 순서다(SObjectWidget::ResetWidget — NativeDestruct 뒤에 자식 Slate 자원을 놓는다):
        // OnDestruct는 아직 붙어 있는 자식 Element를 볼 수 있다.
        auto owner = objects_.Resolve<UserWidget>(access, owner_.Handle());
        if (owner) {
            owner.Value().Get().Value()->StopAllAnimations();
            owner.Value().Get().Value()->OnDestruct();
        }

        auto tree = objects_.Resolve<WidgetTree>(access, handle);
        if (tree) {
            tree.Value().Get().Value()->DetachHost(objects_, *this);
        }

        owner_.Reset();
    }

    // 엔진 정리(트리에서 떼기)만 지금 하고 사용자 통지는 대기열로 넘긴다. 대기열 항목이 owner를
    // RootRef로 붙잡아 통지 전에 GC되지 않는다.
    void DetachDeferred() noexcept {
        if (tree_.IsNull()) {
            return;
        }

        const auto handle = tree_;
        tree_ = {};
        const auto access = objects_.OwnerAccess();
        // 엔진 정리(호스트에서 Element 떼기)만 지금 한다. 이 위젯의 OnDestruct와 루트 Widget의
        // ReleaseElements(중첩 UserWidget·목록·화면 컨테이너의 사용자 hook)는 대기열이나 다시
        // 구성하기 직전(FinishPendingTeardown)에 그 순서로 돈다.
        auto tree = objects_.Resolve<WidgetTree>(access, handle);
        if (tree) {
            tree.Value().Get().Value()->DetachHostElements(*this);
        }

        auto host = host_.Lock();
        if (!host) {
            // 앱에 붙은 적이 없거나 마지막 앱이 이미 부서졌다 — 통지할 자리가 없다(명시
            // ReleaseElements를 쓴다). OnConstruct는 TakeElement 때 돌았으므로 짝이 깨진다(경고).
            Log::Warning(LogUiWidget,
                         "A user widget element was dropped without ReleaseElements and no live "
                         "application can deliver its OnDestruct (never attached, or its last "
                         "application is gone); OnDestruct is skipped");
            owner_.Reset();
            return;
        }

        auto widget = objects_.ResolveForWrite<UserWidget>(access, owner_.Handle());
        if (!widget) {
            Log::Warning(LogUiWidget,
                         "Could not mark a user widget teardown ({Error}); OnDestruct is skipped",
                         widget.Error());
            owner_.Reset();
            return;
        }

        const uint64 token = UserWidgetTeardown::Begin(*widget.Value().Get().Value());
        auto queued = host->DeferTeardown(Delegate<void()>::CreateLambda(
            [objects = &objects_, owner = Move(owner_), token]() noexcept {
                auto pending =
                    objects->ResolveForWrite<UserWidget>(objects->OwnerAccess(), owner.Handle());
                if (pending) {
                    UserWidgetTeardown::Finish(*pending.Value().Get().Value(), *objects, token);
                }
            }));
        if (!queued) {
            Log::Warning(LogUiWidget,
                         "Could not queue a user widget teardown ({Error}); OnDestruct is skipped",
                         queued.Error());
        }

        owner_.Reset();
    }

private:
    UserWidget* LiveOwner() const noexcept {
        if (tree_.IsNull()) {
            return nullptr;
        }

        auto borrowed = objects_.Resolve<UserWidget>(objects_.OwnerAccess(), owner_.Handle());
        return borrowed ? borrowed.Value().Get().Value() : nullptr;
    }

    ObjectRegistry& objects_;
    RootRef<UserWidget> owner_;
    // 마지막으로 붙은 앱의 호스트 묶음이다(미룬 해체 통지 자리 — 떨어질 때 지우지 않는다).
    WeakPtr<UiHostServices> host_;
    ObjectHandle tree_{};
};

} // namespace

ObjectHandle UserWidget::ViewModel() const noexcept {
    return viewModel_.Handle();
}

ObjectHandle UserWidget::Tree() const noexcept {
    return tree_.Handle();
}

Result<void> UserWidget::Initialize(ObjectRegistry& objects) noexcept {
    if (initialized_) {
        return Result<void>::Success();
    }

    if (tree_.Handle().IsNull()) {
        const auto access = objects.OwnerAccess();
        auto created = objects.NewObject<WidgetTree>(access, GeneratedType<WidgetTree>::Id(),
                                                     Handle(), "WidgetTree");
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        auto set = objects.SetReference(access, Handle(), tree_, created.Value().Handle());
        if (!set) {
            return set;
        }
    }

    initialized_ = true;
    OnInitialized();
    return Result<void>::Success();
}

Result<SharedPtr<Element>> UserWidget::BuildElement(ObjectRegistry& objects) noexcept {
    // 옛 Element의 미룬 해체가 남았으면 다시 구성하기 전에 끝낸다(UE 순서 — 옛 NativeDestruct 뒤
    // 새 NativeConstruct, 21.4절). 대기열 항목은 번호가 지워져 아무 일도 하지 않는다.
    FinishPendingTeardown(objects);
    auto initialized = Initialize(objects);
    if (!initialized) {
        return Result<SharedPtr<Element>>::Failure(initialized.Error());
    }

    OnPreConstruct();
    const auto access = objects.OwnerAccess();
    auto tree = objects.Resolve<WidgetTree>(access, Tree());
    if (!tree) {
        return Result<SharedPtr<Element>>::Failure(tree.Error());
    }

    auto owner = objects.TryPin<UserWidget>(access, Handle());
    if (!owner) {
        return Result<SharedPtr<Element>>::Failure(owner.Error());
    }

    auto& allocator = objects.ObjectAllocator();
    auto wrapper =
        SharedPtr<UserWidgetRoot>::TryMake(allocator, allocator, objects, Move(owner.Value()));
    if (!wrapper) {
        return Result<SharedPtr<Element>>::Failure(wrapper.Error());
    }

    auto attached = tree.Value().Get().Value()->AttachHost(objects, wrapper.Value());
    if (!attached) {
        return Result<SharedPtr<Element>>::Failure(attached.Error());
    }

    wrapper.Value()->Activate(Tree());
    return Result<SharedPtr<Element>>::Success(wrapper.Value());
}

void UserWidget::ReleaseElements() noexcept {
    auto root = CachedElement();
    auto layer = viewportLayer_.Lock();
    viewportLayer_ = {};
    if (root && layer && root->Parent() == layer.Get()) {
        (void)layer->RemoveChild(*root);
    }

    Widget::ReleaseElements();
    if (root) {
        StaticElementCast<UserWidgetRoot>(root.Get())->Detach();
    }
}

void UserWidget::OnInitialized() noexcept {
}

void UserWidget::OnPreConstruct() noexcept {
}

bool UserWidget::OnPreviewKey(uint32, const input::InputEvent&) noexcept {
    return false;
}

bool UserWidget::OnKey(uint32, const input::InputEvent&) noexcept {
    return false;
}

PointerReply UserWidget::OnPreviewPointer(const PointerEvent&) noexcept {
    return {};
}

PointerReply UserWidget::OnPointer(const PointerEvent&) noexcept {
    return {};
}

void UserWidget::OnFocusReceived(uint32) noexcept {
}

void UserWidget::OnFocusLost(uint32) noexcept {
}

void UserWidget::OnPointerCaptureLost(PointerSource, uint32, uint32) noexcept {
}

Result<void> UserWidget::OnPaintOverlay(PaintList&, const Geometry&, int32) const noexcept {
    return Result<void>::Success();
}

void UserWidget::SetFocusable(bool focusable) noexcept {
    focusable_ = focusable;
}

bool UserWidget::IsFocusable() const noexcept {
    return focusable_;
}

void UserWidget::OnConstruct() noexcept {
}

void UserWidget::OnDestruct() noexcept {
}

void UserWidget::OnRebuilt() noexcept {
    OnConstruct();
}

void UserWidget::FinishPendingTeardown(ObjectRegistry& objects) noexcept {
    if (pendingTeardown_ == NoPendingTeardown) {
        return;
    }

    pendingTeardown_ = NoPendingTeardown;
    StopAllAnimations();
    OnDestruct();
    if (auto widgets = objects.Resolve<WidgetTree>(objects.OwnerAccess(), Tree())) {
        widgets.Value().Get().Value()->ReleaseRootWidget(objects);
    }
}

} // namespace rocks::ui
