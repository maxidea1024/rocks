#pragma once

#include "GalleryGrid.h"

#include <Core/Memory/UniquePtr.h>
#include <UiFramework/Docking/DockManager.h>

// 배치 탭이다(Docs/Design/UiToolkit 16.6절). 배치 부품마다 테두리를 그린 무대 안에 색 상자를 놓아
// 배치 규칙(Auto·Fill 비율, 칸 합치기, 줄바꿈, 앵커, 비율 맞춤, 스크롤, 전환)이 보이게 하고,
// 마지막에 탭 묶음 셋을 나눈 도킹 영역을 둔다(분할선 끌기·탭 끌어 놓기 — self-test가 이 영역을
// 쓴다).
namespace rocks::ui::gallery {

struct LayoutPage {
    SharedPtr<Element> root;
    UniquePtr<GalleryGrid> grid;
    // 배치 탭 안의 도킹 영역이다.
    SharedPtr<DockManager> docking;
};

Result<LayoutPage> BuildLayoutPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
