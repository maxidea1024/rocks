#include "WidgetPropertyAccess.h"
#include <UiWidget/Widget.generated.h>
#include <UiWidget/UserWidget.generated.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/ProgressBarWidget.generated.h>
#include <UiWidget/SliderWidget.generated.h>
#include <UiWidget/SpinBoxWidget.generated.h>
#include <UiWidget/RadialSliderWidget.generated.h>
#include <UiWidget/ExpandableAreaWidget.generated.h>
#include <UiWidget/TextLabelWidget.generated.h>
#include <UiWidget/WidgetSwitcherWidget.generated.h>

namespace rocks::ui {
namespace {

template <class Owner, auto Setter>
WidgetPropertyAccess MakeAccess(bool notifies = true) noexcept {
    return {[](ConstructionContext& stage, ObjectHandle handle, const FieldValue& value,
               Allocator& allocator) noexcept -> Result<void> {
                auto object = stage.GetForWrite<Owner>(handle);
                if (!object) {
                    return Result<void>::Failure(object.Error());
                }
                return ApplyWidgetSetter(*object.Value(), Setter, value, allocator);
            },
            []() noexcept { return WidgetSetter(Setter); }, notifies};
}

struct PropertyAccessor {
    TypeId type;
    StringView name;
    WidgetPropertyAccess access;
};
} // namespace

Result<WidgetPropertyAccess> ResolveWidgetProperty(const TypeLease& widget, Guid id) noexcept {
    const auto index = widget.FieldIndex(id);
    if (index == TypeLease::NoField) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::NotFound);
    }
    const auto& field = widget.Schema().fields[index];
    const auto name = field.toolName.IsEmpty() ? field.name : field.toolName;
    // ID로 찾은 스키마의 선언 타입과 도구 이름을 엔진의 실제 setter에 대응시킨다.
    // 알 수 없는 속성은 쓰기를 거부해 캐시된 요소와 Object 값이 어긋나지 않게 한다.
    static const PropertyAccessor accessors[] = {
        {GeneratedType<Widget>::Id(), "enabled", MakeAccess<Widget, &Widget::SetEnabled>()},
        {GeneratedType<Widget>::Id(), "forceVolatile",
         MakeAccess<Widget, &Widget::SetForceVolatile>()},
        {GeneratedType<Widget>::Id(), "renderOpacity",
         MakeAccess<Widget, &Widget::SetRenderOpacity>()},
        {GeneratedType<UserWidget>::Id(), "focusable",
         MakeAccess<UserWidget, &UserWidget::SetFocusable>(false)},
        {GeneratedType<EditableTextWidget>::Id(), "text",
         MakeAccess<EditableTextWidget, &EditableTextWidget::SetText>()},
        {GeneratedType<EditableTextWidget>::Id(), "fontSize",
         MakeAccess<EditableTextWidget, &EditableTextWidget::SetFontSize>()},
        {GeneratedType<EditableTextWidget>::Id(), "password",
         MakeAccess<EditableTextWidget, &EditableTextWidget::SetPassword>()},
        {GeneratedType<EditableTextWidget>::Id(), "readOnly",
         MakeAccess<EditableTextWidget, &EditableTextWidget::SetReadOnly>()},
        {GeneratedType<CheckBoxWidget>::Id(), "checked",
         MakeAccess<CheckBoxWidget, &CheckBoxWidget::SetChecked>()},
        {GeneratedType<ProgressBarWidget>::Id(), "value",
         MakeAccess<ProgressBarWidget, &ProgressBarWidget::SetValue>()},
        {GeneratedType<ProgressBarWidget>::Id(), "marquee",
         MakeAccess<ProgressBarWidget, &ProgressBarWidget::SetMarquee>()},
        {GeneratedType<SliderWidget>::Id(), "value",
         MakeAccess<SliderWidget, &SliderWidget::SetValue>()},
        {GeneratedType<SpinBoxWidget>::Id(), "value",
         MakeAccess<SpinBoxWidget, &SpinBoxWidget::SetValue>(false)},
        {GeneratedType<RadialSliderWidget>::Id(), "value",
         MakeAccess<RadialSliderWidget, &RadialSliderWidget::SetValue>(false)},
        {GeneratedType<ExpandableAreaWidget>::Id(), "expanded",
         MakeAccess<ExpandableAreaWidget, &ExpandableAreaWidget::SetExpanded>()},
        {GeneratedType<TextLabelWidget>::Id(), "text",
         MakeAccess<TextLabelWidget, &TextLabelWidget::SetText>()},
        {GeneratedType<TextLabelWidget>::Id(), "fontSize",
         MakeAccess<TextLabelWidget, &TextLabelWidget::SetFontSize>()},
        {GeneratedType<WidgetSwitcherWidget>::Id(), "activeIndex",
         MakeAccess<WidgetSwitcherWidget, &WidgetSwitcherWidget::SetActiveIndex>(false)},
    };
    for (const auto& accessor : accessors) {
        if (accessor.name == StringView(name.Data(), name.Size()) && widget.IsA(accessor.type)) {
            return Result<WidgetPropertyAccess>::Success(accessor.access);
        }
    }
    return Result<WidgetPropertyAccess>::Failure(ErrorCode::Unsupported);
}

Result<WidgetPropertyAccess>
ValidateWidgetPropertyBinding(const TypeLease& source, const TypeLease& target,
                              const WidgetPropertyBinding& binding) noexcept {
    const usize from = source.FieldIndex(binding.sourceField);
    const usize to = target.FieldIndex(binding.targetField);
    if (from == TypeLease::NoField || to == TypeLease::NoField) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::NotFound);
    }
    const auto& sourceValue = source.Schema().fields[from].value;
    const auto& targetValue = target.Schema().fields[to].value;
    if (sourceValue.kind != targetValue.kind || sourceValue.type != targetValue.type) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::TypeMismatch);
    }
    const auto* ops = source.FieldOps(from);
    if (!ops || !ops->read) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::Unsupported);
    }
    auto forward = ResolveWidgetProperty(target, binding.targetField);
    if (!forward || !binding.twoWay) {
        return forward;
    }
    if (binding.once || !forward.Value().notifies) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::Unsupported);
    }
    if (source.IsNativeA(NativeTypeKey<Widget>())) {
        auto reverse = ResolveWidgetProperty(source, binding.sourceField);
        if (!reverse || !reverse.Value().notifies) {
            return Result<WidgetPropertyAccess>::Failure(ErrorCode::Unsupported);
        }
    } else if (HasFlag(source.Schema().fields[from].flags, FieldFlags::ReadOnly) || !ops->write) {
        return Result<WidgetPropertyAccess>::Failure(ErrorCode::Unsupported);
    }
    return forward;
}

} // namespace rocks::ui
