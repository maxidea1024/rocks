#include <UiWidget/Widget.h>
#include <UiWidget/WidgetTree.generated.h>
#include <UiWidget/WidgetTextShaper.h>

namespace rocks::ui {

namespace {

// 셰이퍼가 연결된 화면은 현재 로캘의 실제 표시 문자열을 사용한다. 헤드리스 화면에서는
// 번역 카탈로그를 요구하지 않는 invariant/localized 원문을 제공한다.
Result<void> ReadDescription(ObjectRegistry& objects, ObjectHandle owner, const Text& value,
                             String& out) noexcept {
    if (!value.IsValid()) {
        out.Clear();
        return Result<void>::Success();
    }

    auto tree = objects.Resolve<WidgetTree>(objects.OwnerAccess(), owner);
    if (tree && tree.Value().Get().Value()->TextShaper()) {
        TextLabel label(objects.ObjectAllocator());
        auto shaped = tree.Value().Get().Value()->TextShaper()->Shape(label, value, 16);
        if (!shaped) {
            return shaped;
        }

        return out.TryAssign(label.GetText());
    }

    TextPersistentView view;
    auto persistent = value.TryGetPersistentForm(view);
    return persistent ? out.TryAssign(view.source) : Result<void>::Failure(persistent.Error());
}
} // namespace

Result<void> Widget::ApplyAccessibility(ObjectRegistry& objects, Element& element, const Text& name,
                                        const Text& summary, AccessibleBehavior behavior,
                                        AccessibleBehavior summaryBehavior,
                                        bool allowChildren) noexcept {
    if (!name.IsValid() && !summary.IsValid()) {
        return element.SetAccessibility({}, {}, behavior, summaryBehavior, allowChildren);
    }

    auto outer = PresentationTree(objects);
    if (!outer) {
        return Result<void>::Failure(outer.Error());
    }

    String n(objects.ObjectAllocator()), s(objects.ObjectAllocator());
    auto first = ReadDescription(objects, outer.Value(), name, n);
    if (!first) {
        return first;
    }

    auto second = ReadDescription(objects, outer.Value(), summary, s);
    if (!second) {
        return second;
    }

    return element.SetAccessibility(n.View(), s.View(), behavior, summaryBehavior, allowChildren);
}

const Text& Widget::GetAccessibleText() const noexcept {
    return accessibleText_;
}

const Text& Widget::GetAccessibleSummaryText() const noexcept {
    return accessibleSummaryText_;
}

Result<void> Widget::SetAccessibleText(ObjectRegistry& objects, const Text& name,
                                       const Text& summary) noexcept {
    auto n = name.DeepClone(objects.ObjectAllocator());
    if (!n) {
        return Result<void>::Failure(n.Error());
    }

    auto s = summary.DeepClone(objects.ObjectAllocator());
    if (!s) {
        return Result<void>::Failure(s.Error());
    }

    if (auto live = CachedElement()) {
        auto set = ApplyAccessibility(objects, *live, name, summary, AccessibleBehavior::Custom,
                                      AccessibleBehavior::Custom, childrenAccessible_);
        if (!set) {
            return set;
        }
    }

    accessibleText_ = Move(n.Value());
    accessibleSummaryText_ = Move(s.Value());
    accessibleBehavior_ = static_cast<uint8>(AccessibleBehavior::Custom);
    accessibleSummaryBehavior_ = accessibleBehavior_;
    return Result<void>::Success();
}

Result<void> Widget::SetAccessibleBehavior(ObjectRegistry& objects, AccessibleBehavior name,
                                           AccessibleBehavior summary,
                                           bool allowChildren) noexcept {
    if (name > AccessibleBehavior::Excluded || summary > AccessibleBehavior::Excluded) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto live = CachedElement()) {
        auto set = ApplyAccessibility(objects, *live, accessibleText_, accessibleSummaryText_, name,
                                      summary, allowChildren);
        if (!set) {
            return set;
        }
    }

    accessibleBehavior_ = static_cast<uint8>(name);
    accessibleSummaryBehavior_ = static_cast<uint8>(summary);
    childrenAccessible_ = allowChildren;
    return Result<void>::Success();
}
} // namespace rocks::ui
