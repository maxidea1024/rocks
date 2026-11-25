#include "EntryPatternPanel.h"
#include "MakeWidgetElement.h"

#include <UiFramework/StackPanel.h>
#include <UiFramework/WrapStack.h>
#include <UiWidget/DynamicEntryBoxBaseWidget.h>
#include <UiWidget/WidgetSlot.generated.h>

namespace rocks::ui {

namespace {

Result<void> ApplySpacing(Element& element, DynamicBoxType type, Vec2 spacing) noexcept {
    switch (type) {
    case DynamicBoxType::Vertical:
    case DynamicBoxType::Horizontal:
        return static_cast<StackPanel&>(element).SetSpacing(
            type == DynamicBoxType::Vertical ? spacing.y : spacing.x);
    case DynamicBoxType::Wrap:
    case DynamicBoxType::VerticalWrap:
        return static_cast<WrapStack&>(element).SetSpacing(spacing);
    case DynamicBoxType::Overlay:
    case DynamicBoxType::Radial:
        static_cast<EntryPatternPanel&>(element).SetSpacing(spacing);
        return Result<void>::Success();
    }
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

} // namespace

DynamicEntryBoxBaseWidget::DynamicEntryBoxBaseWidget(Allocator& allocator) noexcept
    : PanelWidget(allocator), spacingPattern_(allocator) {
}

DynamicBoxType DynamicEntryBoxBaseWidget::BoxType() const noexcept {
    return static_cast<DynamicBoxType>(boxType_);
}

Result<void> DynamicEntryBoxBaseWidget::SetBoxType(DynamicBoxType type) noexcept {
    if (type > DynamicBoxType::Radial ||
        (type != DynamicBoxType::Overlay && (entrySpacing_.x < 0 || entrySpacing_.y < 0))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (type != BoxType() && CachedElement()) {
        return Result<void>::Failure(ErrorCode::Busy);
    }
    boxType_ = static_cast<uint8>(type);
    return Result<void>::Success();
}

Vec2 DynamicEntryBoxBaseWidget::EntrySpacing() const noexcept {
    return entrySpacing_;
}

Result<void> DynamicEntryBoxBaseWidget::SetEntrySpacing(Vec2 spacing) noexcept {
    if (!spacing.IsFinite() ||
        (BoxType() != DynamicBoxType::Overlay && (spacing.x < 0 || spacing.y < 0))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (auto panel = CachedElement()) {
        auto applied = ApplySpacing(*panel, BoxType(), spacing);
        if (!applied) {
            return applied;
        }
    }
    entrySpacing_ = spacing;
    return Result<void>::Success();
}

Result<SharedPtr<Element>> DynamicEntryBoxBaseWidget::CreatePanel(Allocator& allocator) noexcept {
    const auto type = BoxType();
    auto makePanel = [&]() noexcept -> Result<SharedPtr<Element>> {
        switch (type) {
        case DynamicBoxType::Vertical:
        case DynamicBoxType::Horizontal:
            return MakeWidgetElement<StackPanel>(allocator, type == DynamicBoxType::Vertical
                                                                ? StackDirection::Vertical
                                                                : StackDirection::Horizontal);
        case DynamicBoxType::Wrap:
        case DynamicBoxType::VerticalWrap: {
            auto wrap = MakeWidgetElement<WrapStack>(allocator, type == DynamicBoxType::Wrap
                                                                    ? StackDirection::Horizontal
                                                                    : StackDirection::Vertical);
            if (wrap) {
                static_cast<WrapStack&>(*wrap.Value()).SetUseAllottedSize(true);
            }
            return wrap;
        }
        case DynamicBoxType::Radial:
        case DynamicBoxType::Overlay:
            return MakeWidgetElement<EntryPatternPanel>(allocator, type == DynamicBoxType::Radial);
        }
        return Result<SharedPtr<Element>>::Failure(ErrorCode::InvalidArgument);
    };
    auto panel = makePanel();
    if (panel && (type == DynamicBoxType::Radial || type == DynamicBoxType::Overlay)) {
        auto& patterned = static_cast<EntryPatternPanel&>(*panel.Value());
        if (!radialSettings_.IsValid()) {return Result<SharedPtr<Element>>::Failure(ErrorCode::InvalidArgument);}
        patterned.SetRadialSettings(radialSettings_);
        auto copied = patterned.SetPattern(spacingPattern_.View());
        if (!copied) {return Result<SharedPtr<Element>>::Failure(copied.Error());}
    }
    if (panel) {
        auto applied = ApplySpacing(*panel.Value(), type, entrySpacing_);
        if (!applied) {
            return Result<SharedPtr<Element>>::Failure(applied.Error());
        }
    }
    return panel;
}

Result<void> DynamicEntryBoxBaseWidget::SetSpacingPattern(Span<const Vec2> steps) noexcept {
    for(const auto& step: steps) {if(!step.IsFinite()){return Result<void>::Failure(ErrorCode::InvalidArgument);}}
    Array<Vec2> staged(spacingPattern_.GetAllocator());
    auto copied = staged.TryAppend(steps);
    if (!copied) {return copied;}
    if (auto live = CachedElement(); live && BoxType() == DynamicBoxType::Overlay) {
        auto applied = static_cast<EntryPatternPanel&>(*live).SetPattern(staged.View());
        if (!applied) {return applied;}
    }
    spacingPattern_ = Move(staged);
    return Result<void>::Success();
}
Span<const Vec2> DynamicEntryBoxBaseWidget::GetSpacingPattern() const noexcept {
    return spacingPattern_.View();
}
Result<void> DynamicEntryBoxBaseWidget::SetRadialSettings(const RadialBoxSettings& settings) noexcept {
    if (!settings.IsValid()) {return Result<void>::Failure(ErrorCode::InvalidArgument);}
    if (auto live = CachedElement(); live && BoxType() == DynamicBoxType::Radial) {
        static_cast<EntryPatternPanel&>(*live).SetRadialSettings(settings);
    }
    radialSettings_ = settings;
    return Result<void>::Success();
}
const RadialBoxSettings& DynamicEntryBoxBaseWidget::GetRadialSettings() const noexcept {return radialSettings_;}

Result<ObjectHandle> DynamicEntryBoxBaseWidget::EntryAt(ObjectRegistry& objects,
                                                        usize index) const noexcept {
    if (index >= ChildCount()) {
        return Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    }
    auto slot = objects.Resolve<WidgetSlot>(objects.OwnerAccess(), SlotAt(index));
    return slot ? Result<ObjectHandle>::Success(slot.Value().Get().Value()->Content())
                : Result<ObjectHandle>::Failure(slot.Error());
}

Result<void> DynamicEntryBoxBaseWidget::ResetEntries(ObjectRegistry& objects) noexcept {
    while (ChildCount() > 0) {
        auto entry = EntryAt(objects, ChildCount() - 1);
        if (!entry) {
            return Result<void>::Failure(entry.Error());
        }
        auto removed = RemoveChild(objects, entry.Value());
        if (!removed) {
            return removed;
        }
    }
    return Result<void>::Success();
}

} // namespace rocks::ui
