#pragma once

#include "GalleryGrid.h"

#include <Core/Memory/UniquePtr.h>

// 표면·캐시·흐림 탭이다(Docs/Design/UiToolkit 16.2절 Effects). 오프스크린 표면을 쓰는 부품을
// 보인다: 배경 흐림(강도 슬라이더, 저품질 대체 brush), 리테이너(그린 결과 보관·effect tint), 캐시
// 영역, 장면 텍스처 위에 UI를 올린 Viewport. 색 버퍼 복사(PostBufferUpdate)는 복사한 버퍼를 쓰는
// 쪽이 있어야 보여서 16.10절에 둔다.
namespace rocks::ui::gallery {

struct EffectsPage {
    SharedPtr<Element> root;
    UniquePtr<GalleryGrid> grid;
};

Result<EffectsPage> BuildEffectsPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
