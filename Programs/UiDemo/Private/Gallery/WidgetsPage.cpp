#include "WidgetsPage.h"

#include "../DemoTextShaper.h"

#include <Core/Text/StringBuilder.h>
#include <PlatformWindow/Clipboard.h>
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
#include <UiFramework/Layout/SizeBox.h>
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
// 편집 텍스트 견본 폭과 여러 줄 견본 크기다(UE 갤러리 SBox MinDesiredWidth 200·MinDesiredHeight
// 48 — SStarshipGallery.cpp 1386·1398행). 여러 줄 상자는 스크롤 막대가 보이게 조금 더 높다.
constexpr float EditWidth = 200;
constexpr Vec2 MultiLineSize{200, 48};
constexpr Vec2 MultiLineBoxSize{200, 64};
// 여러 줄 상자 견본의 줄 수다(보이는 높이를 넘어 스크롤 막대가 생긴다).
constexpr int32 MultiLineSampleLines = 12;
// 값 레이블 처음 글자다.
constexpr StringView ValueIdle = "Value: (type in the box)";

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

// content의 폭만 정한다(높이는 내용 — UE SBox WidthOverride).
Result<SharedPtr<Element>> Wide(GalleryContext& context, const SharedPtr<Element>& content,
                                float width) noexcept {
    auto box = SharedPtr<SizeBox>::TryMake(context.allocator, context.allocator);
    if (!box) {
        return Result<SharedPtr<Element>>::Failure(box.Error());
    }

    auto result = box.Value()->SetWidthOverride(width);
    if (result) {
        result = box.Value()->TryAddChild(content);
    }

    return result ? Result<SharedPtr<Element>>::Success(box.Value())
                  : Result<SharedPtr<Element>>::Failure(result.Error());
}

// 편집기의 클립보드는 창 세션의 것이다.
Platform DesktopPlatform() noexcept {
    return Platform{.readClipboardText = &platform::ReadClipboardText,
                    .writeClipboardText = &platform::WriteClipboardText};
}

// 힌트 글자를 붙인다.
Result<void> SetHint(GalleryContext& context, EditableText& editor, StringView hint) noexcept {
    auto text = Text::Invariant(context.allocator, hint);
    return text ? editor.SetHintText(text.Value()) : Result<void>::Failure(text.Error());
}

// 상자를 만들어 text로 시작한다.
Result<SharedPtr<EditableTextBox>> MakeBox(GalleryContext& context,
                                           const SharedPtr<TextShaper>& shaper, StringView text,
                                           bool multiline) noexcept {
    auto box = SharedPtr<EditableTextBox>::TryMake(context.allocator, context.allocator, shaper,
                                                   DesktopPlatform(), multiline);
    if (!box) {
        return box;
    }

    auto ready = box.Value()->Initialize(text, NormalFontSize);
    if (!ready) {
        return Result<SharedPtr<EditableTextBox>>::Failure(ready.Error());
    }

    return box;
}

// 값 레이블에 "prefix: value"를 보인다.
void ShowValue(GalleryContext& context, const WeakPtr<TextLabel>& target, StringView prefix,
               StringView value) noexcept {
    auto label = target.Lock();
    StringBuilder<> line(context.allocator);
    if (label && line.TryAppend(prefix) && line.TryAppend(": ") && line.TryAppend(value)) {
        (void)context.text.SetText(*label, line.View(), NormalFont);
    }
}

// 확정 사유 이름이다(값 레이블).
StringView CommitPrefix(TextCommitReason reason) noexcept {
    return reason == TextCommitReason::Enter ? StringView("Committed (Enter)")
                                             : StringView("Committed (focus lost)");
}

// 레이블이 붙은 체크 상자를 style로 만든다.
Result<SharedPtr<CheckBox>> MakeStyledCheckBox(GalleryContext& context, const CheckBoxStyle& style,
                                               CheckState state, StringView label) noexcept {
    auto check = SharedPtr<CheckBox>::TryMake(context.allocator, context.allocator);
    if (!check) {
        return check;
    }

    auto result = check.Value()->SetCheckBoxStyle(style);
    if (result) {
        result = check.Value()->SetState(state);
    }

    if (result && !label.IsEmpty()) {
        auto text = MakeText(context, label);
        result =
            text ? check.Value()->TryAddChild(text.Value()) : Result<void>::Failure(text.Error());
    }

    if (!result) {
        return Result<SharedPtr<CheckBox>>::Failure(result.Error());
    }

    return check;
}

