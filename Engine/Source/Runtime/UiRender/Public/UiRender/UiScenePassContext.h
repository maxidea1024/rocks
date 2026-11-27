#pragma once

#include <Core/Math/Color.h>
#include <RenderGraph/RenderGraph.h>

namespace rocks::render {

class RenderLaneContext;
class ShaderSet;

} // namespace rocks::render

namespace rocks::ui {

class UiLaneRenderer;

// 장면 pass 기록 함수가 Render lane에서 받는 문맥이다(UiCore UiScenePassContext의 정의,
// Docs/Design/UiToolkit 18.3절). 기록 함수는 Graph()에 pass를 더해 Target()을 채운다. 대상은 UI
// 그래프에 import된 렌더 타깃이고, 깊이가 필요하면 기록 함수가 그래프에서 만든다.
//
// 첫 pass만 Clear() 색으로 지우고 뒤 pass는 이어 그려야 하므로 TargetLoad()로 load 동작을 받고,
// 대상에 그리는 pass를 더한 뒤 MarkTargetWritten()을 부른다.
//
// 사용 예(앱의 기록 함수):
//   void RecordScene(UiScenePassContext& scene, ScenePacket& packet) noexcept {
//       render::GraphPassAccesses accesses(scene.Graph());
//       accesses.RenderTarget(scene.Target(), scene.TargetLoad(), ToClear(scene.Clear()));
//       scene.Graph().AddPass(SceneName, render::GraphPassKind::Raster,
//                             render::GraphPassFlags::None, accesses,
//                             [packet](render::GraphPassContext& pass) noexcept { ... });
//       scene.MarkTargetWritten();
//   }
class UiScenePassContext {
public:
    UiScenePassContext(render::RenderGraph& graph, render::GraphTexture target, uint32 width,
                       uint32 height, LinearColor clear, render::RenderLaneContext& lane,
                       UiLaneRenderer& renderer, render::ShaderSet& shaders) noexcept
        : graph_(graph), target_(target), width_(width), height_(height), clear_(clear),
          lane_(lane), renderer_(renderer), shaders_(shaders) {
    }

    render::RenderGraph& Graph() const noexcept {
        return graph_;
    }

    render::GraphTexture Target() const noexcept {
        return target_;
    }

    uint32 Width() const noexcept {
        return width_;
    }

    uint32 Height() const noexcept {
        return height_;
    }

    LinearColor Clear() const noexcept {
        return clear_;
    }

    render::GraphLoadAction TargetLoad() const noexcept {
        return written_ ? render::GraphLoadAction::Load : render::GraphLoadAction::Clear;
    }

    void MarkTargetWritten() noexcept {
        written_ = true;
    }

    bool TargetWritten() const noexcept {
        return written_;
    }

    // 렌더 lane 문맥이다(장치·handle 표).
    render::RenderLaneContext& Lane() const noexcept {
        return lane_;
    }

    // UI 렌더러다(UI 묶음을 장면 대상에 그릴 때 — UiLaneRenderer::AddBatchToScene).
    UiLaneRenderer& Renderer() const noexcept {
        return renderer_;
    }

    // UI 그래프가 쓰는 엔진 셰이더 집합이다.
    render::ShaderSet& Shaders() const noexcept {
        return shaders_;
    }

private:
    render::RenderGraph& graph_;
    render::GraphTexture target_;
    uint32 width_;
    uint32 height_;
    LinearColor clear_;
    render::RenderLaneContext& lane_;
    UiLaneRenderer& renderer_;
    render::ShaderSet& shaders_;
    bool written_ = false;
};

} // namespace rocks::ui
