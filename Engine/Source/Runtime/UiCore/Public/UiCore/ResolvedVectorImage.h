#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Math/Rect.h>

namespace rocks::ui {

// 벡터 이미지를 래스터화한 자리다(Docs/Design/UiToolkit 17.2절). resource는 렌더러 brush 등록부의
// 텍스처, uv는 그 안의 정규 좌표다.
struct ResolvedVectorImage {
    SlotHandle resource{};
    Box2<float> uv{{0, 0}, {1, 1}};
};

} // namespace rocks::ui
