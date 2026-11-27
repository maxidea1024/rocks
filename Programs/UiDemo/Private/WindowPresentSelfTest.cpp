#include "DemoLog.h"
#include "SurfaceSelfTest.h"

#include <Core/Diagnostics/Log.h>
#include <PlatformWindow/Window.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/RenderGraph.h>
#include <UiRender/UiLaneRenderer.h>

namespace rocks::ui::detail {

// 창 present 검사 프레임 packet이다. 결과는 ui의 화면 읽기 표에 capture 번호로 보관한다(게임 lane
// 주소를 넘기지 않는다).
struct WindowPresentCheck {
    UiLaneRenderer* ui = nullptr;
    UiWindowSurfaceId surface{};
    uint32 width = 0;
    uint32 height = 0;
    uint64 capture = 0;
};

} // namespace rocks::ui::detail

ROCKS_LANE_TRANSFERABLE(rocks::ui::detail::WindowPresentCheck);

namespace rocks::ui {

namespace {

// 검사 결과의 capture 번호다(LaneUiRenderer·평면·셰이더 검사 번호 구간 밖).
constexpr uint64 WindowPresentCapture = 5ull << 60;
// 검사 창의 client 크기다(픽셀).
constexpr uint32 WindowPresentWidth = 32;
constexpr uint32 WindowPresentHeight = 24;

// 렌더 lane 프레임에서 창 pass의 스왑 체인 공유 규칙을 본다(Docs/Design/UiToolkit 18.4절 —
// RenderBase 8절 "스왑체인 공유 설계"): UI만 그리는 그래프는 창마다 present 한 번, 그 스왑 체인의
// 첫 pass가 Load면 그래프가 거부하고 present도 없다, Load인데 창 크기가 다르면 InvalidArgument다.
void CheckWindowPresentOnLane(render::RenderFrameContext& context,
                              detail::WindowPresentCheck& task) noexcept {
    UiLaneRenderer& ui = *task.ui;
    const auto finish = [&](StringView failedStep) noexcept {
        if (!failedStep.IsEmpty()) {
            Log::Error(LogUiDemo, "Window present self-test failed: {Step}", failedStep);
        }

        ui.StoreCapture(task.capture, Array<uint8>(ui.GetAllocator()),
                        failedStep.IsEmpty() ? ErrorCode::None : ErrorCode::InvalidArgument);
    };
    graphics::Device& device = context.Device();
    render::GraphResourcePool pool(ui.GetAllocator(), device);
    UiFrameTarget target;
    target.window = task.surface;
    target.width = task.width;
    target.height = task.height;
    const uint64 before = device.PresentCount();
    {
        render::RenderGraph graph(render::MakeRenderGraphDesc(context, pool));
        auto added = ui.AddPasses(graph, context, target);
        if (!added || !graph.Execute()) {
            return finish("a UI-only window frame did not execute");
        }

        if (graph.Stats().presents != 1 || device.PresentCount() != before + 1) {
            return finish("a UI-only window frame did not present exactly once");
        }
    }

    {
        // 앞 pass가 없는데 Load로 시작하면 그래프가 거부한다(present 뒤 백버퍼 내용은 정의되지
        // 않는다).
        render::RenderGraph graph(render::MakeRenderGraphDesc(context, pool));
        auto added = ui.AddPasses(graph, context, target, render::GraphLoadAction::Load);
        const bool rejected = !added || !graph.Execute();
        if (!rejected || device.PresentCount() != before + 1) {
            return finish("a window frame that starts with Load was not rejected");
        }
    }

    {
        // Load인데 창 크기가 다르면 앞 pass가 그린 백버퍼를 바꾸지 않고 거부한다.
        render::RenderGraph graph(render::MakeRenderGraphDesc(context, pool));
        UiFrameTarget resized = target;
        resized.width = task.width / 2;
        auto added = ui.AddPasses(graph, context, resized, render::GraphLoadAction::Load);
        if (added || added.Error() != ErrorCode::InvalidArgument) {
            return finish("a Load window pass with a different size was not rejected");
        }
    }

    return finish({});
}

} // namespace

int VerifyWindowPresents(Allocator& allocator, LaneUiRenderer& renderer,
                         render::RenderLane& lane) noexcept {
    platform::WindowDesc desc;
    desc.title = "UiDemo present check";
    desc.width = WindowPresentWidth;
    desc.height = WindowPresentHeight;
    desc.resizable = false;
    desc.visible = false;
    auto window = platform::Window::Create(allocator, desc);
    if (!window) {
        return 180;
    }

    const auto native = window.Value()->NativeHandle();
    UiWindowSurfaceDesc surfaceDesc;
    surfaceDesc.nativeWindow = native.window;
    surfaceDesc.nativeInstance = native.instance;
    surfaceDesc.width = WindowPresentWidth;
    surfaceDesc.height = WindowPresentHeight;
    auto surface = renderer.CreateWindowSurface(surfaceDesc);
    if (!surface) {
        return 181;
    }

    int result = 0;
    auto ticket = lane.BeginFrame();
    if (!ticket) {
        result = 182;
    } else {
        detail::WindowPresentCheck check;
        check.ui = &renderer.LaneRenderer();
        check.surface = surface.Value();
        check.width = WindowPresentWidth;
        check.height = WindowPresentHeight;
        check.capture = WindowPresentCapture;
        auto submitted =
            lane.SubmitFrame(Move(ticket.Value()), Move(check), &CheckWindowPresentOnLane);
        if (!submitted) {
            if (ticket.Value().IsValid()) {
                lane.AbandonFrame(Move(ticket.Value()));
            }

            result = 183;
        } else if (!renderer.Flush()) {
            result = 184;
        } else {
            Array<uint8> unused(renderer.LaneRenderer().GetAllocator());
            result = renderer.LaneRenderer().TakeCapture(WindowPresentCapture, unused) ? 0 : 185;
        }
    }

    // 창 표면은 lane이 그 창의 프레임을 모두 처리한 뒤 부순다(native 창보다 먼저 — 18.4절).
    (void)renderer.DestroyWindowSurface(surface.Value());
    return result;
}

} // namespace rocks::ui
