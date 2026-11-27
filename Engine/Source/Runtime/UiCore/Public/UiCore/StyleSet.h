#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <UiCore/Styles/ButtonStyle.h>
#include <UiCore/Styles/CheckBoxStyle.h>
#include <UiCore/Styles/DockTabStyle.h>
#include <UiCore/Styles/MenuBarStyle.h>
#include <UiCore/Styles/MenuStyle.h>
#include <UiCore/Styles/ProgressBarStyle.h>
#include <UiCore/Styles/ScrollBarStyle.h>
#include <UiCore/Styles/SliderStyle.h>
#include <UiCore/Styles/TextStyle.h>
#include <UiCore/Styles/ToolTipStyle.h>
#include <UiCore/Styles/WindowStyle.h>
#include <UiCore/Styles/StyleBrush.h>
#include <UiCore/ColorRef.h>

// 이름으로 형식별 스타일을 찾는 표다(UE FSlateStyleSet — Docs/Design/UiToolkit 4.1·20.11절).
// Element 스타일 구조체와 brush·색·실수·여백 표를 둔다. 부모 집합 이름을 가질 수 있고, 찾지 못한
// 이름은 StyleRegistry가 부모로 올라가 찾는다(UE ParentStyleName). 전역 등록부는 없다 — 호스트가
// StyleRegistry에 등록해 Application에 넘긴다.
//
// 사용 예:
//   StyleSet styles(allocator);
//   if (auto added = AddCoreStyles(styles); !added) { return added; }
//   const ButtonStyle* primary = styles.Find<ButtonStyle>("PrimaryButton");
namespace rocks::ui {

// 스타일·집합 이름 비교다. UE FName처럼 ASCII 대소문자를 가리지 않는다("Scrollbar"와 "ScrollBar"는
// 같다 — UE도 두 철자로 같은 키를 찾는다, StarshipStyle.cpp 170행).
inline bool SameStyleName(StringView a, StringView b) noexcept {
    return a.ByteCount() == b.ByteCount() && a.StartsWith(b, StringComparison::IgnoreAsciiCase);
}

class ROCKS_UICORE_API StyleSet {
public:
    explicit StyleSet(Allocator& allocator) noexcept;
    StyleSet(const StyleSet&) = delete;
    StyleSet& operator=(const StyleSet&) = delete;

    // 같은 이름(ASCII 대소문자 무시)이 있으면 값을 바꾼다. 빈 이름은 InvalidArgument, 할당 실패면
    // 표를 바꾸지 않는다.
    // 바꿀 때마다 Revision이 오른다(등록부를 쓰는 Application이 배치·그리기를 다시 한다).
    template <typename T>
    Result<void> Set(StringView name, const T& value) noexcept {
        return SetValue(name, value);
    }

    // 없으면 nullptr이다(부모는 보지 않는다 — StyleRegistry가 본다). 주소는 다음 Set 전까지
    // 유효하다.
    template <typename T>
    const T* Find(StringView name) const noexcept {
        return FindValue(name, static_cast<const T*>(nullptr));
    }

    // 부모 집합 이름이다(빈 이름이면 부모 없음).
    Result<void> SetParentName(StringView name) noexcept;
    StringView ParentName() const noexcept;
    uint64 Revision() const noexcept;

