#include <Core/Containers/HashSet.h>
#include <UiWidget/Widget.generated.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetTextShaper.h>
#include <UiFramework/Frame.h>

namespace rocks::ui {

const Text& Widget::ToolTipText() const noexcept {
    return toolTipText_;
}

ObjectHandle Widget::ToolTipWidget() const noexcept {
    return toolTipWidget_.Handle();
}

Result<void> Widget::ApplyToolTip(ObjectRegistry& objects, Element& owner, const Text& text,
                                  ObjectHandle custom) noexcept {
    TextPersistentView fallback;
    if (text.IsValid()) {
        (void)text.TryGetPersistentForm(fallback);
    }

    if (!custom.IsNull()) {
        auto target = objects.ResolveForWrite<Widget>(objects.OwnerAccess(), custom);
        if (!target) {
            return Result<void>::Failure(target.Error());
        }

        auto* widget = target.Value().Get().Value();
        if (widget == this || widget->rebuilding_ || !widget->Slot().IsNull()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto content = widget->TakeElement(objects);
        if (!content) {
            return Result<void>::Failure(content.Error());
        }

        if (owner.IsWithin(*content.Value())) {
            return Result<void>::Failure(ErrorCode::CycleDetected);
        }

        return owner.SetToolTipFactory(
            Element::ToolTipFactory::CreateLambda([view = Move(content.Value())]() noexcept {
                return view->Parent() ? Result<SharedPtr<Element>>::Failure(ErrorCode::Busy)
                                      : Result<SharedPtr<Element>>::Success(view);
            }),
            fallback.source, true);
    }

    if (!text.IsValid()) {
        return owner.SetToolTipFactory({});
    }

    auto outer = PresentationTree(objects);
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(objects.OwnerAccess(), outer.Value());
    if (!tree && !outer.Value().IsNull()) {
        return Result<void>::Failure(tree.Error());
    }

    auto shared = text.Clone();
    if (!shared) {
        return Result<void>::Failure(shared.Error());
    }

    auto& allocator = objects.ObjectAllocator();
    return owner.SetToolTipFactory(
        Element::ToolTipFactory::CreateLambda(
            [shaper =
                 tree ? tree.Value().Get().Value()->TextShaper() : SharedPtr<WidgetTextShaper>{},
             body = Move(shared.Value()), &allocator]() noexcept -> Result<SharedPtr<Element>> {
                if (!shaper) {
                    return Result<SharedPtr<Element>>::Failure(ErrorCode::NotReady);
                }

                auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
                if (!label) {
                    return Result<SharedPtr<Element>>::Failure(label.Error());
                }

                auto shaped = shaper->Shape(*label.Value(), body, 14);
                if (!shaped) {
                    return Result<SharedPtr<Element>>::Failure(shaped.Error());
                }

                auto frame = SharedPtr<Frame>::TryMake(allocator, allocator);
                if (!frame) {
                    return Result<SharedPtr<Element>>::Failure(frame.Error());
                }

                frame.Value()->SetColor({0.05f, 0.05f, 0.05f, 1});
                (void)frame.Value()->SetPadding({8, 4, 8, 4});
                auto added = frame.Value()->TryAddChild(label.Value());
                if (!added) {
                    return Result<SharedPtr<Element>>::Failure(added.Error());
                }

                return Result<SharedPtr<Element>>::Success(Move(frame.Value()));
            }),
        fallback.source, false);
}

Result<void> Widget::SetToolTipText(ObjectRegistry& objects, const Text& text) noexcept {
    auto copy = text.DeepClone(objects.ObjectAllocator());
    if (!copy) {
        return Result<void>::Failure(copy.Error());
    }

    if (auto live = CachedElement()) {
        auto updated = ApplyToolTip(objects, *live, copy.Value(), toolTipWidget_.Handle());
        if (!updated) {
            return updated;
        }
    }

    toolTipText_ = Move(copy.Value());
    return Result<void>::Success();
}

Result<void> Widget::SetToolTip(ObjectRegistry& objects, ObjectHandle custom) noexcept {
    const auto access = objects.OwnerAccess();
    const ObjectHandle previous = toolTipWidget_.Handle();
    if (previous == custom) {
        return Result<void>::Success();
    }

    if (!custom.IsNull()) {
        auto mine = PresentationTree(objects);
        auto theirs = objects.GetOuter(access, custom);
        if (!mine || !theirs || mine.Value() != theirs.Value()) {
            return Result<void>::Failure(ErrorCode::WrongContext);
        }

        HashSet<ObjectHandle> visited(objects.ObjectAllocator());
        for (auto id = custom; !id.IsNull();) {
            auto seen = visited.Add(id);
            if (!seen) {
                return Result<void>::Failure(seen.Error());
            }

            if (id == Handle() || seen.Value() != InsertOutcome::Inserted) {
                return Result<void>::Failure(ErrorCode::CycleDetected);
            }

            auto widget = objects.Resolve<Widget>(access, id);
            if (!widget) {
                return Result<void>::Failure(widget.Error());
            }

            if (!widget.Value().Get().Value()->Slot().IsNull()) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }

            id = widget.Value().Get().Value()->ToolTipWidget();
        }
    }

    auto set = objects.SetReference(access, Handle(), toolTipWidget_, custom);
    if (!set) {
        return set;
    }

    if (auto live = CachedElement()) {
        auto applied = ApplyToolTip(objects, *live, toolTipText_, custom);
        if (!applied) {
            (void)objects.SetReference(access, Handle(), toolTipWidget_, previous);
            return applied;
        }
    }

    return Result<void>::Success();
}
} // namespace rocks::ui
