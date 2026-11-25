#pragma once

#include <UiRender/DrawBatchRenderer.h>

namespace rocks::ui {

// 창을 만들지 않고 장면·보관 표면·후처리 버퍼를 실제 장치에서 합성하고 픽셀을 검사한다.
int VerifyWorldWidgetPlane(Allocator& allocator, graphics::Device& device) noexcept;
int VerifyViewportWorld(Allocator& allocator, graphics::Device& device,
                        graphics::PipelineHandle solid) noexcept;
int VerifyWidgetSurfaces(Allocator& allocator, graphics::Device& device,
                         graphics::PipelineHandle solid) noexcept;

} // namespace rocks::ui
