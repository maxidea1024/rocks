#pragma once

#include <UiCore/Styles/TextStyle.h>

namespace rocks::ui {

// 편집 글자 스타일이다(UE FEditableTextStyle — 키 "NormalEditableText", Docs/Design/UiToolkit
// 22.3절). selection은 선택 바탕, caret은 caret 색, composition은 IME 조합 점선 밑줄 색이다.
// hintOpacity는 본문이 빌 때 힌트를 그리는 전경색 불투명도다(UE HintTextOpacity 기본 0.35).
struct EditableTextStyle {
    TextStyle text{};
    ColorRef selection{ColorSource::Foreground};
    ColorRef caret{ColorSource::Foreground};
    ColorRef composition{ColorSource::Foreground};
    float hintOpacity = 0.35f;
    bool operator==(const EditableTextStyle&) const noexcept = default;
};

} // namespace rocks::ui
