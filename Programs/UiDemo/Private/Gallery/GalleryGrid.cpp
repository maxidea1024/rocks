#include "GalleryGrid.h"

#include <UiFramework/CheckBox.h>
#include <UiFramework/HorizontalStack.h>
#include <UiFramework/ScrollView.h>
#include <UiFramework/VerticalStack.h>

namespace rocks::ui::gallery {

namespace {

// UE NextSlot의 칸 여백이다(체크 12, 이름 (24, 16, 12, 16), 위젯 (12, 16, 12, 16)).
constexpr Vec4 TogglePadding{12, 12, 12, 12};
constexpr Vec4 NamePadding{24, 16, 12, 16};
constexpr Vec4 WidgetPadding{12, 16, 12, 16};

} // namespace

GalleryGrid::GalleryGrid(GalleryContext& context) noexcept
    : context_(context), contents_(context.allocator), toggles_(context.allocator) {
    auto grid = SharedPtr<Grid>::TryMake(context.allocator, context.allocator);
    if (!grid) {
        status_ = Result<void>::Failure(grid.Error());
        return;
    }

    grid_ = Move(grid.Value());
}

void GalleryGrid::AddRow(StringView name, StringView rocksName,
                         Span<const SharedPtr<Element>> widgets, float spacing) noexcept {
    if (status_) {
        status_ = TryAddRow(name, rocksName, widgets, spacing);
    }
}

void GalleryGrid::AddRow(StringView name, StringView rocksName,
                         const SharedPtr<Element>& widget) noexcept {
    AddRow(name, rocksName, Span<const SharedPtr<Element>>(&widget, 1));
}

Result<void> GalleryGrid::TryAddRow(StringView name, StringView rocksName,
                                    Span<const SharedPtr<Element>> widgets,
                                    float spacing) noexcept {
    Allocator& allocator = context_.allocator;
    auto row = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    auto toggle = SharedPtr<CheckBox>::TryMake(allocator, allocator);
    auto names = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto label = MakeText(context_, name, NormalFont, ColorRef::Specified(LabelColor));
    auto rocks = MakeText(context_, rocksName, SmallFont, ColorRef::FromTheme(ThemeSlot::White25));
    if (const ErrorCode error = FirstError(row, toggle, names, label, rocks);
        error != ErrorCode::None) {
        return Result<void>::Failure(error);
    }

    for (usize i = 0; i < widgets.Size(); ++i) {
        if (!widgets[i]) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto added = row.Value()->TryAddSlot(
            widgets[i], {.before = i == 0 ? 0.0f : spacing, .alignment = StackAlignment::Center});
        if (!added) {
            return added;
        }
    }

    auto added = names.Value()->TryAddSlot(label.Value(), {.alignment = StackAlignment::End});
    if (added) {
        added = names.Value()->TryAddSlot(rocks.Value(), {.alignment = StackAlignment::End});
    }

    // 체크 상자는 행의 활성 상태를 그대로 보여 주고 바꾼다(UE
    // IsChecked_Lambda·OnCheckStateChanged). 행이 사라져도 콜백이 매달리지 않게 약한 참조로 잡는다.
    if (added) {
        added = toggle.Value()->SetState(CheckState::Checked);
    }

    if (!added) {
        return added;
    }

    toggle.Value()->SetOnChanged(Delegate<void(CheckState)>::CreateLambda(
        [content = WeakPtr<HorizontalStack>(row.Value())](CheckState state) noexcept {
            if (auto locked = content.Lock()) {
                locked->SetEnabled(state == CheckState::Checked);
            }
        }));

    const uint32 column = (count_ / RowsPerColumn) * ColumnsPerRow;
    const uint32 line = count_ % RowsPerColumn;
    added = grid_->TryAddSlot(toggle.Value(), {.column = column,
                                               .row = line,
                                               .padding = TogglePadding,
                                               .horizontal = StackAlignment::Start,
                                               .vertical = StackAlignment::Center});
    if (added) {
        added = grid_->TryAddSlot(names.Value(), {.column = column + 1,
                                                  .row = line,
                                                  .padding = NamePadding,
                                                  .horizontal = StackAlignment::End,
                                                  .vertical = StackAlignment::Center});
    }

    if (added) {
        added = grid_->TryAddSlot(row.Value(), {.column = column + 2,
                                                .row = line,
                                                .padding = WidgetPadding,
                                                .horizontal = StackAlignment::Start,
                                                .vertical = StackAlignment::Center});
    }

    if (added) {
        added = contents_.TryPushBack(row.Value());
    }

    if (added) {
        added = toggles_.TryPushBack(toggle.Value());
    }

    if (!added) {
        return added;
    }

    ++count_;
    return Result<void>::Success();
}

Result<SharedPtr<Element>> GalleryGrid::Finish() noexcept {
    if (!status_) {
        return Result<SharedPtr<Element>>::Failure(status_.Error());
    }

    auto scroll = SharedPtr<ScrollView>::TryMake(context_.allocator, context_.allocator);
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }

    scroll.Value()->SetClipToBounds(true);
    if (auto added = scroll.Value()->TryAddChild(grid_); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return Result<SharedPtr<Element>>::Success(scroll.Value());
}

usize GalleryGrid::RowCount() const noexcept {
    return contents_.Size();
}

SharedPtr<Element> GalleryGrid::RowContent(usize row) const noexcept {
    return row < contents_.Size() ? contents_[row] : SharedPtr<Element>{};
}

SharedPtr<Element> GalleryGrid::RowToggle(usize row) const noexcept {
    return row < toggles_.Size() ? toggles_[row] : SharedPtr<Element>{};
}

} // namespace rocks::ui::gallery
