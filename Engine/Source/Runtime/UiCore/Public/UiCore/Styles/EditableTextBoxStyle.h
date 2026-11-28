#pragma once

#include <Core/Math/Vector4.h>
#include <UiCore/Styles/EditableTextStyle.h>
#include <UiCore/Styles/StyleBrush.h>

namespace rocks::ui {

// 편집 글자 상자 스타일이다(UE FEditableTextBoxStyle — 키 "NormalEditableTextBox", Docs/Design/
// UiToolkit 22.3절). 바탕은 상태(보통·올림·초점·읽기 전용)마다 고르고, 전경은 보통·초점·읽기
// 전용으로 고른다(읽기 전용 기본은 물려받은 전경 — UE UseForeground). padding은 (왼쪽, 위, 오른쪽,
// 아래)다. errorMarker는 오류 "!" 표지 바탕, errorText는 오류 팝업 글자다(UE SPopupErrorText).
struct EditableTextBoxStyle {
    StyleBrush normal{};
    StyleBrush hovered{};
    StyleBrush focused{};
    StyleBrush readOnly{};
    Vec4 padding{};
    ColorRef foreground{ColorSource::Foreground};
    ColorRef focusedForeground{ColorSource::Foreground};
    ColorRef readOnlyForeground{ColorSource::Foreground};
    EditableTextStyle text{};
    StyleBrush errorMarker{};
    TextStyle errorText{};
    bool operator==(const EditableTextBoxStyle&) const noexcept = default;
};

} // namespace rocks::ui
