#include "DemoLog.h"
#include "SurfaceSelfTest.h"

#include <Core/Diagnostics/Log.h>
#include <UiRender/UiLaneRenderer.h>

namespace rocks::ui::detail {

// 셰이더 다시 만들기 검사 lane 작업 값이다. 결과는 ui의 화면 읽기 표에 capture 번호로 보관한다(게임
// lane 주소를 넘기지 않는다).
struct ShaderReloadCheck {
    UiLaneRenderer* ui = nullptr;
    uint64 capture = 0;
};

} // namespace rocks::ui::detail

ROCKS_RENDER_TRANSFERABLE(rocks::ui::detail::ShaderReloadCheck);

namespace rocks::ui {

namespace {

// 검사 결과의 capture 번호다(LaneUiRenderer와 평면 검사 번호 구간 밖).
constexpr uint64 ShaderReloadCapture = 3ull << 61;

// 렌더 lane에서 UiLaneRenderer의 셰이더 다시 만들기 규칙을 본다(Docs/Design/UiToolkit 18.4절 —
// 셰이더 감시 교체): 파이프라인을 만든 집합에 lease를 잡고 있고, 같은 집합으로 다시 만들면 아무
// 일도 없고, Ready가 아닌 집합이면 NotReady로 실패하며 옛 집합·lease를 그대로 둔다.
void CheckShaderReloadOnLane(render::RenderLaneContext& context,
                             detail::ShaderReloadCheck& task) noexcept {
    UiLaneRenderer& ui = *task.ui;
    const auto finish = [&](StringView failedStep) noexcept {
        if (!failedStep.IsEmpty()) {
            Log::Error(LogUiDemo, "Shader reload self-test failed: {Step}", failedStep);
        }

        ui.StoreCapture(task.capture, Array<uint8>(ui.GetAllocator()),
                        failedStep.IsEmpty() ? ErrorCode::None : ErrorCode::InvalidArgument);
    };
    render::ShaderSet* opened = ui.Shaders();
    if (!opened) {
        return finish("the renderer has no shader set");
    }

    const uint32 leases = opened->LeaseCount();
    if (leases == 0) {
        return finish("the renderer holds no lease on its shader set");
    }

    if (!ui.ReloadShaders(context.Device(), *opened) || ui.Shaders() != opened ||
        opened->LeaseCount() != leases) {
        return finish("reloading with the same set changed the renderer");
    }

    // 열지 않은 집합은 Ready가 아니다(lease를 받지 않는다).
    render::ShaderSetDesc desc;
    desc.allocator = &ui.GetAllocator();
    desc.device = &context.Device();
    render::ShaderSet unopened(desc);
    auto reloaded = ui.ReloadShaders(context.Device(), unopened);
    if (reloaded || reloaded.Error() != ErrorCode::NotReady) {
        return finish("reloading with a set that is not ready did not fail with NotReady");
    }

    if (ui.Shaders() != opened || opened->LeaseCount() != leases || unopened.LeaseCount() != 0) {
        return finish("a failed reload did not keep the previous set and lease");
    }

    return finish({});
}

} // namespace

int VerifyShaderReload(LaneUiRenderer& renderer, render::RenderLane& lane) noexcept {
    detail::ShaderReloadCheck check;
    check.ui = &renderer.LaneRenderer();
    check.capture = ShaderReloadCapture;
    if (!lane.PostSurfaceAndWait(Move(check), &CheckShaderReloadOnLane)) {
        return 170;
    }

    // 실패한 다시 만들기 뒤에도 옛 파이프라인으로 그린다 — 이어지는 표면·평면 self-test가 같은
    // 렌더러로 화소를 확인한다.
    Array<uint8> unused(renderer.LaneRenderer().GetAllocator());
    return renderer.LaneRenderer().TakeCapture(ShaderReloadCapture, unused) ? 0 : 171;
}

} // namespace rocks::ui