// 체크 상자 형식 행이다(23.3절 — UE "ToggleButtonCheckbox"·"ToggleButtonCheckboxAlt"·
// "TransparentCheckBox"·"RadioButton"). 라디오 셋은 묶음 하나다(하나를 체크하면 나머지를 푼다 —
// UE처럼 호출자가 묶는다).
void AddCheckTypeRows(GalleryContext& context, GalleryGrid& rows, BuildStatus& status,
                      CheckSamples& samples) noexcept {
    auto toggle =
        MakeStyledCheckBox(context, DefaultToggleButtonCheckBoxStyle(), CheckState::Checked, "On");
    auto toggleOff = MakeStyledCheckBox(context, DefaultToggleButtonCheckBoxStyle(),
                                        CheckState::Unchecked, "Off");
    auto alt = MakeStyledCheckBox(context, DefaultToggleButtonCheckBoxAltStyle(),
                                  CheckState::Checked, "On");
    auto altOff = MakeStyledCheckBox(context, DefaultToggleButtonCheckBoxAltStyle(),
                                     CheckState::Unchecked, "Off");
    auto transparent = MakeStyledCheckBox(context, DefaultTransparentCheckBoxStyle(),
                                          CheckState::Checked, "Transparent");
    if (!status.Take(toggle, "Toggle Button") || !status.Take(toggleOff) || !status.Take(alt) ||
        !status.Take(altOff) || !status.Take(transparent)) {
        return;
    }

    const SharedPtr<Element> toggles[] = {toggle.Value(), toggleOff.Value()};
    rows.AddRow("Toggle Button", "CheckBox + DefaultToggleButtonCheckBoxStyle()", toggles);
    const SharedPtr<Element> alts[] = {alt.Value(), altOff.Value()};
    rows.AddRow("Toggle Button (alt)", "CheckBox + DefaultToggleButtonCheckBoxAltStyle()", alts);
    rows.AddRow("Transparent Check Box", "CheckBox + DefaultTransparentCheckBoxStyle()",
                transparent.Value());
    samples.toggle = toggle.Value();
    constexpr const char* RadioLabels[RadioSampleCount] = {"One", "Two", "Three"};
    SharedPtr<Element> radios[RadioSampleCount];
    for (usize i = 0; i < RadioSampleCount; ++i) {
        auto radio = MakeStyledCheckBox(context, DefaultRadioButtonStyle(),
                                        i == 0 ? CheckState::Checked : CheckState::Unchecked,
                                        RadioLabels[i]);
        if (!status.Take(radio, "Radio Button")) {
            return;
        }

        samples.radios[i] = radio.Value();
        radios[i] = radio.Value();
    }

    // 묶음: 하나를 체크하면 나머지를 푼다. 체크된 것을 다시 누르면 그대로 둔다(라디오 관례). 묶음은
    // 공유 객체 하나로 잡고(delegate 안 저장 크기), 견본은 약한 참조다.
    struct RadioGroup {
        WeakPtr<CheckBox> members[RadioSampleCount];
    };

    auto group = SharedPtr<RadioGroup>::TryMake(context.allocator);
    if (!status.Take(group, "Radio Button group")) {
        return;
    }

    for (usize i = 0; i < RadioSampleCount; ++i) {
        group.Value()->members[i] = WeakPtr<CheckBox>(samples.radios[i]);
    }

    for (usize i = 0; i < RadioSampleCount; ++i) {
        samples.radios[i]->SetOnChanged(Delegate<void(CheckState)>::CreateLambda(
            [shared = group.Value(), i](CheckState) noexcept {
                for (usize j = 0; j < RadioSampleCount; ++j) {
                    if (auto member = shared->members[j].Lock()) {
                        (void)member->SetState(j == i ? CheckState::Checked
                                                      : CheckState::Unchecked);
                    }
                }
            }));
    }

    rows.AddRow("Radio Button", "CheckBox + DefaultRadioButtonStyle()", radios);
}

