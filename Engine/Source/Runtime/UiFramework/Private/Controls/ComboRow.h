#pragma once

#include <UiFramework/Controls/Button.h>

namespace rocks::ui {

class ComboBox;

// 콤보 펼침 목록의 행이다(UE SComboRow·"ComboBox.Row" — Docs/Design/UiToolkit 23.2절). 버튼 모양은
// 콤보가 행 스타일에서 만들어 준다(바탕 없음·올림 Primary·글자 White). 키보드로 옮긴 강조 행도
// 올림 모양이다. 행은 콤보가 연 메뉴 안에서만 살고, 콤보는 닫히거나 사라질 때 메뉴를 떼므로 소유
// 콤보를 raw로 가리킨다.
class ComboRow final : public Button {
    ROCKS_UI_ELEMENT(ComboRow, Button)

public:
    ComboRow(Allocator& allocator, const ComboBox& owner, int32 index) noexcept;

    bool IsHighlighted() const noexcept override;

private:
    const ComboBox& owner_;
    int32 index_;
};

} // namespace rocks::ui
