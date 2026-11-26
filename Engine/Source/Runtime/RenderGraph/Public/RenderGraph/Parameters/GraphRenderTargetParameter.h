#pragma once

#include <RenderGraph/GraphPassAccesses.h>

// 렌더 타깃 slot 묶음 멤버다(ShaderSystem 16.2절 SSD59 RenderTargets, UE
// RENDER_TARGET_BINDING_SLOTS). Raster pass만 받는다. 수집기가 색 colors[0, colorCount)를
// GraphPassAccesses::ColorTarget으로(배열 순서가 slot 순서), hasDepth면 depth를 DepthTarget으로
// 넣는다. 구조체에 하나만 둘 수 있다.
//
// 사용 예:
//   render::GraphRenderTargetParameter targets;
//   parameters->targets.colors[0] = {sceneColor, GraphLoadAction::Load};
//   parameters->targets.colorCount = 1;
namespace rocks::render {

struct alignas(16) GraphRenderTargetParameter {
    GraphColorTarget colors[graphics::MaxColorTargets];
    uint32 colorCount = 0;
    GraphDepthTarget depth;
    bool hasDepth = false;
};

} // namespace rocks::render
