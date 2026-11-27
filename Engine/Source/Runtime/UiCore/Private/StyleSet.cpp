#include <UiCore/StyleSet.h>

#include <UiCore/StyleRegistry.h>
#include <UiCore/Styles/CoreStyles.h>
#include <UiCore/Styles/GameWidgetStyles.h>

namespace rocks::ui {

StyleSet::StyleSet(Allocator& allocator) noexcept
    : allocator_(allocator), parent_(allocator), buttons_(allocator), checkBoxes_(allocator),
      dockTabs_(allocator), menuBars_(allocator), menus_(allocator), progressBars_(allocator),
      scrollBars_(allocator), sliders_(allocator), texts_(allocator), toolTips_(allocator),
      windows_(allocator), brushes_(allocator), colors_(allocator), floats_(allocator),
      margins_(allocator) {
}

template <typename T>
Result<void> StyleSet::Store(Array<Entry<T>>& entries, StringView name, const T& value) noexcept {
    if (name.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ++revision_;
    for (auto& entry : entries) {
        if (entry.name.View() == name) {
            entry.value = value;
            return Result<void>::Success();
        }
    }

    String key(allocator_);
    if (auto assigned = key.TryAssign(name); !assigned) {
        return assigned;
    }

    return entries.TryPushBack(Entry<T>{Move(key), value});
}

template <typename T>
const T* StyleSet::Lookup(const Array<Entry<T>>& entries, StringView name) noexcept {
    for (const auto& entry : entries) {
        if (entry.name.View() == name) {
            return &entry.value;
        }
    }

    return nullptr;
}

Result<void> StyleSet::SetParentName(StringView name) noexcept {
    ++revision_;
    return parent_.TryAssign(name);
}

StringView StyleSet::ParentName() const noexcept {
    return parent_.View();
}

uint64 StyleSet::Revision() const noexcept {
    return revision_;
}

Result<void> StyleSet::SetValue(StringView name, const ButtonStyle& value) noexcept {
    return Store(buttons_, name, value);
}

const ButtonStyle* StyleSet::FindValue(StringView name, const ButtonStyle*) const noexcept {
    return Lookup(buttons_, name);
}

Result<void> StyleSet::SetValue(StringView name, const CheckBoxStyle& value) noexcept {
    return Store(checkBoxes_, name, value);
}

const CheckBoxStyle* StyleSet::FindValue(StringView name, const CheckBoxStyle*) const noexcept {
    return Lookup(checkBoxes_, name);
}

Result<void> StyleSet::SetValue(StringView name, const DockTabStyle& value) noexcept {
    return Store(dockTabs_, name, value);
}

const DockTabStyle* StyleSet::FindValue(StringView name, const DockTabStyle*) const noexcept {
    return Lookup(dockTabs_, name);
}

Result<void> StyleSet::SetValue(StringView name, const MenuBarStyle& value) noexcept {
    return Store(menuBars_, name, value);
}

const MenuBarStyle* StyleSet::FindValue(StringView name, const MenuBarStyle*) const noexcept {
    return Lookup(menuBars_, name);
}

Result<void> StyleSet::SetValue(StringView name, const MenuStyle& value) noexcept {
    return Store(menus_, name, value);
}

const MenuStyle* StyleSet::FindValue(StringView name, const MenuStyle*) const noexcept {
    return Lookup(menus_, name);
}

Result<void> StyleSet::SetValue(StringView name, const ProgressBarStyle& value) noexcept {
    return Store(progressBars_, name, value);
}

const ProgressBarStyle* StyleSet::FindValue(StringView name,
                                            const ProgressBarStyle*) const noexcept {
    return Lookup(progressBars_, name);
}

Result<void> StyleSet::SetValue(StringView name, const ScrollBarStyle& value) noexcept {
    return Store(scrollBars_, name, value);
}

const ScrollBarStyle* StyleSet::FindValue(StringView name, const ScrollBarStyle*) const noexcept {
    return Lookup(scrollBars_, name);
}

Result<void> StyleSet::SetValue(StringView name, const SliderStyle& value) noexcept {
    return Store(sliders_, name, value);
}

const SliderStyle* StyleSet::FindValue(StringView name, const SliderStyle*) const noexcept {
    return Lookup(sliders_, name);
}

Result<void> StyleSet::SetValue(StringView name, const TextStyle& value) noexcept {
    return Store(texts_, name, value);
}

const TextStyle* StyleSet::FindValue(StringView name, const TextStyle*) const noexcept {
    return Lookup(texts_, name);
}

Result<void> StyleSet::SetValue(StringView name, const ToolTipStyle& value) noexcept {
    return Store(toolTips_, name, value);
}

const ToolTipStyle* StyleSet::FindValue(StringView name, const ToolTipStyle*) const noexcept {
    return Lookup(toolTips_, name);
}

Result<void> StyleSet::SetValue(StringView name, const WindowStyle& value) noexcept {
    return Store(windows_, name, value);
}

const WindowStyle* StyleSet::FindValue(StringView name, const WindowStyle*) const noexcept {
    return Lookup(windows_, name);
}

Result<void> StyleSet::SetValue(StringView name, const StyleBrush& value) noexcept {
    return Store(brushes_, name, value);
}

const StyleBrush* StyleSet::FindValue(StringView name, const StyleBrush*) const noexcept {
    return Lookup(brushes_, name);
}

Result<void> StyleSet::SetValue(StringView name, const ColorRef& value) noexcept {
    return Store(colors_, name, value);
}

const ColorRef* StyleSet::FindValue(StringView name, const ColorRef*) const noexcept {
    return Lookup(colors_, name);
}

Result<void> StyleSet::SetValue(StringView name, const float& value) noexcept {
    return Store(floats_, name, value);
}

const float* StyleSet::FindValue(StringView name, const float*) const noexcept {
    return Lookup(floats_, name);
}

Result<void> StyleSet::SetValue(StringView name, const Vec4& value) noexcept {
    return Store(margins_, name, value);
}

const Vec4* StyleSet::FindValue(StringView name, const Vec4*) const noexcept {
    return Lookup(margins_, name);
}

Result<void> AddCoreStyles(StyleSet& styles) noexcept {
    // 글꼴은 StarshipCoreStyle.cpp 34–40행(Small Regular 8, NormalBold Bold 10, Large Regular 14),
    // 색은 모두 NormalText처럼 상속 전경색이다(1252–1258행).
    const TextStyle small{{Typeface::Regular, SmallFontSize}, {ColorSource::Foreground}};
    const TextStyle primaryButton{{Typeface::Bold, NormalFontSize}, {ColorSource::Foreground}};
    const TextStyle large{{Typeface::Regular, LargeFontSize}, {ColorSource::Foreground}};
    Result<void> added = styles.Set<ButtonStyle>("Button", DefaultButtonStyle());
    if (added) {
        added = styles.Set<ButtonStyle>("PrimaryButton", PrimaryButtonStyle());
    }

    if (added) {
        added = styles.Set<ButtonStyle>("SimpleButton", SimpleButtonStyle());
    }

    if (added) {
        added = styles.Set<ButtonStyle>("NoBorder", NoBorderButtonStyle());
    }

    if (added) {
        added = styles.Set<TextStyle>("NormalText", NormalTextStyle());
    }

    if (added) {
        added = styles.Set<TextStyle>("SmallText", small);
    }

    if (added) {
        added = styles.Set<TextStyle>("PrimaryButtonText", primaryButton);
    }

    if (added) {
        added = styles.Set<TextStyle>("Text.Large", large);
    }

    // 그 밖의 Element 스타일 기본값이다(UE 키: Checkbox·Docking.Tab·Slider·ProgressBar·ScrollBar·
    // ToolTip·Window, Rocks 이름: Menu·MenuBar).
    if (added) {
        added = styles.Set<CheckBoxStyle>("Checkbox", DefaultCheckBoxStyle());
    }

    if (added) {
        added = styles.Set<DockTabStyle>("Docking.Tab", DefaultDockTabStyle());
    }

    if (added) {
        added = styles.Set<SliderStyle>("Slider", DefaultSliderStyle());
    }

    if (added) {
        added = styles.Set<ProgressBarStyle>("ProgressBar", DefaultProgressBarStyle());
    }

    if (added) {
        added = styles.Set<ScrollBarStyle>("ScrollBar", DefaultScrollBarStyle());
    }

    if (added) {
        added = styles.Set<ToolTipStyle>("ToolTip", DefaultToolTipStyle());
    }

    if (added) {
        added = styles.Set<WindowStyle>("Window", DefaultWindowStyle());
    }

    if (added) {
        added = styles.Set<MenuStyle>("Menu", DefaultMenuStyle());
    }

    if (added) {
        added = styles.Set<MenuBarStyle>("MenuBar", DefaultMenuBarStyle());
    }

    return added;
}

Result<void> AddEditorStyles(StyleSet& styles) noexcept {
    // StarshipStyle.cpp 205행 SetParentStyleName("CoreStyle"), 3394–3395행 메뉴 레이블 여백.
    constexpr Vec4 MenuLabelPadding{0.0f, 0.0f, 0.0f, 0.0f};
    constexpr Vec4 MenuLabelContentPadding{10.0f, 2.0f, 10.0f, 2.0f};
    Result<void> added = styles.SetParentName(CoreStyleName);
    if (added) {
        added = styles.Set<Vec4>("Menu.Label.Padding", MenuLabelPadding);
    }

    if (added) {
        added = styles.Set<Vec4>("Menu.Label.ContentPadding", MenuLabelContentPadding);
    }

    return added;
}

Result<void> AddGameWidgetStyles(StyleSet& styles) noexcept {
    // UMGCoreStyle.cpp: Button 427행, NoBorder 323행, NormalText·SmallText 596·599행, Slider 731행.
    Result<void> added = styles.SetParentName({});
    if (added) {
        added = styles.Set<ButtonStyle>("Button", GameButtonStyle());
    }

    if (added) {
        added = styles.Set<ButtonStyle>("NoBorder", GameNoBorderButtonStyle());
    }

    if (added) {
        added = styles.Set<TextStyle>("NormalText", GameNormalTextStyle());
    }

    if (added) {
        added = styles.Set<TextStyle>("SmallText", GameSmallTextStyle());
    }

    if (added) {
        added = styles.Set<SliderStyle>("Slider", GameSliderStyle());
    }

    return added;
}

} // namespace rocks::ui