// "Text Input" 묶음(UE 갤러리 1365–1405행 — SEditableText·SEditableTextBox·여러 줄)과 Rocks가 더한
// 읽기 전용·비밀번호·오류·값 레이블 행이다(22.9절).
void AddTextInputRows(GalleryContext& context, GalleryGrid& rows, BuildStatus& status,
                      TextInputSamples& samples) noexcept {
    Allocator& allocator = context.allocator;
    auto made = SharedPtr<DemoTextShaper>::TryMake(allocator, allocator, context.text);
    if (!status.Take(made, "Text Input shaper")) {
        return;
    }

    const SharedPtr<TextShaper> shaper = made.Value();
    const auto one = [&](StringView name, StringView usage,
                         Result<SharedPtr<Element>> widget) noexcept {
        if (status.Take(widget, name)) {
            rows.AddRow(name, usage, widget.Value());
        }
    };

    // 바탕 없는 편집기(UE SEditableText).
    {
        auto editor = SharedPtr<EditableText>::TryMake(allocator, allocator, shaper,
                                                       DesktopPlatform(), false);
        if (status.Take(editor)) {
            auto ready = editor.Value()->Initialize({}, NormalFontSize);
            if (ready) {
                ready = SetHint(context, *editor.Value(), "This is editable text");
            }

            if (status.Take(ready, "Editable Text")) {
                one("Editable Text", "EditableText + SetHintText",
                    Wide(context, editor.Value(), EditWidth));
            }
        }
    }

    // 힌트 상자와 값 레이블(바뀐 값·확정 사유).
    {
        auto box = MakeBox(context, shaper, {}, false);
        auto value = MakeText(context, ValueIdle, NormalFont, ColorRef::Specified(LabelColor));
        if (status.Take(box) && status.Take(value)) {
            EditableText& editor = box.Value()->Editor();
            auto ready = SetHint(context, editor, "This is an editable text box");
            const WeakPtr<TextLabel> target(value.Value());
            GalleryContext* owner = &context;
            editor.SetOnTextChanged(
                Delegate<void(StringView)>::CreateLambda([owner, target](StringView text) noexcept {
                    ShowValue(*owner, target, "Changed", text);
                }));
            editor.SetOnTextCommitted(Delegate<void(StringView, TextCommitReason)>::CreateLambda(
                [owner, target](StringView text, TextCommitReason reason) noexcept {
                    ShowValue(*owner, target, CommitPrefix(reason), text);
                }));
            auto wide = Wide(context, box.Value(), EditWidth);
            if (status.Take(ready, "Editable Text Box") && status.Take(wide)) {
                const SharedPtr<Element> parts[] = {wide.Value(), value.Value()};
                rows.AddRow("Editable Text Box", "EditableTextBox + SetOnTextChanged / Committed",
                            parts);
                samples.box = box.Value();
                samples.value = value.Value();
            }
        }
    }

    // 읽기 전용(UE ReadOnly 바탕 Header)·비밀번호.
    {
        auto readOnly = MakeBox(context, shaper, "Read only text", false);
        auto password = MakeBox(context, shaper, "secret", false);
        if (status.Take(readOnly) && status.Take(password)) {
            readOnly.Value()->Editor().SetReadOnly(true);
            auto masked = password.Value()->Editor().SetPassword(true);
            if (status.Take(masked, "Password")) {
                one("Editable Text Box (read only)", "EditableText::SetReadOnly",
                    Wide(context, readOnly.Value(), EditWidth));
                one("Editable Text Box (password)", "EditableText::SetPassword",
                    Wide(context, password.Value(), EditWidth));
            }
        }
    }

    // 오류 표지와 팝업(UE SPopupErrorText — 22.6절).
    {
        auto box = MakeBox(context, shaper, "Untitled", false);
        auto message = Text::Invariant(allocator, "Name is already in use");
        if (status.Take(box) && status.Take(message)) {
            auto set = box.Value()->SetError(message.Value());
            if (status.Take(set, "Editable Text Box (error)")) {
                one("Editable Text Box (error)", "EditableTextBox::SetError",
                    Wide(context, box.Value(), EditWidth));
                samples.error = box.Value();
            }
        }
    }

    // 여러 줄(UE SMultiLineEditableText·Box): 힌트 편집기와 넘치는 긴 글 상자(스크롤 막대).
    {
        auto editor =
            SharedPtr<EditableText>::TryMake(allocator, allocator, shaper, DesktopPlatform(), true);
        StringBuilder<> lines(allocator);
        bool written = true;
        for (int32 i = 1; i <= MultiLineSampleLines && written; ++i) {
            written = lines.TryAppend(i == 1 ? StringView("Line 1") : StringView("\nLine ")) &&
                      (i == 1 || lines.TryAppendFormat("{}", i));
        }

        auto box = MakeBox(context, shaper, written ? lines.View() : StringView{}, true);
        if (status.Take(editor) && status.Take(box)) {
            auto ready = editor.Value()->Initialize({}, NormalFontSize);
            if (ready) {
                ready = SetHint(context, *editor.Value(), "This is multi-line\neditable text");
            }

            if (status.Take(ready, "Multi-Line Editable Text")) {
                one("Multi-Line Editable Text", "EditableText (multiline)",
                    Sized(context, editor.Value(), MultiLineSize));
                one("Multi-Line Editable Text Box", "EditableTextBox (multiline) + ScrollBar",
                    Sized(context, box.Value(), MultiLineBoxSize));
                samples.multiLine = box.Value();
            }
        }
    }
}

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

    // 체크 상자 형식(토글·라디오 — 23.3절).
    CheckSamples checks;
    AddCheckTypeRows(context, rows, status, checks);

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
                rows.AddRow("Combo Box", "ComboBox + DefaultComboBoxStyle()", parts);
            }
        }
    }

    // 테두리 없는 콤보(UE "SimpleComboBox" — 23.2절).
    {
        auto combo = SharedPtr<ComboBox>::TryMake(allocator, allocator);
        if (status.Take(combo)) {
            auto built = combo.Value()->Construct(
                {.count = ComboItemCount,
                 .selected = 0,
                 .makeOption = ComboBox::MakeOption::CreateLambda(
                     [target = &context](usize index, bool) noexcept -> Result<SharedPtr<Element>> {
                         return AsElement(MakeText(*target, ComboItems[index]));
                     })});
            if (built) {
                built = combo.Value()->SetComboStyle(DefaultSimpleComboBoxStyle());
            }

            if (status.Take(built)) {
                one("Simple Combo Box", "ComboBox + DefaultSimpleComboBoxStyle()",
                    Result<SharedPtr<Element>>::Success(combo.Value()));
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

    // 숫자 입력(UE 1313–1328행 — SSpinBox, 범위 0–100이라 채움 막대가 보인다).
    {
        auto shaper = SharedPtr<DemoTextShaper>::TryMake(allocator, allocator, context.text);
        auto spin = shaper ? SharedPtr<SpinBox>::TryMake(allocator, allocator, shaper.Value(),
                                                         DesktopPlatform())
                           : Result<SharedPtr<SpinBox>>::Failure(shaper.Error());
        if (status.Take(spin, "Spin Box")) {
            SpinBox& box = *spin.Value();
            auto built = box.Initialize(NormalFontSize);
            if (built) {
                built = box.SetValueRange(0.0, SpinMaximum);
            }

            if (built) {
                built = box.SetValue(SpinValue);
            }

            if (status.Take(built)) {
                one("Spin Box", "SpinBox", Sized(context, spin.Value(), SpinSize));
            }
        }
    }

    // 편집 텍스트(UE 순서 — 숫자 입력 다음, 1365행).
    TextInputSamples textInput;
    AddTextInputRows(context, rows, status, textInput);

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

    return Result<WidgetsPage>::Success(
        {root.Value(), Move(grid.Value()), Move(textInput), Move(checks)});
}

} // namespace rocks::ui::gallery
