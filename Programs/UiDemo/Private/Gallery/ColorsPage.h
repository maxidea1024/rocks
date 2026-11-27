#pragma once

#include "GalleryParts.h"

// 색 탭이다(UE SStarshipGallery::ConstructColorsGallery, 219–392행). 테마 slot마다 둥근 견본에
// 이름과 sRGB 16진 값을 적고, UE와 같은 순서·묶음(BASE·FOREGROUND·ACCENT COLORS)으로 늘어놓는다.
namespace rocks::ui::gallery {

Result<SharedPtr<Element>> BuildColorsPage(GalleryContext& context,
                                           const ThemeColors& theme) noexcept;

} // namespace rocks::ui::gallery
