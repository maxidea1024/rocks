#pragma once

#include "GalleryParts.h"

// 아이콘 탭이다(Docs/Design/UiToolkit 16.2·17.4절 — UE SStarshipGallery "Icons" 탭 대응). 등록한
// SVG 아이콘 전부를 16·20·32 px(Foreground)와 20 px tint 둘(Primary·Warning)로 보이고, 골라 쓸 수
// 있게 키 경로를 적는다.
namespace rocks::ui::gallery {

Result<SharedPtr<Element>> BuildIconsPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
