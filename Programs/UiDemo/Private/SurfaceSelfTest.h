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
// UI 렌더러의 셰이더 다시 만들기 규칙(lease 유지, 실패 시 옛 집합 유지)을 렌더 lane에서
// 본다(18.4절).
int VerifyShaderReload(LaneUiRenderer& renderer, render::RenderLane& lane) noexcept;
// 숨은 창의 스왑 체인으로 창 pass 규칙(UI만 그리면 present 한 번, 첫 pass Load 거부, Load인데
// 크기가 다르면 거부)을 본다(18.4절 — RenderBase 8절 "스왑체인 공유 설계").
int VerifyWindowPresents(Allocator& allocator, LaneUiRenderer& renderer,
                         render::RenderLane& lane) noexcept;

} // namespace rocks::ui
