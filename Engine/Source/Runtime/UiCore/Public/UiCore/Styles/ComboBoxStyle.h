#pragma once

#include <UiCore/Styles/ComboButtonStyle.h>

namespace rocks::ui {

// 콤보 상자 스타일이다(UE FComboBoxStyle — Docs/Design/UiToolkit 23.2절). 콤보 버튼 모양과 펼침
// 목록 행 여백을 담는다. 행 바탕·글자색은 TableRowStyle("ComboBox.Row")이다.
struct ComboBoxStyle {
    ComboButtonStyle comboButton{};
    // 버튼 안 내용 여백이다(UE FComboBoxStyle::ContentPadding — 콤보 버튼 여백 대신 쓴다).
    Vec4 contentPadding{};
    // 펼침 목록 행 여백이다(UE MenuRowPadding).
    Vec4 menuRowPadding{};
    bool operator==(const ComboBoxStyle&) const noexcept = default;
};

} // namespace rocks::ui
