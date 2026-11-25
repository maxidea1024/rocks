#include "ControlCaption.h"

#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetTextShaper.h>

namespace rocks::ui {

Result<Delegate<Result<void>(StringView)>>
AttachControlCaption(ObjectRegistry& objects, ObjectHandle owner, Element& control) noexcept {
    using Caption = Delegate<Result<void>(StringView)>;
    auto& allocator = objects.ObjectAllocator();
    const auto access = objects.OwnerAccess();
    auto outer = objects.GetOuter(access, owner);
    if (!outer) {
        return Result<Caption>::Failure(outer.Error());
    }

    auto tree = objects.Resolve<WidgetTree>(access, outer.Value());
    if (!tree) {
        return Result<Caption>::Failure(tree.Error());
    }

    auto shaper = tree.Value().Get().Value()->TextShaper();
    if (!shaper) {
        return Result<Caption>::Failure(ErrorCode::NotReady);
    }

    auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
    if (!label) {
        return Result<Caption>::Failure(label.Error());
    }

    auto attached = control.TryAddChild(label.Value());
    if (!attached) {
        return Result<Caption>::Failure(attached.Error());
    }

    return Caption::TryCreateLambda(allocator, [shaper = Move(shaper), label = Move(label.Value()),
                                                &allocator](StringView caption) noexcept {
        if (caption.IsEmpty()) {
            return label->SetShapedText({}, {}, {}, 16, false);
        }

        auto text = Text::Invariant(allocator, caption);
        return text ? shaper->Shape(*label, text.Value(), 16) : Result<void>::Failure(text.Error());
    });
}

} // namespace rocks::ui
