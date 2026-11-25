#include <UiFramework/Overlay.h>

#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/UserWidget.generated.h>

namespace rocks::ui {

UserWidget::UserWidget(Allocator& allocator) noexcept : animations_(allocator) {
}

namespace {

// 화면 트리가 UserWidget을 붙잡고, UserWidget은 이 루트를 약하게 참조한다.
class UserWidgetRoot final : public Overlay {
public:
    UserWidgetRoot(Allocator& allocator, ObjectRegistry& objects,
                   RootRef<UserWidget> owner) noexcept
        : Overlay(allocator), objects_(objects), owner_(Move(owner)) {
    }

    ~UserWidgetRoot() noexcept override {
        Detach();
    }

    bool SupportsKeyboardFocus() const noexcept override {
        auto* owner = LiveOwner();
        return owner && owner->IsFocusable();
    }

    bool OnPreviewKeyDown(uint32 user, const input::InputEvent& event) noexcept override {
        auto* owner = LiveOwner();
        return owner && owner->OnPreviewKey(user, event);
    }

    bool OnKeyEvent(uint32 user, const input::InputEvent& event) noexcept override {
        auto* owner = LiveOwner();
        return owner && owner->OnKey(user, event);
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
        static_cast<UserWidgetRoot*>(root.Get())->Detach();
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
