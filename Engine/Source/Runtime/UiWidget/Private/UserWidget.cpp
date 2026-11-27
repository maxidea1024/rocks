#include <Core/Diagnostics/Log.h>
#include <UiFramework/Layout/Overlay.h>

#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

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
    // 버려지면 마지막으로 측정한 앱의 해체 대기열로 미룬다(Docs/Design/UiToolkit 21.4절).
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
        auto tree = objects_.Resolve<WidgetTree>(access, handle);
        if (tree) {
            tree.Value().Get().Value()->DetachHost(objects_, *this);
        }

        auto owner = objects_.Resolve<UserWidget>(access, owner_.Handle());
        if (owner) {
            owner.Value().Get().Value()->StopAllAnimations();
            owner.Value().Get().Value()->OnDestruct();
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
        auto tree = objects_.Resolve<WidgetTree>(access, handle);
        if (tree) {
            tree.Value().Get().Value()->DetachHost(objects_, *this);
        }

        auto host = host_.Lock();
        if (!host) {
            // 앱에 붙어 측정된 적이 없다 — 통지할 자리가 없다(명시 ReleaseElements를 쓴다).
            Log::Warning(LogUiWidget,
                         "A user widget element was dropped without ReleaseElements before any "
                         "application measured it; OnDestruct is skipped");
            owner_.Reset();
            return;
        }

        auto queued = host->DeferTeardown(
            Delegate<void()>::CreateLambda([objects = &objects_, owner = Move(owner_)]() noexcept {
                auto widget = objects->Resolve<UserWidget>(objects->OwnerAccess(), owner.Handle());
                if (widget) {
                    widget.Value().Get().Value()->StopAllAnimations();
                    widget.Value().Get().Value()->OnDestruct();
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
    // 마지막으로 측정한 앱의 호스트 묶음이다(미룬 해체 통지 자리).
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

} // namespace rocks::ui
