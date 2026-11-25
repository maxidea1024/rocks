#pragma once

#include <Core/Math/Vector2.h>
#include <UiCore/VectorImageKey.h>

// 엔진 아이콘 키다(Docs/Design/UiToolkit 17.3절 — Engine/Content/Ui/Icons 아래 경로). 스타일
// 기본값과 위젯이 이 이름으로 아이콘을 가리킨다. UE 대응 이름은 17.3절 표에 있다.
namespace rocks::ui::icons {

// 크기 격자다(UE CoreStyle Icon16x16·Icon20x20 — 14장 Icon sizes).
inline constexpr Vec2 Icon16{16.0f, 16.0f};
inline constexpr Vec2 Icon20{20.0f, 20.0f};

inline constexpr VectorImageKey Check = VectorImageKey::FromPath("CoreWidgets/CheckBox/Check");
inline constexpr VectorImageKey Indeterminate =
    VectorImageKey::FromPath("CoreWidgets/CheckBox/Indeterminate");
inline constexpr VectorImageKey RadioOn = VectorImageKey::FromPath("CoreWidgets/CheckBox/RadioOn");
inline constexpr VectorImageKey RadioOff =
    VectorImageKey::FromPath("CoreWidgets/CheckBox/RadioOff");
inline constexpr VectorImageKey WideChevronDown =
    VectorImageKey::FromPath("CoreWidgets/ComboBox/WideChevronDown");
inline constexpr VectorImageKey SortUp = VectorImageKey::FromPath("CoreWidgets/TableView/SortUp");
inline constexpr VectorImageKey SortDown =
    VectorImageKey::FromPath("CoreWidgets/TableView/SortDown");
inline constexpr VectorImageKey CaretRight = VectorImageKey::FromPath("Common/CaretRight");
inline constexpr VectorImageKey CaretDown = VectorImageKey::FromPath("Common/CaretDown");
inline constexpr VectorImageKey ChevronRight = VectorImageKey::FromPath("Common/ChevronRight");
inline constexpr VectorImageKey CloseSmall = VectorImageKey::FromPath("Common/CloseSmall");
inline constexpr VectorImageKey Search = VectorImageKey::FromPath("Common/Search");

} // namespace rocks::ui::icons
