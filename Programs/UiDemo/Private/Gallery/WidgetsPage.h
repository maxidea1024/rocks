#pragma once

#include "GalleryGrid.h"

#include <Core/Memory/UniquePtr.h>
#include <UiFramework/Controls/EditableTextBox.h>

// 위젯 탭이다(UE SStarshipGallery::ConstructWidgetGallery, 716–1400행). UE 순서로
// 버튼(Primary·기본· Simple), 체크 상자 세 상태, 그 밖의 기본 입력 위젯을 행마다 놓는다. 아직 UE
// 스타일이 아닌 위젯은 UW4b에서 바꾼다(Docs/Design/UiToolkit 9.1절 UW4).
namespace rocks::ui::gallery {

// "Text Input" 묶음의 견본이다(Docs/Design/UiToolkit 22.9절 — self-test가 초점·타자·오류 팝업·
// 스크롤 막대를 본다).
struct TextInputSamples {
    // 힌트 상자와 그 값 레이블(바뀐 값·확정 사유)이다.
    SharedPtr<EditableTextBox> box;
    SharedPtr<TextLabel> value;
    // 오류 표지·팝업 상자다.
    SharedPtr<EditableTextBox> error;
    // 긴 글이라 세로 스크롤 막대가 보이는 여러 줄 상자다.
    SharedPtr<EditableTextBox> multiLine;
};

// 체크 상자 형식 견본이다(23.3절 — self-test가 토글 바탕·라디오 묶음을 본다).
inline constexpr usize RadioSampleCount = 3;

struct CheckSamples {
    SharedPtr<CheckBox> toggle;
    SharedPtr<CheckBox> radios[RadioSampleCount];
};

struct WidgetsPage {
    SharedPtr<Element> root;
    // self-test가 행의 활성 상태를 바꿔 본다.
    UniquePtr<GalleryGrid> grid;
    TextInputSamples textInput;
    CheckSamples checks;
};

Result<WidgetsPage> BuildWidgetsPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
