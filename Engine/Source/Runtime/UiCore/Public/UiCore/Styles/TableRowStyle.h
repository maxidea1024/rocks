#pragma once

#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 목록 행 스타일이다(UE FTableRowStyle — Docs/Design/UiToolkit 23.2절). 지금은 콤보 펼침 목록
// 행("ComboBox.Row")이 쓰는 칸만 둔다: 짝·홀 행 바탕과 올림 바탕, 선택(활성) 행 바탕, 초점 선택
// 외곽선, 글자색. 목록(ListView) 스타일을 옮길 때 나머지 칸(놓기 표시·비활성 선택 등)을 더한다.
struct TableRowStyle {
    StyleBrush evenRow{};
    StyleBrush evenRowHovered{};
    StyleBrush oddRow{};
    StyleBrush oddRowHovered{};
    StyleBrush active{};
    StyleBrush activeHovered{};
    StyleBrush selectorFocused{};
    ColorRef text{ColorSource::Foreground};
    ColorRef selectedText{ColorSource::Foreground};
    bool operator==(const TableRowStyle&) const noexcept = default;
};

} // namespace rocks::ui