    Result<void> SetValue(StringView name, const ButtonStyle& value) noexcept;
    const ButtonStyle* FindValue(StringView name, const ButtonStyle*) const noexcept;
    Result<void> SetValue(StringView name, const CheckBoxStyle& value) noexcept;
    const CheckBoxStyle* FindValue(StringView name, const CheckBoxStyle*) const noexcept;
    Result<void> SetValue(StringView name, const DockTabStyle& value) noexcept;
    const DockTabStyle* FindValue(StringView name, const DockTabStyle*) const noexcept;
    Result<void> SetValue(StringView name, const MenuBarStyle& value) noexcept;
    const MenuBarStyle* FindValue(StringView name, const MenuBarStyle*) const noexcept;
    Result<void> SetValue(StringView name, const MenuStyle& value) noexcept;
    const MenuStyle* FindValue(StringView name, const MenuStyle*) const noexcept;
    Result<void> SetValue(StringView name, const ProgressBarStyle& value) noexcept;
    const ProgressBarStyle* FindValue(StringView name, const ProgressBarStyle*) const noexcept;
    Result<void> SetValue(StringView name, const ScrollBarStyle& value) noexcept;
    const ScrollBarStyle* FindValue(StringView name, const ScrollBarStyle*) const noexcept;
    Result<void> SetValue(StringView name, const SliderStyle& value) noexcept;
    const SliderStyle* FindValue(StringView name, const SliderStyle*) const noexcept;
    Result<void> SetValue(StringView name, const TextStyle& value) noexcept;
    const TextStyle* FindValue(StringView name, const TextStyle*) const noexcept;
    Result<void> SetValue(StringView name, const ToolTipStyle& value) noexcept;
    const ToolTipStyle* FindValue(StringView name, const ToolTipStyle*) const noexcept;
    Result<void> SetValue(StringView name, const WindowStyle& value) noexcept;
    const WindowStyle* FindValue(StringView name, const WindowStyle*) const noexcept;
    Result<void> SetValue(StringView name, const StyleBrush& value) noexcept;
    const StyleBrush* FindValue(StringView name, const StyleBrush*) const noexcept;
    Result<void> SetValue(StringView name, const ColorRef& value) noexcept;
    const ColorRef* FindValue(StringView name, const ColorRef*) const noexcept;
    Result<void> SetValue(StringView name, const float& value) noexcept;
    const float* FindValue(StringView name, const float*) const noexcept;
    Result<void> SetValue(StringView name, const Vec4& value) noexcept;
    const Vec4* FindValue(StringView name, const Vec4*) const noexcept;

private:
    template <typename T>
    struct Entry {
        String name;
        T value;
    };

    template <typename T>
    Result<void> Store(Array<Entry<T>>& entries, StringView name, const T& value) noexcept;
    template <typename T>
    static const T* Lookup(const Array<Entry<T>>& entries, StringView name) noexcept;

    Allocator& allocator_;
    String parent_;
    uint64 revision_ = 0;
    Array<Entry<ButtonStyle>> buttons_;
    Array<Entry<CheckBoxStyle>> checkBoxes_;
    Array<Entry<DockTabStyle>> dockTabs_;
    Array<Entry<MenuBarStyle>> menuBars_;
    Array<Entry<MenuStyle>> menus_;
    Array<Entry<ProgressBarStyle>> progressBars_;
    Array<Entry<ScrollBarStyle>> scrollBars_;
    Array<Entry<SliderStyle>> sliders_;
    Array<Entry<TextStyle>> texts_;
    Array<Entry<ToolTipStyle>> toolTips_;
    Array<Entry<WindowStyle>> windows_;
    Array<Entry<StyleBrush>> brushes_;
    Array<Entry<ColorRef>> colors_;
    Array<Entry<float>> floats_;
    Array<Entry<Vec4>> margins_;
};

// UE 5.8 핵심 스타일 중 Rocks 형식이 있는 항목을 넣는다(14장·20.11절). 이름은 UE 키와 같다
// (StarshipCoreStyle.cpp — 버튼 1333–1339행, 글자 1503–1511행 등). UE에 같은 키가 없는 Rocks 형식은
// Rocks 이름("Menu"·"MenuBar")이다.
ROCKS_UICORE_API Result<void> AddCoreStyles(StyleSet& styles) noexcept;

// UE 5.8 에디터 스타일(StarshipStyle.cpp)을 넣는다(20.11절). 부모는 CoreStyle이다(205행). Rocks
// 형식이 있는 키는 UE도 다시 정하지 않으므로 부모 값을 받고, 에디터에만 있는 여백
// "Menu.Label.Padding"·"Menu.Label.ContentPadding"(3394–3395행)만 둔다.
ROCKS_UICORE_API Result<void> AddEditorStyles(StyleSet& styles) noexcept;

// UE 5.8 게임 위젯 스타일(UMGCoreStyle.cpp)을 넣는다(20.11절 — GameWidgetStyles.h). 부모는
// 없다(UE와 같다).
ROCKS_UICORE_API Result<void> AddGameWidgetStyles(StyleSet& styles) noexcept;

} // namespace rocks::ui
