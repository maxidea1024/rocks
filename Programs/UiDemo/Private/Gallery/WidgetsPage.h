#pragma once

#include "GalleryGrid.h"

#include <Core/Memory/UniquePtr.h>

// 위젯 탭이다(UE SStarshipGallery::ConstructWidgetGallery, 716–1400행). UE 순서로
// 버튼(Primary·기본· Simple), 체크 상자 세 상태, 그 밖의 기본 입력 위젯을 행마다 놓는다. 아직 UE
// 스타일이 아닌 위젯은 UW4b에서 바꾼다(Docs/Design/UiToolkit 9.1절 UW4).
namespace rocks::ui::gallery {

struct WidgetsPage {
    SharedPtr<Element> root;
    // self-test가 행의 활성 상태를 바꿔 본다.
    UniquePtr<GalleryGrid> grid;
};

Result<WidgetsPage> BuildWidgetsPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
