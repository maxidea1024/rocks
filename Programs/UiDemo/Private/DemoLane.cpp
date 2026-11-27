#include "DemoLane.h"
#include "DemoLog.h"
#include "DemoShaders.h"

#include <Core/Diagnostics/Log.h>
#include <PlatformWindow/Window.h>

namespace rocks::ui {

namespace {

void PumpSentMessagesOnly(void*) noexcept {
    platform::PumpSentMessages();
}

} // namespace

DemoLaneProcessor::DemoLaneProcessor(Allocator& allocator) noexcept
    : resources_(allocator), ui_(allocator) {
}

Result<void> DemoLaneProcessor::OnLaneStart(render::RenderLaneContext& context) noexcept {
    graphics::Device& device = context.Device();
    if (auto opened = OpenDemoShaders(resources_, device); !opened) {
        Log::Error(LogUiDemo, "The UI shader set could not be opened: {Problem}",
                   resources_.Problem());
        return opened;
    }

    // 집합 원천으로 연다 — 감시 재컴파일의 셰이더 교체를 프레임마다 받는다(SSD76).
    return ui_.Open(device, resources_, graphics::PixelFormat::RGBA8UnormSrgb);
}

void DemoLaneProcessor::OnLaneStop(render::RenderLaneContext& context) noexcept {
    ui_.Close(context.Device());
    resources_.Retire(context.Device());
}

render::RenderLanePump DemoLaneProcessor::Pump() noexcept {
    return {&PumpSentMessagesOnly, nullptr};
}

} // namespace rocks::ui
