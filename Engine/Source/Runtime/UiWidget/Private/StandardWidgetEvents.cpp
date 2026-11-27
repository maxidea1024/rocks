#include <UiWidget/WidgetEventCatalog.h>
#include <UiWidget/WidgetEventIds.h>
#include <UiWidget/ButtonWidget.generated.h>
#include <UiWidget/CheckBoxWidget.generated.h>
#include <UiWidget/ComboBoxWidgetBase.generated.h>
#include <UiWidget/EditableTextWidget.generated.h>
#include <UiWidget/ExpandableAreaWidget.generated.h>
#include <UiWidget/InputChordValue.generated.h>
#include <UiWidget/InputKeySelectorWidget.generated.h>
#include <UiWidget/RadialSliderWidget.generated.h>
#include <UiWidget/ScrollBarWidget.generated.h>
#include <UiWidget/SliderWidget.generated.h>
#include <UiWidget/SpinBoxWidget.generated.h>

namespace rocks::ui {

Result<WidgetEventCatalog> WidgetEventCatalog::CreateStandard(Allocator& allocator) noexcept {
    WidgetEventCatalog catalog(allocator);
    auto registered = catalog.RegisterEvent(
        widgetEvents::ButtonClicked, widgetSignatures::NoArguments, &ButtonWidget::OnClicked);
    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::CheckBoxChanged, widgetSignatures::Boolean,
                                           &CheckBoxWidget::OnChanged);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::CheckBoxStateChanged, widgetSignatures::CheckState,
                                  &CheckBoxWidget::OnStateChanged);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::ComboSelectionChanged,
                                           widgetSignatures::IndexWithCause,
                                           &ComboBoxWidgetBase::OnSelectionChanged);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::TextChanged, widgetSignatures::String,
                                           &EditableTextWidget::OnChanged);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::TextCommitted, widgetSignatures::TextCommit,
                                  &EditableTextWidget::OnCommitted);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ExpansionChanged, widgetSignatures::Boolean,
                                  &ExpandableAreaWidget::OnExpansionChanged);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::InputKeySelected, widgetSignatures::KeyChord,
                                  &InputKeySelectorWidget::OnSelected);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::RadialSliderChanged, widgetSignatures::Number,
                                  &RadialSliderWidget::OnChanged);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::ScrollBarScrolled,
                                           widgetSignatures::Number, &ScrollBarWidget::OnScrolled);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SliderChanged, widgetSignatures::Number,
                                           &SliderWidget::OnChanged);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SpinBoxChanged, widgetSignatures::Number,
                                           &SpinBoxWidget::OnChanged);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::ButtonPressed,
                                           widgetSignatures::NoArguments, &ButtonWidget::OnPressed);
    }

    if (registered) {
        registered = catalog.RegisterEvent(
            widgetEvents::ButtonReleased, widgetSignatures::NoArguments, &ButtonWidget::OnReleased);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::ButtonHovered,
                                           widgetSignatures::NoArguments, &ButtonWidget::OnHovered);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::ButtonUnhovered, widgetSignatures::NoArguments,
                                  &ButtonWidget::OnUnhovered);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderCaptureBegin, widgetSignatures::NoArguments,
                                  &SliderWidget::OnCaptureBegin);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderCaptureEnd, widgetSignatures::NoArguments,
                                  &SliderWidget::OnCaptureEnd);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SpinDragBegin, widgetSignatures::NoArguments,
                                  &SpinBoxWidget::OnDragBegin);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SpinDragEnd, widgetSignatures::Number,
                                           &SpinBoxWidget::OnDragEnd);
    }

    if (registered) {
        registered = catalog.RegisterEvent(
            widgetEvents::SpinCommitted, widgetSignatures::SpinCommit, &SpinBoxWidget::OnCommitted);
    }

    if (registered) {
        registered = catalog.RegisterEvent(widgetEvents::SliderControllerBegin,
                                           widgetSignatures::NoArguments,
                                           &SliderWidget::OnControllerCaptureBegin);
    }

    if (registered) {
        registered =
            catalog.RegisterEvent(widgetEvents::SliderControllerEnd, widgetSignatures::NoArguments,
                                  &SliderWidget::OnControllerCaptureEnd);
    }

    if (!registered) {
        return Result<WidgetEventCatalog>::Failure(registered.Error());
    }

    return Result<WidgetEventCatalog>::Success(Move(catalog));
}

} // namespace rocks::ui
