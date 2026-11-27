#pragma once

#include <RenderBase/EngineShaderHost.h>
#include <RenderBase/RenderLane.h>
#include <UiRender/UiLaneRenderer.h>

namespace rocks::ui {

// UiDemo의 렌더 lane 처리기다(Docs/Design/UiToolkit 18.5절). lane 시작 때 엔진 셰이더 집합과 UI
// 렌더러(UiLaneRenderer)를 열고, 정지 때 닫는다. 창 표면·텍스처·프레임은 LaneUiRenderer가 lane
// 작업으로 보낸다.
//
// 사용 예:
//   DemoLaneProcessor processor(allocator);
//   render::RenderLaneConfig config;
//   config.processor = &processor;
//   config.pump = DemoLaneProcessor::Pump();
//   lane.Start(config);
//   LaneUiRenderer renderer(allocator, lane, processor.Ui());
class DemoLaneProcessor final : public render::RenderLaneProcessor {
public:
    explicit DemoLaneProcessor(Allocator& allocator) noexcept;

    Result<void> OnLaneStart(render::RenderLaneContext& context) noexcept override;
    void OnLaneStop(render::RenderLaneContext& context) noexcept override;

    UiLaneRenderer& Ui() noexcept {
        return ui_;
    }

    // lane 대기 중 펌프다. 보낸 메시지만 처리한다(입력 같은 게시 메시지는 큐에 남아 다음 정상
    // 펌프가 처리한다 — 재진입 없음, 스왑 체인이 창 스레드로 보내는 메시지 교착 방지, 18.5절).
    static render::RenderLanePump Pump() noexcept;

private:
    render::EngineRenderResources resources_;
    UiLaneRenderer ui_;
};

} // namespace rocks::ui
