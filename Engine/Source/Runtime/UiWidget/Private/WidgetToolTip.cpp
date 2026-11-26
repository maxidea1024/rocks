#include <Core/Containers/HashSet.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiFramework/ToolTip.h>
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

        // 사용자 위젯은 바탕·여백 없는 툴팁 상자에 담는다(UE UWidget::SetToolTip —
        // SToolTip.TextMargin(0).BorderImage(nullptr), Widget.cpp 1514–1516행). 닫힌 지난 상자에
        // 남은 내용은 떼어 새 상자에 담는다.
        auto& boxAllocator = objects.ObjectAllocator();
        return owner.SetToolTipFactory(
            Element::ToolTipFactory::CreateLambda(
                [view = Move(content.Value()),
                 &boxAllocator]() noexcept -> Result<SharedPtr<Element>> {
                    if (Element* box = view->Parent()) {
                        if (box->Parent()) {
                            return Result<SharedPtr<Element>>::Failure(ErrorCode::Busy);
                        }

                        (void)box->RemoveChild(*view);
                    }

                    auto tip = SharedPtr<ToolTip>::TryMake(boxAllocator, boxAllocator);
                    if (!tip) {
                        return Result<SharedPtr<Element>>::Failure(tip.Error());
                    }

                    auto built =
                        tip.Value()->Construct({.content = view, .style = &BareToolTipStyle()});
                    return built ? Result<SharedPtr<Element>>::Success(tip.Value())
                                 : Result<SharedPtr<Element>>::Failure(built.Error());
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

                // 글자 툴팁 상자다(UE UWidget::SetToolTipText — SToolTip, Widget.cpp 552행).
                auto shaped =
                    shaper->Shape(*label.Value(), body, DefaultToolTipStyle().text.font.size);
                if (!shaped) {
                    return Result<SharedPtr<Element>>::Failure(shaped.Error());
                }

                auto tip = SharedPtr<ToolTip>::TryMake(allocator, allocator);
                if (!tip) {
                    return Result<SharedPtr<Element>>::Failure(tip.Error());
                }

                auto built = tip.Value()->Construct({.text = label.Value()});
                return built ? Result<SharedPtr<Element>>::Success(tip.Value())
                             : Result<SharedPtr<Element>>::Failure(built.Error());
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
