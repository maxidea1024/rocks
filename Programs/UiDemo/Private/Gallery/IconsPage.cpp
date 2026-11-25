#include "IconsPage.h"

#include <UiFramework/HorizontalStack.h>
#include <UiFramework/Image.h>
#include <UiFramework/ScrollView.h>
#include <UiFramework/VerticalStack.h>
#include <UiFramework/WrapStack.h>
#include <UiRender/VectorImageAtlas.h>

namespace rocks::ui::gallery {

namespace {

// UE 아이콘 격자(CoreStyle Icon16x16·Icon20x20·Icon32x32 — 14장 Icon sizes)다.
constexpr float SmallIcon = 16;
constexpr float MediumIcon = 20;
constexpr float LargeIcon = 32;
// 견본 칸 폭·간격, 페이지 여백이다.
constexpr float CellWidth = 236;
constexpr float CellPadding = 6;
constexpr float IconGap = 10;
constexpr float PagePadding = 32;

// 아이콘 하나를 크기·tint로 그리는 Image다.
Result<SharedPtr<Element>> MakeIcon(GalleryContext& context, VectorImageKey key, float size,
                                    ThemeSlot tint) noexcept {
    auto image = SharedPtr<Image>::TryMake(context.allocator, context.allocator);
    if (!image) {
        return Result<SharedPtr<Element>>::Failure(image.Error());
    }

    auto set =
        image.Value()->SetBrush(Brush::Vector(key, {size, size}, DefaultThemeColors().Get(tint)));
    if (!set) {
        return Result<SharedPtr<Element>>::Failure(set.Error());
    }

    return Sized(context, image.Value(), {size, size});
}

// 견본 칸: [16][20][32] Foreground, [20] Primary, [20] Warning, 그 아래 키 경로.
Result<SharedPtr<Element>> MakeCell(GalleryContext& context, VectorImageKey key,
                                    StringView path) noexcept {
    Allocator& allocator = context.allocator;
    auto row = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    auto column = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto name = MakeText(context, path, SmallFont, ColorRef::Specified(LabelColor));
    if (const ErrorCode error = FirstError(row, column, name); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    struct Sample {
        float size;
        ThemeSlot tint;
    };

    constexpr Sample Samples[] = {{SmallIcon, ThemeSlot::Foreground},
                                  {MediumIcon, ThemeSlot::Foreground},
                                  {LargeIcon, ThemeSlot::Foreground},
                                  {MediumIcon, ThemeSlot::Primary},
                                  {MediumIcon, ThemeSlot::Warning}};
    for (const Sample& sample : Samples) {
        auto icon = MakeIcon(context, key, sample.size, sample.tint);
        if (!icon) {
            return icon;
        }

        // 행 높이(가장 큰 아이콘)로 늘어나지 않게 가운데에 둔다.
        auto added = row.Value()->TryAddSlot(
            icon.Value(), {.after = IconGap, .alignment = StackAlignment::Center});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    auto result = column.Value()->TryAddSlot(
        row.Value(), {.after = CellPadding, .alignment = StackAlignment::Start});
    if (result) {
        result = column.Value()->TryAddSlot(name.Value(), {.alignment = StackAlignment::Start});
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Sized(context, column.Value(), {CellWidth, LargeIcon + CellPadding * 4});
}

} // namespace

Result<SharedPtr<Element>> BuildIconsPage(GalleryContext& context) noexcept {
    Allocator& allocator = context.allocator;
    auto page = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
    auto scroll = SharedPtr<ScrollView>::TryMake(allocator, allocator);
    auto wrap = SharedPtr<WrapStack>::TryMake(allocator, allocator);
    auto title = MakeSectionTitle(context, "ICONS (SVG, Engine/Content/Ui/Icons)");
    if (const ErrorCode error = FirstError(page, scroll, wrap, title); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    wrap.Value()->SetUseAllottedSize(true);
    const VectorImageAtlas* icons = context.icons;
    const usize count = icons ? icons->ImageCount() : 0;
    for (usize i = 0; i < count; ++i) {
        auto cell = MakeCell(context, icons->ImageKey(i), icons->ImagePath(i));
        if (!cell) {
            return cell;
        }

        auto added = wrap.Value()->TryAddSlot(
            cell.Value(),
            {.placement = {.padding = {CellPadding, CellPadding, CellPadding, CellPadding}}});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    auto result = page.Value()->TryAddSlot(
        title.Value(), {.after = PagePadding / 2, .alignment = StackAlignment::Start});
    if (result) {
        result = page.Value()->TryAddSlot(wrap.Value(), {});
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    auto padded =
        PadContent(context, page.Value(), {PagePadding, PagePadding, PagePadding, PagePadding});
    if (!padded) {
        return padded;
    }

    if (auto added = scroll.Value()->TryAddChild(padded.Value()); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return Result<SharedPtr<Element>>::Success(scroll.Value());
}

} // namespace rocks::ui::gallery
