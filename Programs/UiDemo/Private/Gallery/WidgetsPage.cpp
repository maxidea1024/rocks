#include "WidgetsPage.h"

#include <UiCore/KeyChord.h>
#include <UiFramework/Controls/ActivityIndicator.h>
#include <UiFramework/Controls/CheckBox.h>
#include <UiFramework/Controls/ComboBox.h>
#include <UiFramework/Controls/Image.h>
#include <UiFramework/Controls/InputKeySelector.h>
#include <UiFramework/Controls/ProgressBar.h>
#include <UiFramework/Controls/RadialSlider.h>
#include <UiFramework/Scrolling/ScrollBar.h>
#include <UiFramework/Controls/Slider.h>
#include <UiFramework/Controls/SpinBox.h>
#include <UiFramework/Layout/HorizontalStack.h>
#include <UiFramework/Controls/Timeline.h>
#include <UiFramework/Popups/ToolTip.h>
#include <UiFramework/Layout/VerticalStack.h>

namespace rocks::ui::gallery {

namespace {

// UE 갤러리 콤보 항목이다(ConstructWidgetGallery 724–737행).
constexpr const char* ComboItems[] = {
    "Option A One",    "Option A Two",    "Option A Three",    "Option A Four",    "Option A Five",
    "Option A Six",    "Option A Seven",  "Option A Eight",    "Option A Nine",    "Option A Ten",
    "Option A Eleven", "Option A Twelve", "Option A Thirteen", "Option A Fourteen"};
constexpr usize ComboItemCount = sizeof(ComboItems) / sizeof(ComboItems[0]);

// 너비가 내용에서 나오지 않는 위젯의 갤러리 크기다(UE 갤러리 행 높이에 맞춘 값).
constexpr Vec2 BarSize{200, 20};
constexpr Vec2 SpinSize{120, 22};
constexpr Vec2 RadialSize{64, 64};
constexpr Vec2 TimelineSize{240, 36};
constexpr Vec2 HorizontalScrollSize{200, 12};
constexpr Vec2 VerticalScrollSize{12, 64};
constexpr Vec2 ImageSize{48, 48};
constexpr Vec2 SpacerSize{40, 1};
// UE 체크 상자 행의 상자 사이 여백이다(1037–1041행 Padding 8).
constexpr float CheckBoxSpacing = 8;
// 숫자 입력 견본 값이다.
constexpr float SpinValue = 42;
constexpr float SpinMaximum = 100;
constexpr float ProgressValue = 0.5f;
constexpr float SliderValue = 0.5f;
constexpr float ScrollThumb = 0.3f;
constexpr float ImageRadius = 6;

// 위젯이 정한 표시 문자열을 레이블에 shaping하는 콜백이다(SpinBox·InputKeySelector caption).
// 레이블이 사라져도 매달리지 않게 약한 참조로 잡는다.
Delegate<Result<void>(StringView)> Caption(GalleryContext& context,
                                           const SharedPtr<TextLabel>& label) noexcept {
    return Delegate<Result<void>(StringView)>::CreateLambda(
        [text = &context.text, target = WeakPtr<TextLabel>(label)](StringView value) noexcept {
            auto locked = target.Lock();
            return locked ? text->SetText(*locked, value, NormalFont) : Result<void>::Success();
        });
}

template <typename T>
Result<SharedPtr<Element>> AsElement(Result<SharedPtr<T>>&& made) noexcept;

// 글자 툴팁이다(밝은 바탕 위 검은 글자 — DefaultToolTipStyle).
Result<SharedPtr<Element>> MakeTextToolTip(GalleryContext& context, StringView text) noexcept {
    const ToolTipStyle& style = DefaultToolTipStyle();
    auto label = MakeText(context, text, style.text.font);
    auto tip = SharedPtr<ToolTip>::TryMake(context.allocator, context.allocator);
    if (const ErrorCode error = FirstError(label, tip); error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    auto built = tip.Value()->Construct({.text = label.Value()});
    return built ? Result<SharedPtr<Element>>::Success(tip.Value())
                 : Result<SharedPtr<Element>>::Failure(built.Error());
}

// 제목·단축키·설명 툴팁이다(어두운 바탕 — ToolTip 표준 API, ContentToolTipStyle).
Result<SharedPtr<Element>> MakeContentToolTip(GalleryContext& context) noexcept {
    const ToolTipStyle& style = ContentToolTipStyle();
    auto title = MakeText(context, "Save Current Level", style.title.font);
    auto keys = MakeText(context, "Ctrl+S", style.shortcut.font);
    auto description =
        MakeText(context, "Saves the level that is open in the viewport.", style.description.font);
    auto tip = SharedPtr<ToolTip>::TryMake(context.allocator, context.allocator);
    if (const ErrorCode error = FirstError(title, keys, description, tip);
        error != ErrorCode::None) {
        return Result<SharedPtr<Element>>::Failure(error);
    }

    auto built = tip.Value()->Construct(
        {.title = title.Value(), .shortcut = keys.Value(), .description = description.Value()});
    return built ? Result<SharedPtr<Element>>::Success(tip.Value())
                 : Result<SharedPtr<Element>>::Failure(built.Error());
}

template <typename T>
Result<SharedPtr<Element>> AsElement(Result<SharedPtr<T>>&& made) noexcept {
    if (!made) {
        return Result<SharedPtr<Element>>::Failure(made.Error());
    }

    return Result<SharedPtr<Element>>::Success(made.Value());
}

} // namespace

Result<WidgetsPage> BuildWidgetsPage(GalleryContext& context) noexcept {
    Allocator& allocator = context.allocator;
    auto grid = UniquePtr<GalleryGrid>::TryMake(allocator, context);
    if (!grid) {
        return Result<WidgetsPage>::Failure(grid.Error());
    }

    GalleryGrid& rows = *grid.Value();
    BuildStatus status;
    const auto one = [&](StringView name, StringView usage,
                         Result<SharedPtr<Element>> widget) noexcept {
        if (status.Take(widget, name)) {
            rows.AddRow(name, usage, widget.Value());
        }
    };

    // 버튼(UE 824–948행).
    one("Primary Button", "Button + PrimaryButtonStyle()",
        AsElement(MakeTextButton(context, "Label", PrimaryButtonStyle())));
    one("Default/Secondary Button", "Button + DefaultButtonStyle()",
        AsElement(MakeTextButton(context, "Label", DefaultButtonStyle())));
    one("Simple Button", "Button + SimpleButtonStyle()",
        AsElement(MakeTextButton(context, "Label", SimpleButtonStyle())));

    // 체크 상자(UE 1030–1062행): 레이블 없는 상자 넷(해제·해제·체크·미정), 그리고 레이블 붙은 변형.
    {
        auto a = MakeCheckBox(context, CheckState::Unchecked, {});
        auto b = MakeCheckBox(context, CheckState::Unchecked, {});
        auto c = MakeCheckBox(context, CheckState::Checked, {});
        auto d = MakeCheckBox(context, CheckState::Indeterminate, {});
        if (status.Take(a) && status.Take(b) && status.Take(c) && status.Take(d)) {
            const SharedPtr<Element> checks[] = {a.Value(), b.Value(), c.Value(), d.Value()};
            rows.AddRow("Check Box", "CheckBox + DefaultCheckBoxStyle()", checks, CheckBoxSpacing);
        }

        auto labeled = MakeCheckBox(context, CheckState::Checked, "Label");
        one("Check Box (label)", "CheckBox + TextLabel", Move(labeled));
    }

    // 콤보 상자(UE 1095–1120행): 고른 항목을 옆 레이블에 보인다.
    {
        auto combo = SharedPtr<ComboBox>::TryMake(allocator, allocator);
        auto chosen = MakeText(context, ComboItems[0]);
        if (status.Take(combo) && status.Take(chosen)) {
            auto built = combo.Value()->Construct(
                {.count = ComboItemCount,
                 .selected = 0,
                 .makeOption = ComboBox::MakeOption::CreateLambda(
                     [target = &context](usize index, bool) noexcept -> Result<SharedPtr<Element>> {
                         return AsElement(MakeText(*target, ComboItems[index]));
                     }),
                 .onSelectionChanged = Delegate<void(int32, SelectionCause)>::CreateLambda(
                     [target = &context, label = WeakPtr<TextLabel>(chosen.Value())](
                         int32 index, SelectionCause) noexcept {
                         auto locked = label.Lock();
                         if (locked && index >= 0 && static_cast<usize>(index) < ComboItemCount) {
                             (void)target->text.SetText(*locked, ComboItems[index], NormalFont);
                         }
                     })});
            if (status.Take(built)) {
                const SharedPtr<Element> parts[] = {combo.Value(), chosen.Value()};
                rows.AddRow("Combo Box", "ComboBox (MenuAnchor)", parts);
            }
        }
    }

    // throbber(UE 1164행)·진행 막대(1194–1215행, marquee 1432행).
    {
        auto throbber = SharedPtr<ActivityIndicator>::TryMake(allocator, allocator, true);
        auto progress = SharedPtr<ProgressBar>::TryMake(allocator, allocator);
        auto marquee = SharedPtr<ProgressBar>::TryMake(allocator, allocator);
        if (status.Take(throbber) && status.Take(progress) && status.Take(marquee)) {
            throbber.Value()->SetAnimating(true);
            auto set = progress.Value()->SetValue(ProgressValue);
            if (status.Take(set)) {
                marquee.Value()->SetMarquee(true);
                one("Activity Indicator", "ActivityIndicator",
                    Result<SharedPtr<Element>>::Success(throbber.Value()));
                one("Progress Bar", "ProgressBar", Sized(context, progress.Value(), BarSize));
                one("Progress Bar (marquee)", "ProgressBar::SetMarquee",
                    Sized(context, marquee.Value(), BarSize));
            }
        }
    }

    // 슬라이더(UE 1258–1288행): 기본·손잡이 들여쓰기 없음.
    {
        auto slider = SharedPtr<Slider>::TryMake(allocator, allocator);
        auto flat = SharedPtr<Slider>::TryMake(allocator, allocator);
        if (status.Take(slider) && status.Take(flat)) {
            auto set = slider.Value()->SetValue(SliderValue);
            if (set) {
                set = flat.Value()->SetValue(SliderValue);
            }

            if (status.Take(set)) {
                flat.Value()->SetIndentHandle(false);
                one("Slider", "Slider", Sized(context, slider.Value(), BarSize));
                one("Slider (no indentation)", "Slider::SetIndentHandle(false)",
                    Sized(context, flat.Value(), BarSize));
            }
        }
    }

    // 간격(UE 1290행): 두 버튼 사이 고정 간격.
    {
        auto left = MakeTextButton(context, "Left", DefaultButtonStyle());
        auto right = MakeTextButton(context, "Right", DefaultButtonStyle());
        auto spacer = Sized(context, {}, SpacerSize);
        if (status.Take(left) && status.Take(right) && status.Take(spacer)) {
            const SharedPtr<Element> parts[] = {left.Value(), spacer.Value(), right.Value()};
            rows.AddRow("Spacer", "SizeBox (fixed size)", parts, 0);
        }
    }

    // 숫자 입력(UE 1313–1328행).
    {
        auto spin = SharedPtr<SpinBox>::TryMake(allocator, allocator);
        auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
        if (status.Take(spin) && status.Take(label)) {
            auto built = spin.Value()->Construct({.value = SpinValue,
                                                  .minimum = 0,
                                                  .maximum = SpinMaximum,
                                                  .step = 1,
                                                  .content = label.Value(),
                                                  .caption = Caption(context, label.Value())});
            if (status.Take(built)) {
                one("Spin Box", "SpinBox", Sized(context, spin.Value(), SpinSize));
            }
        }
    }

    // UE 갤러리에 없는 Rocks 부품(16.4절 뒤쪽 행).
    {
        auto keys = SharedPtr<InputKeySelector>::TryMake(allocator, allocator);
        auto label = SharedPtr<TextLabel>::TryMake(allocator, allocator);
        if (status.Take(keys) && status.Take(label)) {
            auto built = keys.Value()->Construct({.selectedKey = KeyChord{input::Key::F},
                                                  .content = label.Value(),
                                                  .caption = Caption(context, label.Value())});
            if (status.Take(built)) {
                one("Input Key Selector", "InputKeySelector",
                    Result<SharedPtr<Element>>::Success(keys.Value()));
            }
        }
    }

    {
        auto radial = SharedPtr<RadialSlider>::TryMake(allocator, allocator);
        if (status.Take(radial)) {
            auto built = radial.Value()->Construct({.value = SliderValue});
            if (status.Take(built)) {
                one("Radial Slider", "RadialSlider", Sized(context, radial.Value(), RadialSize));
            }
        }
    }

    {
        auto timeline = SharedPtr<Timeline>::TryMake(allocator, allocator);
        if (status.Take(timeline)) {
            auto set = timeline.Value()->SetValue(SliderValue);
            if (status.Take(set)) {
                one("Timeline", "Timeline", Sized(context, timeline.Value(), TimelineSize));
            }
        }
    }

    {
        auto horizontal = SharedPtr<ScrollBar>::TryMake(allocator, allocator);
        auto vertical = SharedPtr<ScrollBar>::TryMake(allocator, allocator);
        if (status.Take(horizontal) && status.Take(vertical)) {
            auto built =
                horizontal.Value()->Construct({.thumbSize = ScrollThumb, .horizontal = true});
            if (built) {
                built = vertical.Value()->Construct({.thumbSize = ScrollThumb});
            }

            auto wide = Sized(context, horizontal.Value(), HorizontalScrollSize);
            auto tall = Sized(context, vertical.Value(), VerticalScrollSize);
            if (status.Take(built) && status.Take(wide) && status.Take(tall)) {
                const SharedPtr<Element> bars[] = {wide.Value(), tall.Value()};
                rows.AddRow("Scroll Bar", "ScrollBar", bars);
            }
        }
    }

    {
        auto image = SharedPtr<Image>::TryMake(allocator, allocator);
        if (status.Take(image)) {
            auto set = image.Value()->SetBrush(
                Brush::RoundedBox(DefaultThemeColors().Get(ThemeSlot::AccentBlue), ImageRadius));
            if (status.Take(set)) {
                one("Image", "Image + Brush", Sized(context, image.Value(), ImageSize));
            }
        }
    }

    // 툴팁: 글자 툴팁(밝은 바탕)과 내용 툴팁(어두운 바탕, 제목·단축키·설명).
    {
        auto plain = MakeTextButton(context, "Hover me", DefaultButtonStyle());
        auto rich = MakeTextButton(context, "Rich tip", DefaultButtonStyle());
        if (status.Take(plain) && status.Take(rich)) {
            auto set = plain.Value()->SetToolTipFactory(
                Element::ToolTipFactory::CreateLambda(
                    [target = &context]() noexcept -> Result<SharedPtr<Element>> {
                        return MakeTextToolTip(*target, "Plain text tool tip");
                    }),
                "Plain text tool tip");
            if (status.Take(set)) {
                set = rich.Value()->SetToolTipFactory(
                    Element::ToolTipFactory::CreateLambda(
                        [target = &context]() noexcept -> Result<SharedPtr<Element>> {
                            return MakeContentToolTip(*target);
                        }),
                    "Save Current Level");
            }

            if (status.Take(set)) {
                const SharedPtr<Element> parts[] = {plain.Value(), rich.Value()};
                rows.AddRow("Tool Tip", "ToolTip + DefaultToolTipStyle() / ContentToolTipStyle()",
                            parts);
            }
        }
    }

    if (!status.Ok()) {
        return Result<WidgetsPage>::Failure(status.Error());
    }

    auto root = rows.Finish();
    if (!root) {
        return Result<WidgetsPage>::Failure(root.Error());
    }

    return Result<WidgetsPage>::Success({root.Value(), Move(grid.Value())});
}

} // namespace rocks::ui::gallery
