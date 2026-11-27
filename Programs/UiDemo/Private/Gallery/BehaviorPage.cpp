#include "BehaviorPage.h"

#include <Core/Math/MathUtility.h>
#include <Core/Text/StringBuilder.h>
#include <UiFramework/Layout/Frame.h>
#include <UiFramework/Layout/HorizontalStack.h>
#include <UiFramework/Controls/ProgressBar.h>
#include <UiFramework/Controls/Slider.h>
#include <UiFramework/Layout/VerticalStack.h>

namespace rocks::ui::gallery {

namespace {

// 견본 크기다.
constexpr Vec2 SliderSize{160, 20};
constexpr Vec2 BlockSize{90, 28};
constexpr Vec2 ClipStage{120, 30};
constexpr Vec2 CursorCell{96, 26};
constexpr Vec2 ProgressSize{160, 14};
constexpr float TwoPi = 6.2831853f;
// 배율·기울기 견본 값이다.
constexpr float SampleScale = 1.3f;
constexpr float SampleShear = 0.4f;
constexpr float SampleOpacity = 0.5f;
// 가운데 기준 피벗(로컬 크기 비율)이다.
constexpr Vec2 CenterPivot{0.5f, 0.5f};

template <typename T>
Result<SharedPtr<Element>> Up(Result<SharedPtr<T>>&& made) noexcept {
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    return Result<SharedPtr<Element>>::Success(made.Value());
}

// 슬라이더 값이 바뀔 때 apply를 부른다.
template <typename Apply>
Result<SharedPtr<Element>> MakeSlider(GalleryContext& context, float value,
                                      Apply&& apply) noexcept {
    auto slider = SharedPtr<Slider>::TryMake(context.allocator, context.allocator);
    if (!slider) {
        return Result<SharedPtr<Element>>::Failure(slider.Error());
    }

    if (auto set = slider.Value()->SetValue(value); !set) {
        return Result<SharedPtr<Element>>::Failure(set.Error());
    }

    slider.Value()->SetOnChanged(Delegate<void(float)>::CreateLambda(Move(apply)));
    return Sized(context, slider.Value(), SliderSize);
}

AffineTransform2D Rotation(float turns) noexcept {
    const float angle = turns * TwoPi;
    const float c = math::Cos(angle);
    const float s = math::Sin(angle);
    return {c, -s, s, c, 0, 0};
}

} // namespace

Result<BehaviorPage> BuildBehaviorPage(GalleryContext& context) noexcept {
    Allocator& allocator = context.allocator;
    auto grid = UniquePtr<GalleryGrid>::TryMake(allocator, context);
    auto model = SharedPtr<BindingSample>::TryMake(allocator);
    if (const ErrorCode error = FirstError(grid, model); error != ErrorCode::None) {
        return Result<BehaviorPage>::Failure(error);
    }

    GalleryGrid& rows = *grid.Value();
    BuildStatus status;

    // 초점 외곽선(9.1절 UW4f1): Tab·방향키로 옮기면 외곽선이 보이고, 눌러서 옮긴 초점에는 없다.
    {
        auto first = MakeTextButton(context, "First", DefaultButtonStyle());
        auto second = MakeTextButton(context, "Second", DefaultButtonStyle());
        auto primary = MakeTextButton(context, "Primary", PrimaryButtonStyle());
        auto check = MakeCheckBox(context, CheckState::Checked, "Check");
        auto slider = MakeSlider(context, 0.5f, [](float) noexcept {});
        if (status.Take(first) && status.Take(second) && status.Take(primary) &&
            status.Take(check) && status.Take(slider)) {
            const SharedPtr<Element> parts[] = {first.Value(), second.Value(), primary.Value(),
                                                check.Value(), slider.Value()};
            rows.AddRow("Focus Outline", "Tab, arrows: outline / click: none", parts);
        }
    }

    // 렌더 변환: 회전(슬라이더), 배율, 기울기. 변환은 원하는 크기를 바꾸지 않고, hit도 변환을
    // 따른다.
    {
        auto rotated = MakeTextButton(context, "Rotate me", DefaultButtonStyle());
        if (status.Take(rotated)) {
            auto slider = MakeSlider(
                context, 0.0f, [target = WeakPtr<Button>(rotated.Value())](float value) noexcept {
                    if (auto locked = target.Lock()) {
                        (void)locked->SetRenderTransform(Rotation(value), CenterPivot);
                    }
                });
            if (status.Take(slider)) {
                const SharedPtr<Element> parts[] = {rotated.Value(), slider.Value()};
                rows.AddRow("Render Transform (rotation)", "Element::SetRenderTransform", parts);
            }
        }

        auto scaled = MakeBlock(context, "Scale 1.3", ThemeSlot::AccentBlue);
        auto sheared = MakeBlock(context, "Shear 0.4", ThemeSlot::AccentGreen);
        if (status.Take(scaled) && status.Take(sheared)) {
            auto set = scaled.Value()->SetRenderTransform({SampleScale, 0, 0, SampleScale, 0, 0},
                                                          CenterPivot);
            if (set) {
                set =
                    sheared.Value()->SetRenderTransform({1, SampleShear, 0, 1, 0, 0}, CenterPivot);
            }

            auto a = Sized(context, scaled.Value(), BlockSize);
            auto b = Sized(context, sheared.Value(), BlockSize);
            if (status.Take(set) && status.Take(a) && status.Take(b)) {
                const SharedPtr<Element> parts[] = {a.Value(), b.Value()};
                rows.AddRow("Render Transform (scale, shear)", "AffineTransform2D + pivot", parts,
                            24);
            }
        }
    }

    // 불투명도(슬라이더).
    {
        auto block = MakeBlock(context, "Opacity", ThemeSlot::AccentOrange);
        if (status.Take(block)) {
            auto set = block.Value()->SetRenderOpacity(SampleOpacity);
            auto slider =
                MakeSlider(context, SampleOpacity,
                           [target = WeakPtr<Frame>(block.Value())](float value) noexcept {
                               if (auto locked = target.Lock()) {
                                   (void)locked->SetRenderOpacity(value);
                               }
                           });
            if (status.Take(set) && status.Take(slider)) {
                const SharedPtr<Element> parts[] = {block.Value(), slider.Value()};
                rows.AddRow("Render Opacity", "Element::SetRenderOpacity", parts);
            }
        }
    }

    // 자르기: 같은 넘치는 레이블을 끈 상자·켠 상자에.
    {
        auto loose = MakeText(context, "This label is wider than its box");
        auto clipped = MakeText(context, "This label is wider than its box");
        if (status.Take(loose) && status.Take(clipped)) {
            auto off = Sized(context, loose.Value(), ClipStage);
            auto on = Sized(context, clipped.Value(), ClipStage);
            if (status.Take(off) && status.Take(on)) {
                on.Value()->SetClipToBounds(true);
                const SharedPtr<Element> parts[] = {off.Value(), on.Value()};
                rows.AddRow("Clip To Bounds (off, on)", "Element::SetClipToBounds", parts, 160);
            }
        }
    }

    // 가시성 5종: 가운데 상자의 가시성만 바꾼 세 상자 줄. Collapsed는 자리를 차지하지 않는다.
    {
        struct Mode {
            const char* name;
            Visibility visibility;
        };

        constexpr Mode Modes[] = {{"Visible", Visibility::Visible},
                                  {"Collapsed", Visibility::Collapsed},
                                  {"Hidden", Visibility::Hidden},
                                  {"HitTestInvisible", Visibility::HitTestInvisible},
                                  {"SelfHitTestInvisible", Visibility::SelfHitTestInvisible}};
        for (const Mode& mode : Modes) {
            auto line = SharedPtr<HorizontalStack>::TryMake(allocator, allocator);
            auto left = MakeBlock(context, "A", ThemeSlot::AccentBlue);
            auto middle = MakeTextButton(context, mode.name, DefaultButtonStyle());
            auto right = MakeBlock(context, "C", ThemeSlot::AccentBlue);
            if (!status.Take(line) || !status.Take(left) || !status.Take(middle) ||
                !status.Take(right)) {
                break;
            }

            middle.Value()->SetVisibility(mode.visibility);
            const SharedPtr<Element> parts[] = {left.Value(), middle.Value(), right.Value()};
            Result<void> added = Result<void>::Success();
            for (const SharedPtr<Element>& part : parts) {
                if (added) {
                    added = line.Value()->TryAddSlot(part, {.after = 4});
                }
            }

            if (status.Take(added)) {
                StringBuilder<> name(allocator);
                auto formatted = name.TryAppendFormat("Visibility::{}", mode.name);
                if (status.Take(formatted)) {
                    rows.AddRow(name.View(), "Element::SetVisibility", line.Value());
                }
            }
        }
    }

    // 전경색 상속: Frame이 정한 전경색을 레이블(상속)·흐린 레이블(SubduedForeground)이 따른다.
    {
        auto inherit = MakeText(context, "Inherits");
        auto subdued =
            MakeText(context, "Subdued", NormalFont, ColorRef{ColorSource::SubduedForeground});
        auto column = SharedPtr<VerticalStack>::TryMake(allocator, allocator);
        if (status.Take(inherit) && status.Take(subdued) && status.Take(column)) {
            auto added = column.Value()->TryAddSlot(inherit.Value(), {});
            if (added) {
                added = column.Value()->TryAddSlot(subdued.Value(), {});
            }

            if (added) {
                added =
                    column.Value()->SetForegroundColor(ColorRef::FromTheme(ThemeSlot::AccentGreen));
            }

            if (status.Take(added)) {
                rows.AddRow("Foreground Color", "SetForegroundColor + ColorSource", column.Value());
            }
        }
    }

    // 커서 11종: 칸에 올리면 그 커서다.
    {
        struct Cursor {
            const char* name;
            input::MouseCursor cursor;
        };

        constexpr Cursor Cursors[] = {{"Arrow", input::MouseCursor::Arrow},
                                      {"ResizeH", input::MouseCursor::ResizeHorizontal},
                                      {"ResizeV", input::MouseCursor::ResizeVertical},
                                      {"Text", input::MouseCursor::Text},
                                      {"Hand", input::MouseCursor::Hand},
                                      {"Crosshair", input::MouseCursor::Crosshair},
                                      {"Move", input::MouseCursor::Move},
                                      {"ResizeNWSE", input::MouseCursor::ResizeNorthWestSouthEast},
                                      {"ResizeNESW", input::MouseCursor::ResizeNorthEastSouthWest},
                                      {"Unavailable", input::MouseCursor::Unavailable},
                                      {"Wait", input::MouseCursor::Wait}};
        constexpr usize CursorsPerRow = 6;
        for (usize first = 0; first < sizeof(Cursors) / sizeof(Cursors[0]);
             first += CursorsPerRow) {
            Array<SharedPtr<Element>> cells(allocator);
            for (usize i = first;
                 i < first + CursorsPerRow && i < sizeof(Cursors) / sizeof(Cursors[0]); ++i) {
                // 커서는 실제로 나오는 어두운 입력 칸 바탕에서 본다. Windows 글자·십자 커서는
                // 바탕을 뒤집어 그려 중간 회색 위에서는 거의 사라진다.
                auto cell =
                    MakeBlock(context, Cursors[i].name, ThemeSlot::Input, ThemeSlot::Foreground);
                if (!status.Take(cell)) {
                    break;
                }

                cell.Value()->SetCursorOverride(Cursors[i].cursor);
                auto sized = Sized(context, cell.Value(), CursorCell);
                if (status.Take(sized)) {
                    auto pushed = cells.TryPushBack(sized.Value());
                    status.Take(pushed);
                }
            }

            if (status.Ok()) {
                rows.AddRow(first == 0 ? "Mouse Cursor" : "Mouse Cursor (cont.)",
                            "Element::SetCursorOverride", cells.View(), 4);
            }
        }
    }

    // 속성 바인딩: 슬라이더가 모델을 쓰고, 상자 색·진행 막대가 Bind로 읽는다(값이 바뀔 때만 다시
    // 그림).
    {
        auto bound = SharedPtr<Frame>::TryMake(allocator, allocator);
        auto label = MakeText(context, "Bound color", SmallBoldFont,
                              ColorRef::FromTheme(ThemeSlot::Background));
        auto progress = SharedPtr<ProgressBar>::TryMake(allocator, allocator);
        if (status.Take(bound) && status.Take(label) && status.Take(progress)) {
            auto built = bound.Value()->Construct(
                {.content = label.Value(),
                 .color = Bind(model.Value(), &BindingSample::tint),
                 .brush =
                     Brush::RoundedBox(DefaultThemeColors().Get(ThemeSlot::White), BlockRadius),
                 .padding = BlockPadding});
            progress.Value()->BindValue(Bind(model.Value(), &BindingSample::progress));
            auto slider =
                MakeSlider(context, model.Value()->progress,
                           [target = WeakPtr<BindingSample>(model.Value())](float value) noexcept {
                               if (auto locked = target.Lock()) {
                                   locked->progress = value;
                                   locked->tint = {1 - value, value, 1, 1};
                               }
                           });
            auto bar = Sized(context, progress.Value(), ProgressSize);
            if (status.Take(built) && status.Take(slider) && status.Take(bar)) {
                const SharedPtr<Element> parts[] = {slider.Value(), bound.Value(), bar.Value()};
                rows.AddRow("Attribute Binding", "Bind(model, &Model::field)", parts);
            }
        }
    }

    if (!status.Ok()) {
        return Result<BehaviorPage>::Failure(status.Error());
    }

    auto root = rows.Finish();
    if (!root) {
        return Result<BehaviorPage>::Failure(root.Error());
    }

    return Result<BehaviorPage>::Success({root.Value(), Move(grid.Value()), model.Value()});
}

} // namespace rocks::ui::gallery
