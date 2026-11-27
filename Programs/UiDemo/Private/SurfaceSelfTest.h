#pragma once

#include <RenderBase/RenderLane.h>
#include <UiRender/LaneUiRenderer.h>

namespace rocks::ui {

// 창을 만들지 않고 장면·보관 표면·후처리 버퍼를 실제 장치에서 합성하고 픽셀을 검사한다(Render lane
// 경로 — Docs/Design/UiToolkit 18.5절).
int VerifyWorldWidgetPlane(Allocator& allocator, LaneUiRenderer& renderer,
                           render::RenderLane& lane) noexcept;
int VerifyViewportWorld(Allocator& allocator, UiRenderer& renderer) noexcept;
int VerifyWidgetSurfaces(Allocator& allocator, LaneUiRenderer& renderer,
                         render::RenderLane& lane) noexcept;

} // namespace rocks::ui
