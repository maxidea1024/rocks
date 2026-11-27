#pragma once

#include <UiCore/Rendering/UiTexture.h>

namespace rocks::ui {

// viewport가 그릴 대상이다. 렌더 타깃은 SurfaceRecorder가 viewport 크기로 만든다.
struct ViewportTarget {
    UiTextureId texture{};
    uint32 width = 0;
    uint32 height = 0;
};

} // namespace rocks::ui
