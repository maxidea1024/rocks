#include <Core/Guid.h>
#include <Object/ConstructionContext.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/Widget.generated.h>

namespace rocks::ui {

Result<RootRef<Widget>> WidgetTree::ConstructWidget(ObjectRegistry& objects, TypeId type,
                                                    StringView name) noexcept {
    auto id = TryNewGuid();
    if (!id) {
        return Result<RootRef<Widget>>::Failure(id.Error());
    }

    const auto access = objects.OwnerAccess();
    ConstructionContext transaction(objects, access);
    auto handle = transaction.Create<Widget>(type, Handle(), name);
    if (!handle) {
        return Result<RootRef<Widget>>::Failure(handle.Error());
    }

    auto value = transaction.GetForWrite<Widget>(handle.Value());
    if (!value) {
        return Result<RootRef<Widget>>::Failure(value.Error());
    }

    value.Value()->stableId_ = id.Value();
    auto root = transaction.Commit();
    if (!root) {
        return Result<RootRef<Widget>>::Failure(root.Error());
    }

    return objects.TryPin<Widget>(access, handle.Value());
}

void WidgetTree::SetTextShaper(const SharedPtr<TextShaper>& shaper) noexcept {
    textShaper_ = shaper;
}

const SharedPtr<TextShaper>& WidgetTree::GetTextShaper() const noexcept {
    return textShaper_;
}

void WidgetTree::SetPlatform(Platform platform) noexcept {
    platform_ = platform;
}

Platform WidgetTree::GetPlatform() const noexcept {
    return platform_;
}

ObjectHandle WidgetTree::RootWidget() const noexcept {
    return root_.Handle();
}

Result<void> WidgetTree::SetRoot(ObjectRegistry& objects, ObjectHandle widget) noexcept {
    if (root_.Handle() == widget) {
        return Result<void>::Success();
    }

    const auto access = objects.OwnerAccess();
    const auto host = host_.Lock();
    SharedPtr<Element> replacement;
    if (!widget.IsNull()) {
        auto outer = objects.GetOuter(access, widget);
        if (!outer) {
            return Result<void>::Failure(outer.Error());
        }

        if (outer.Value() != Handle()) {
            return Result<void>::Failure(ErrorCode::WrongContext);
        }

        auto content = objects.Resolve<Widget>(access, widget);
        if (!content) {
            return Result<void>::Failure(content.Error());
        }

        auto* value = content.Value().Get().Value();
        if (!value->slot_.Handle().IsNull()) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        if (host) {
            auto built = value->TakeElement(objects);
            if (!built) {
                return Result<void>::Failure(built.Error());
            }

            replacement = Move(built.Value());
            if (replacement->Parent()) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }

    const auto previous = root_.Handle();
    auto connected = objects.SetReference(access, Handle(), root_, widget);
    if (!connected || !host) {
        return connected;
    }

    auto old = host->ChildRefAt(0);
    if (old) {
        (void)host->RemoveChild(*old);
    }

    if (replacement) {
        auto attached = host->TryAddChild(replacement);
        if (!attached) {
            (void)objects.SetReference(access, Handle(), root_, previous);
            if (old) {
                (void)host->TryAddChild(old);
            }

            return attached;
        }
    }

    if (old) {
        auto content = objects.Resolve<Widget>(access, previous);
        if (content) {
            content.Value().Get().Value()->ReleaseElements();
        }
    }

    return Result<void>::Success();
}

Result<void> WidgetTree::AttachHost(ObjectRegistry& objects,
                                    const SharedPtr<Element>& host) noexcept {
    if (!host || host->ChildCount() || host_.Lock()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }

    if (!root_.Handle().IsNull()) {
        auto borrowed = objects.Resolve<Widget>(objects.OwnerAccess(), root_.Handle());
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto built = borrowed.Value().Get().Value()->TakeElement(objects);
        if (!built) {
            return Result<void>::Failure(built.Error());
        }

        auto added = host->TryAddChild(built.Value());
        if (!added) {
            return added;
        }
    }

    host_ = WeakPtr<Element>(host);
    return Result<void>::Success();
}

void WidgetTree::DetachHost(ObjectRegistry& objects, Element& host) noexcept {
    if (auto live = host_.Lock(); live && live.Get() != &host) {
        return;
    }

    const bool attached = host.ChildCount() > 0;
    DetachHostElements(host);
    if (attached) {
        ReleaseRootWidget(objects);
    }
}

void WidgetTree::DetachHostElements(Element& host) noexcept {
    if (auto live = host_.Lock(); live && live.Get() != &host) {
        return;
    }

    host_ = {};
    if (auto child = host.ChildRefAt(0)) {
        (void)host.RemoveChild(*child);
    }
}

void WidgetTree::ReleaseRootWidget(ObjectRegistry& objects) noexcept {
    auto borrowed = objects.Resolve<Widget>(objects.OwnerAccess(), root_.Handle());
    if (borrowed) {
        borrowed.Value().Get().Value()->ReleaseElements();
    }
}

} // namespace rocks::ui
