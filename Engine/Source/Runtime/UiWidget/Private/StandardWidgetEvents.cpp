#include <UiWidget/WidgetEventCatalog.h>
#include <UiWidget/WidgetEventIds.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/ComboBoxWidgetBase.generated.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/ExpandableAreaWidget.generated.h>
#include <UiWidget/InputKeySelectorWidget.generated.h>
#include <UiWidget/RadialSliderWidget.generated.h>
#include <UiWidget/ScrollBarWidget.generated.h>
#include <UiWidget/SliderWidget.generated.h>
#include <UiWidget/SpinBoxWidget.generated.h>

namespace rocks::ui {

Result<WidgetEventCatalog> WidgetEventCatalog::CreateStandard(Allocator& allocator) noexcept {
    WidgetEventCatalog catalog(allocator);
    auto registered = catalog.RegisterEvent(
        widgetEvents::ButtonClicked, widgetSignatures::NoArguments, &ButtonWidget::SetOnClicked);
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::CheckBoxChanged, widgetSignatures::Boolean,
                                           &CheckBoxWidget::SetOnChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::CheckBoxStateChanged, widgetSignatures::CheckState,
                                  &CheckBoxWidget::SetOnStateChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ComboSelectionChanged, widgetSignatures::Integer,
                                  &ComboBoxWidgetBase::SetOnSelectionChanged);
    }
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::TextChanged, widgetSignatures::String,
                                           &EditableTextWidget::SetOnChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::TextCommitted, widgetSignatures::TextCommit,
                                  &EditableTextWidget::SetOnCommitted);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ExpansionChanged, widgetSignatures::Boolean,
                                  &ExpandableAreaWidget::SetOnExpansionChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::InputKeySelected, widgetSignatures::KeyChord,
                                  &InputKeySelectorWidget::SetOnSelected);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::RadialSliderChanged, widgetSignatures::Number,
                                  &RadialSliderWidget::SetOnChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ScrollBarScrolled, widgetSignatures::Number,
                                  &ScrollBarWidget::SetOnScrolled);
    }
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SliderChanged, widgetSignatures::Number,
                                           &SliderWidget::SetOnChanged);
    }
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SpinBoxChanged, widgetSignatures::Number,
                                           &SpinBoxWidget::SetOnChanged);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ButtonPressed, widgetSignatures::NoArguments,
                                  &ButtonWidget::SetOnPressed);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ButtonReleased, widgetSignatures::NoArguments,
                                  &ButtonWidget::SetOnReleased);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ButtonHovered, widgetSignatures::NoArguments,
                                  &ButtonWidget::SetOnHovered);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ButtonUnhovered, widgetSignatures::NoArguments,
                                  &ButtonWidget::SetOnUnhovered);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderCaptureBegin, widgetSignatures::NoArguments,
                                  &SliderWidget::SetOnCaptureBegin);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderCaptureEnd, widgetSignatures::NoArguments,
                                  &SliderWidget::SetOnCaptureEnd);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SpinDragBegin, widgetSignatures::NoArguments,
                                  &SpinBoxWidget::SetOnDragBegin);
    }
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SpinDragEnd, widgetSignatures::Number,
                                           &SpinBoxWidget::SetOnDragEnd);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SpinCommitted, widgetSignatures::SpinCommit,
                                  &SpinBoxWidget::SetOnCommitted);
    }
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SliderControllerBegin,
                                           widgetSignatures::NoArguments,
                                           &SliderWidget::SetOnControllerCaptureBegin);
    }
    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderControllerEnd, widgetSignatures::NoArguments,
                                  &SliderWidget::SetOnControllerCaptureEnd);
    }
    if (!registered) {
        return Result<WidgetEventCatalog>::Failure(registered.Error());
    }
    return Result<WidgetEventCatalog>::Success(Move(catalog));
}

} // namespace rocks::ui
