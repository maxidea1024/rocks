#pragma once

#include "GalleryParts.h"

// 텍스트 탭이다(Docs/Design/UiToolkit 16.3절). 위쪽은 UE SStarshipGallery::ConstructTextGallery
// (519–640행)의 세 열과 Rocks 데모 글꼴의 문자 체계 열이고, 그 아래로 UE 에디터 글꼴 지정 전부
// (핵심·제목·용도별), typeface 목록, 문자 체계, 문단 기능 견본을 둔다. 한 행은 [스타일 이름(UE 키)]
// [typeface·크기][견본]이다.
namespace rocks::ui::gallery {

Result<SharedPtr<Element>> BuildTextPage(GalleryContext& context) noexcept;

} // namespace rocks::ui::gallery
