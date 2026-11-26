#include "LayoutPage.h"

#include <Core/Text/StringBuilder.h>
#include <UiFramework/AnchorCanvas.h>
#include <UiFramework/Carousel.h>
#include <UiFramework/CarouselPage.h>
#include <UiFramework/FitBox.h>
#include <UiFramework/Frame.h>
#include <UiFramework/Grid.h>
#include <UiFramework/HorizontalStack.h>
#include <UiFramework/Overlay.h>
#include <UiFramework/WindowTitleBar.h>
#include <UiFramework/ScaleLayer.h>
#include <UiFramework/ScrollView.h>
#include <UiFramework/SizeBox.h>
#include <UiFramework/Switcher.h>
#include <UiFramework/UniformGrid.h>
#include <UiFramework/VerticalStack.h>
#include <UiFramework/WindowTitleBarArea.h>
#include <UiFramework/WrapStack.h>

namespace rocks::ui::gallery {

namespace {

// 무대(견본을 담는 테두리 상자) 크기다.
constexpr Vec2 WideStage{320, 44};
constexpr Vec2 TallStage{140, 150};
constexpr Vec2 GridStage{320, 120};
constexpr Vec2 UniformStage{320, 80};
constexpr Vec2 WrapStage{320, 110};
constexpr Vec2 OverlayStage{200, 80};
constexpr Vec2 FitStage{110, 70};
constexpr Vec2 CanvasStage{320, 120};
constexpr Vec2 ScrollStage{200, 100};
constexpr Vec2 HorizontalScrollStage{200, 44};
constexpr Vec2 SwitchStage{160, 60};
constexpr Vec2 DockStage{520, 260};
// 무대 테두리·안쪽 여백·상자 반경이다.
constexpr float StageRadius = 4;
constexpr float StagePadding = 6;
// 견본 상자 크기다.
constexpr Vec2 FitContentSize{100, 40};
constexpr Vec2 BadgeSize{56, 22};
constexpr Vec2 CanvasBlockSize{70, 24};
constexpr float WrapExtent = 300;
constexpr Vec2 WrapSpacing{4, 4};
constexpr usize WrapBlockCount = 12;
constexpr usize ScrollRowCount = 12;
constexpr float UniformCellPadding = 2;
constexpr float SpacedSlot = 4;
constexpr float SmallScale = 0.75f;
constexpr float LargeScale = 1.25f;
constexpr float CarouselSpeed = 4;
constexpr float DockSideFraction = 0.3f;
constexpr float DockBottomFraction = 0.35f;
// 전환 견본의 페이지 이름이다.
constexpr const char* Pages[] = {"Page 1", "Page 2", "Page 3"};
constexpr usize PageCount = sizeof(Pages) / sizeof(Pages[0]);
// 상자 색 순서다(테마 강조색).
constexpr ThemeSlot BlockColors[] = {ThemeSlot::AccentBlue,   ThemeSlot::AccentGreen,
                                     ThemeSlot::AccentOrange, ThemeSlot::AccentPurple,
                                     ThemeSlot::AccentPink,   ThemeSlot::AccentYellow};
constexpr usize BlockColorCount = sizeof(BlockColors) / sizeof(BlockColors[0]);

// 색 상자 하나(둥근 채움 + 어두운 레이블)다.
Result<SharedPtr<Element>> MakeBlock(GalleryContext& context, StringView label,
                                     usize colorIndex) noexcept {
    auto block = MakeBlock(context, label, BlockColors[colorIndex % BlockColorCount]);
    if (!block) {
        return Result<SharedPtr<Element>>::Failure(block.Error());
    }

    return Result<SharedPtr<Element>>::Success(block.Value());
}

// 테두리를 그린 고정 크기 무대에 content를 담는다(Recessed 채움 + Dropdown 외곽선).
Result<SharedPtr<Element>> MakeStage(GalleryContext& context, const SharedPtr<Element>& content,
                                     Vec2 size) noexcept {
    auto frame = SharedPtr<Frame>::TryMake(context.allocator, context.allocator);
    if (!frame) {
        return Result<SharedPtr<Element>>::Failure(frame.Error());
    }

    const ThemeColors& theme = DefaultThemeColors();
    auto result = frame.Value()->Construct(
        {.content = content,
         .brush = Brush::RoundedBox(theme.Get(ThemeSlot::Recessed), StageRadius,
                                    theme.Get(ThemeSlot::Dropdown), 1.0f),
         .padding = {StagePadding, StagePadding, StagePadding, StagePadding}});
    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Sized(context, frame.Value(), size);
}

template <typename T>
Result<SharedPtr<Element>> Up(Result<SharedPtr<T>>&& made) noexcept {
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    return Result<SharedPtr<Element>>::Success(made.Value());
}

Result<SharedPtr<Element>> MakeStack(GalleryContext& context, bool horizontal) noexcept {
    SharedPtr<StackPanel> stack;
    if (horizontal) {
        auto made = SharedPtr<HorizontalStack>::TryMake(context.allocator, context.allocator);
        if (!made) {
            return Result<SharedPtr<Element>>::Failure(made.Error());
        }

        stack = made.Value();
    } else {
        auto made = SharedPtr<VerticalStack>::TryMake(context.allocator, context.allocator);
        if (!made) {
            return Result<SharedPtr<Element>>::Failure(made.Error());
        }

        stack = made.Value();
    }

    struct Part {
        const char* label;
        StackSlot slot;
    };

    const Part parts[] = {
        {"Auto", {.after = SpacedSlot}},
        {"Auto", {.after = SpacedSlot}},
        {"Fill 1", {.rule = StackSizeRule::Fill, .weight = 1, .after = SpacedSlot}},
        {"Fill 2", {.rule = StackSizeRule::Fill, .weight = 2}}};
    for (usize i = 0; i < sizeof(parts) / sizeof(parts[0]); ++i) {
        auto block = MakeBlock(context, parts[i].label, i);
        if (!block) {
            return block;
        }

        auto added = stack->TryAddSlot(block.Value(), parts[i].slot);
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    return MakeStage(context, stack, horizontal ? WideStage : TallStage);
}

Result<SharedPtr<Element>> MakeGrid(GalleryContext& context) noexcept {
    auto grid = SharedPtr<Grid>::TryMake(context.allocator, context.allocator);
    if (!grid) {
        return Result<SharedPtr<Element>>::Failure(grid.Error());
    }

    struct Cell {
        const char* label;
        GridSlot slot;
    };

    constexpr Vec4 Gap{2, 2, 2, 2};
    const Cell cells[] = {
        {"span 2", {.column = 0, .row = 0, .columnSpan = 2, .padding = Gap}},
        {"0,2", {.column = 2, .row = 0, .padding = Gap}},
        {"1,0", {.column = 0, .row = 1, .padding = Gap}},
        {"1,1 span 2 rows", {.column = 1, .row = 1, .rowSpan = 2, .padding = Gap}},
        {"1,2", {.column = 2, .row = 1, .padding = Gap}},
        {"2,0", {.column = 0, .row = 2, .padding = Gap}},
        {"2,2", {.column = 2, .row = 2, .padding = Gap}}};
    for (usize i = 0; i < sizeof(cells) / sizeof(cells[0]); ++i) {
        auto block = MakeBlock(context, cells[i].label, i);
        if (!block) {
            return block;
        }

        auto added = grid.Value()->TryAddSlot(block.Value(), cells[i].slot);
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    // 가운데 열이 남은 폭을 더 받는다(채움 비율 1 : 2 : 1).
    auto filled = grid.Value()->SetColumnFill(0, 1);
    if (filled) {
        filled = grid.Value()->SetColumnFill(1, 2);
    }

    if (filled) {
        filled = grid.Value()->SetColumnFill(2, 1);
    }

    if (!filled) {
        return Result<SharedPtr<Element>>::Failure(filled.Error());
    }

    return MakeStage(context, grid.Value(), GridStage);
}

Result<SharedPtr<Element>> MakeUniformGrid(GalleryContext& context) noexcept {
    auto grid = SharedPtr<UniformGrid>::TryMake(context.allocator, context.allocator);
    if (!grid) {
        return Result<SharedPtr<Element>>::Failure(grid.Error());
    }

    constexpr uint32 Columns = 4;
    constexpr uint32 Rows = 2;
    auto padded = grid.Value()->SetCellPadding(
        {UniformCellPadding, UniformCellPadding, UniformCellPadding, UniformCellPadding});
    if (!padded) {
        return Result<SharedPtr<Element>>::Failure(padded.Error());
    }

    StringBuilder<> label(context.allocator);
    for (uint32 row = 0; row < Rows; ++row) {
        for (uint32 column = 0; column < Columns; ++column) {
            label.Clear();
            auto formatted = label.TryAppendFormat("{},{}", row, column);
            auto block = formatted ? MakeBlock(context, label.View(), row * Columns + column)
                                   : Result<SharedPtr<Element>>::Failure(formatted.Error());
            if (!block) {
                return block;
            }

            auto added = grid.Value()->TryAddSlot(block.Value(), row, column);
            if (!added) {
                return Result<SharedPtr<Element>>::Failure(added.Error());
            }
        }
    }

    return MakeStage(context, grid.Value(), UniformStage);
}

Result<SharedPtr<Element>> MakeWrap(GalleryContext& context) noexcept {
    auto wrap = SharedPtr<WrapStack>::TryMake(context.allocator, context.allocator);
    if (!wrap) {
        return Result<SharedPtr<Element>>::Failure(wrap.Error());
    }

    constexpr const char* Labels[] = {"Wrap", "Box",  "keeps", "adding", "items", "to",
                                      "the",  "line", "until", "it",     "is",    "full"};
    auto result = wrap.Value()->SetPreferredExtent(WrapExtent);
    if (result) {
        result = wrap.Value()->SetSpacing(WrapSpacing);
    }

    for (usize i = 0; result && i < WrapBlockCount; ++i) {
        auto block = MakeBlock(context, Labels[i], i);
        result = block ? wrap.Value()->TryAddSlot(block.Value(), {})
                       : Result<void>::Failure(block.Error());
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return MakeStage(context, wrap.Value(), WrapStage);
}

Result<SharedPtr<Element>> MakeOverlay(GalleryContext& context) noexcept {
    auto overlay = SharedPtr<Overlay>::TryMake(context.allocator, context.allocator);
    auto base = MakeBlock(context, "Base layer", 0);
    auto badge = MakeBlock(context, "Top", 2);
    if (const ErrorCode error = FirstError(overlay, base, badge); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    // 위 층은 앵커 배치로 오른쪽 위 구석에 둔다.
    auto corner = SharedPtr<AnchorCanvas>::TryMake(context.allocator, context.allocator);
    if (!corner) {
        return Result<SharedPtr<Element>>::Failure(corner.Error());
    }

    auto result =
        corner.Value()->TryAddSlot(badge.Value(), {.anchorMin = {1, 0},
                                                   .anchorMax = {1, 0},
                                                   .offset = {0, 0, BadgeSize.x, BadgeSize.y},
                                                   .alignment = {1, 0}});
    if (result) {
        result = overlay.Value()->TryAddSlot(base.Value(), 0);
    }

    if (result) {
        result = overlay.Value()->TryAddSlot(corner.Value(), 1);
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return MakeStage(context, overlay.Value(), OverlayStage);
}

Result<SharedPtr<Element>> MakeFit(GalleryContext& context, FitMode mode,
                                   StringView label) noexcept {
    auto fit = SharedPtr<FitBox>::TryMake(context.allocator, context.allocator);
    auto block = MakeBlock(context, label, static_cast<usize>(mode));
    if (const ErrorCode error = FirstError(fit, block); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    auto sized = Sized(context, block.Value(), FitContentSize);
    if (!sized) {
        return sized;
    }

    fit.Value()->SetFit(mode);
    if (auto added = fit.Value()->TryAddChild(sized.Value()); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return MakeStage(context, fit.Value(), FitStage);
}

Result<SharedPtr<Element>> MakeScaled(GalleryContext& context, float scale,
                                      StringView label) noexcept {
    auto layer = SharedPtr<ScaleLayer>::TryMake(context.allocator, context.allocator);
    auto block = MakeBlock(context, label, 3);
    if (const ErrorCode error = FirstError(layer, block); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    auto result = layer.Value()->SetScale(scale);
    if (result) {
        result = layer.Value()->TryAddChild(block.Value());
    }

    if (!result) {
        return Result<SharedPtr<Element>>::Failure(result.Error());
    }

    return Result<SharedPtr<Element>>::Success(layer.Value());
}

Result<SharedPtr<Element>> MakeCanvas(GalleryContext& context) noexcept {
    auto canvas = SharedPtr<AnchorCanvas>::TryMake(context.allocator, context.allocator);
    if (!canvas) {
        return Result<SharedPtr<Element>>::Failure(canvas.Error());
    }

    struct Anchored {
        const char* label;
        CanvasSlot slot;
    };

    const Vec2 size = CanvasBlockSize;
    const Anchored items[] = {
        {"top left",
         {.anchorMin = {0, 0},
          .anchorMax = {0, 0},
          .offset = {0, 0, size.x, size.y},
          .alignment = {0, 0}}},
        {"top right",
         {.anchorMin = {1, 0},
          .anchorMax = {1, 0},
          .offset = {0, 0, size.x, size.y},
          .alignment = {1, 0}}},
        {"bottom left",
         {.anchorMin = {0, 1},
          .anchorMax = {0, 1},
          .offset = {0, 0, size.x, size.y},
          .alignment = {0, 1}}},
        {"bottom right",
         {.anchorMin = {1, 1},
          .anchorMax = {1, 1},
          .offset = {0, 0, size.x, size.y},
          .alignment = {1, 1}}},
        {"center",
         {.anchorMin = {0.5f, 0.5f}, .anchorMax = {0.5f, 0.5f}, .offset = {0, 0, size.x, size.y}}},
    };
    for (usize i = 0; i < sizeof(items) / sizeof(items[0]); ++i) {
        auto block = MakeBlock(context, items[i].label, i);
        if (!block) {
            return block;
        }

        auto added = canvas.Value()->TryAddSlot(block.Value(), items[i].slot);
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    return MakeStage(context, canvas.Value(), CanvasStage);
}

Result<SharedPtr<Element>> MakeScroll(GalleryContext& context, bool horizontal) noexcept {
    auto scroll = SharedPtr<ScrollView>::TryMake(context.allocator, context.allocator,
                                                 horizontal ? StackDirection::Horizontal
                                                            : StackDirection::Vertical);
    if (!scroll) {
        return Result<SharedPtr<Element>>::Failure(scroll.Error());
    }

    // ScrollView는 내용 하나를 받는다. 상자들은 같은 방향 스택에 담는다.
    SharedPtr<StackPanel> list;
    if (horizontal) {
        auto made = SharedPtr<HorizontalStack>::TryMake(context.allocator, context.allocator);
        if (!made) {
            return Result<SharedPtr<Element>>::Failure(made.Error());
        }

        list = made.Value();
    } else {
        auto made = SharedPtr<VerticalStack>::TryMake(context.allocator, context.allocator);
        if (!made) {
            return Result<SharedPtr<Element>>::Failure(made.Error());
        }

        list = made.Value();
    }

    StringBuilder<> label(context.allocator);
    for (usize i = 0; i < ScrollRowCount; ++i) {
        label.Clear();
        auto formatted = label.TryAppendFormat(horizontal ? "Item {}" : "Row {}", i + 1);
        auto block = formatted ? MakeBlock(context, label.View(), i)
                               : Result<SharedPtr<Element>>::Failure(formatted.Error());
        if (!block) {
            return block;
        }

        auto added = list->TryAddSlot(block.Value(), {.after = SpacedSlot});
        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    if (auto added = scroll.Value()->TryAddChild(list); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return MakeStage(context, scroll.Value(), horizontal ? HorizontalScrollStage : ScrollStage);
}

// 단추로 바꾸는 전환 상자다(Switcher 또는 Carousel).
template <typename T>
Result<SharedPtr<Element>> MakeSwitchRow(GalleryContext& context, bool carousel) noexcept {
    Allocator& allocator = context.allocator;
    auto switcher = SharedPtr<T>::TryMake(allocator, allocator);
    auto row = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
    if (const ErrorCode error = FirstError(switcher, row); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    for (usize i = 0; i < PageCount; ++i) {
        auto block = MakeBlock(context, Pages[i], i);
        if (!block) {
            return block;
        }

        Result<void> added = Result<void>::Success();
        if (carousel) {
            // 캐러셀 페이지는 처음 보일 때 내용을 만든다(UE 대응 없음 — CarouselPage lazy 생성).
            auto page = SharedPtr<CarouselPage>::TryMake(
                allocator, allocator,
                CarouselPage::Factory::CreateLambda(
                    [target = &context, i]() noexcept { return MakeBlock(*target, Pages[i], i); }));
            added = page ? static_cast<Carousel&>(*switcher.Value()).AddPage(page.Value())
                         : Result<void>::Failure(page.Error());
        } else {
            added = switcher.Value()->TryAddChild(block.Value());
        }

        if (!added) {
            return Result<SharedPtr<Element>>::Failure(added.Error());
        }
    }

    auto stage = MakeStage(context, switcher.Value(), SwitchStage);
    if (!stage) {
        return stage;
    }

    auto added = row.Value()->TryAddSlot(stage.Value(), {.after = SpacedSlot});
    for (usize i = 0; added && i < PageCount; ++i) {
        auto button = MakeTextButton(context, Pages[i], DefaultButtonStyle());
        if (!button) {
            return Result<SharedPtr<Element>>::Failure(button.Error());
        }

        button.Value()->SetOnClicked(Delegate<void()>::CreateLambda(
            [target = WeakPtr<T>(switcher.Value()), i, carousel]() noexcept {
                auto locked = target.Lock();
                if (!locked) {
                    return;
                }

                if (carousel) {
                    (void)static_cast<Carousel&>(*locked).Show(i, true, CarouselSpeed);
                } else {
                    (void)locked->SetActiveIndex(i);
                }
            }));
        added = row.Value()->TryAddSlot(button.Value(),
                                        {.after = SpacedSlot, .alignment = StackAlignment::Center});
    }

    if (!added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return Result<SharedPtr<Element>>::Success(row.Value());
}

Result<SharedPtr<Element>> MakeTitleBar(GalleryContext& context) noexcept {
    auto area = SharedPtr<WindowTitleBarArea>::TryMake(context.allocator, context.allocator);
    auto block = MakeBlock(context, "Drag here to move the window", 0);
    if (const ErrorCode error = FirstError(area, block); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    if (auto added = area.Value()->TryAddChild(block.Value()); !added) {
        return Result<SharedPtr<Element>>::Failure(added.Error());
    }

    return MakeStage(context, area.Value(), WideStage);
}

// 탭 묶음 셋(왼쪽·가운데·아래)을 나눈 도킹 영역이다.
Result<SharedPtr<DockManager>> MakeDocking(GalleryContext& context) noexcept {
    auto dock = SharedPtr<DockManager>::TryMake(context.allocator, context.allocator);
    if (!dock) {
        return dock;
    }

    struct Panel {
        const char* id;
        DockRole role;
    };

    constexpr Panel Panels[] = {{"Outliner", DockRole::Panel},
                                {"Viewport", DockRole::Document},
                                {"Details", DockRole::Panel},
                                {"Output Log", DockRole::Panel},
                                {"Content", DockRole::Panel}};
    for (usize i = 0; i < sizeof(Panels) / sizeof(Panels[0]); ++i) {
        auto header = MakeText(context, Panels[i].id);
        auto body = MakeBlock(context, Panels[i].id, i);
        if (const ErrorCode error = FirstError(header, body); error != ErrorCode::None) {
            return Result<SharedPtr<DockManager>>::Failure(error);
        }

        auto padded = PadContent(context, body.Value(), {8, 8, 8, 8});
        if (!padded) {
            return Result<SharedPtr<DockManager>>::Failure(padded.Error());
        }

        auto registered = dock.Value()->RegisterTab(
            {Panels[i].id, header.Value(), padded.Value(), Panels[i].role, false});
        if (!registered) {
            return Result<SharedPtr<DockManager>>::Failure(registered.Error());
        }
    }

    DockManager& area = *dock.Value();
    if (!area.OpenTab("Viewport") ||
        !area.Dock("Outliner", "Viewport", DockSide::Left, DockSideFraction) ||
        !area.Dock("Details", "Viewport", DockSide::Right, DockSideFraction) ||
        !area.Dock("Output Log", "Viewport", DockSide::Bottom, DockBottomFraction) ||
        !area.Dock("Content", "Output Log", DockSide::Center)) {
        return Result<SharedPtr<DockManager>>::Failure(ErrorCode::InvalidArgument);
    }

    return dock;
}

// 직접 그린 창의 제목 막대 견본이다(9.1절 UW7b). 실제 창 동작 대신 아무것도 하지 않는다.
struct TitleBarSample {
    StringView title;
    float width;
    bool active;
    bool maximized;
    bool canMaximize;
};

Result<SharedPtr<Element>> MakeTitleBar(GalleryContext& context,
                                        const TitleBarSample& sample) noexcept {
    const WindowStyle& style = DefaultWindowStyle();
    auto title = MakeText(context, sample.title, style.title.font);
    auto bar = SharedPtr<WindowTitleBar>::TryMake(context.allocator, context.allocator);
    if (const ErrorCode error = FirstError(title, bar); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    GalleryContext* target = &context;
    auto built = bar.Value()->Construct(
        {.title = title.Value(),
         .canMaximize = sample.canMaximize,
         .makeLabel =
             Delegate<Result<SharedPtr<TextLabel>>(StringView, const FontStyle&)>::CreateLambda(
                 [target](StringView text, const FontStyle& font) noexcept {
                     return MakeText(*target, text, font);
                 })});
    if (!built) {
        return Result<SharedPtr<Element>>::Failure(built.Error());
    }

    bar.Value()->SetActive(sample.active);
    bar.Value()->SetMaximized(sample.maximized);
    return MakeStage(
        context, bar.Value(),
        {sample.width, WindowTitleBar::DesiredHeight(style, title.Value()->GetDesiredSize().y) +
                           2 * StagePadding});
}

} // namespace

Result<LayoutPage> BuildLayoutPage(GalleryContext& context) noexcept {
    auto grid = UniquePtr<GalleryGrid>::TryMake(context.allocator, context);
    if (!grid) {
        return Result<LayoutPage>::Failure(grid.Error());
    }

    GalleryGrid& rows = *grid.Value();
    BuildStatus status;
    const auto one = [&](StringView name, StringView usage,
                         Result<SharedPtr<Element>> sample) noexcept {
        if (status.Take(sample, name)) {
            rows.AddRow(name, usage, sample.Value());
        }
    };
    // 도킹은 에디터의 바탕이라 맨 위에 둔다(self-test도 화면 안에서 끈다).
    auto docking = MakeDocking(context);
    if (status.Take(docking, "Docking")) {
        one("Docking", "DockManager (split, tab drag)",
            MakeStage(context, docking.Value(), DockStage));
    }

    // 직접 그린 창 제목 막대: 활성·비활성(흐림)·최대화(복원 아이콘)·크기 고정(최대화 꺼짐)·좁은
    // 폭(제목 잘림). 창 버튼에 올리면 툴팁, 누르면 올림·눌림 바탕(닫기는 빨강)을 본다.
    {
        constexpr float BarWidth = 420;
        constexpr float NarrowWidth = 200;
        const TitleBarSample samples[] = {
            {"Rocks Editor", BarWidth, true, false, true},
            {"Inactive Window", BarWidth, false, false, true},
            {"Maximized Window", BarWidth, true, true, true},
            {"Fixed Size Dialog", BarWidth, true, false, false},
            {"A Very Long Window Title That Does Not Fit", NarrowWidth, true, false, true}};
        constexpr StringView names[] = {
            "Window Title Bar", "Window Title Bar (inactive)", "Window Title Bar (maximized)",
            "Window Title Bar (fixed size)", "Window Title Bar (narrow)"};
        constexpr StringView usages[] = {"WindowTitleBar + DefaultWindowStyle()",
                                         "WindowTitleBar::SetActive(false)",
                                         "WindowTitleBar::SetMaximized(true)",
                                         "Args::canMaximize = false", "title clipped to its slot"};
        for (usize i = 0; i < sizeof(samples) / sizeof(samples[0]); ++i) {
            one(names[i], usages[i], MakeTitleBar(context, samples[i]));
        }
    }

    one("Horizontal Stack", "HorizontalStack (Auto, Fill)", MakeStack(context, true));
    one("Vertical Stack", "VerticalStack (Auto, Fill)", MakeStack(context, false));
    one("Grid", "Grid (span, fill weights)", MakeGrid(context));
    one("Uniform Grid", "UniformGrid", MakeUniformGrid(context));
    one("Wrap Stack", "WrapStack", MakeWrap(context));
    one("Overlay", "Overlay (z order)", MakeOverlay(context));
    {
        auto fit = MakeFit(context, FitMode::Fit, "Fit");
        auto cover = MakeFit(context, FitMode::Cover, "Cover");
        auto fill = MakeFit(context, FitMode::Fill, "Fill");
        if (status.Take(fit, "Fit Box") && status.Take(cover, "Fit Box") &&
            status.Take(fill, "Fit Box")) {
            const SharedPtr<Element> parts[] = {fit.Value(), cover.Value(), fill.Value()};
            rows.AddRow("Fit Box", "FitBox (Fit, Cover, Fill)", parts);
        }
    }

    {
        auto small = MakeScaled(context, SmallScale, "x0.75");
        auto large = MakeScaled(context, LargeScale, "x1.25");
        if (status.Take(small, "Scale Layer") && status.Take(large, "Scale Layer")) {
            const SharedPtr<Element> parts[] = {small.Value(), large.Value()};
            rows.AddRow("Scale Layer", "ScaleLayer", parts);
        }
    }

    one("Anchor Canvas", "AnchorCanvas (anchors, alignment)", MakeCanvas(context));
    {
        auto vertical = MakeScroll(context, false);
        auto horizontal = MakeScroll(context, true);
        if (status.Take(vertical, "Scroll View") && status.Take(horizontal, "Scroll View")) {
            const SharedPtr<Element> parts[] = {vertical.Value(), horizontal.Value()};
            rows.AddRow("Scroll View", "ScrollView (vertical, horizontal)", parts);
        }
    }

    one("Switcher", "Switcher", MakeSwitchRow<Switcher>(context, false));
    one("Carousel", "Carousel + CarouselPage", MakeSwitchRow<Carousel>(context, true));
    one("Window Title Bar Area", "WindowTitleBarArea", MakeTitleBar(context));

    if (!status.Ok()) {
        return Result<LayoutPage>::Failure(status.Error());
    }

    auto root = rows.Finish();
    if (!root) {
        return Result<LayoutPage>::Failure(root.Error());
    }

    return Result<LayoutPage>::Success({root.Value(), Move(grid.Value()), docking.Value()});
}

} // namespace rocks::ui::gallery
