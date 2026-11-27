#include "FrameStats.h"

#include "DemoLog.h"

#include <Core/Diagnostics/Log.h>
#include <Platform/Clock.h>

namespace rocks::ui {

namespace {

constexpr double MillisecondsPerNanosecond = 1.0e-6;

double NowMilliseconds() noexcept {
    return static_cast<double>(platform::MonotonicNanoseconds()) * MillisecondsPerNanosecond;
}

} // namespace

void FrameStats::SetEnabled(bool enabled) noexcept {
    enabled_ = enabled;
    running_ = false;
}

bool FrameStats::IsEnabled() const noexcept {
    return enabled_;
}

void FrameStats::Begin() noexcept {
    if (!enabled_) {
        return;
    }

    last_ = NowMilliseconds();
    running_ = true;
}

void FrameStats::Mark(FrameStage stage) noexcept {
    if (!running_ || stage >= FrameStage::Count) {
        return;
    }

    const double now = NowMilliseconds();
    stages_[static_cast<usize>(stage)] += now - last_;
    marked_ |= 1u << static_cast<uint32>(stage);
    last_ = now;
}

void FrameStats::NoteCandidates(usize count) noexcept {
    candidates_ = count > candidates_ ? count : candidates_;
}

void FrameStats::EndFrame(uint32 index) noexcept {
    if (!running_) {
        return;
    }

    running_ = false;
    if (++frames_ < ReportInterval) {
        return;
    }

    const double frames = static_cast<double>(frames_);
    const auto average = [&](FrameStage stage) noexcept {
        return stages_[static_cast<usize>(stage)] / frames;
    };
    if (marked_ == 1u << static_cast<uint32>(FrameStage::Submit)) {
        Log::Info(LogUiDemo, "Frame stats Submit ({Frames} frames, ms/frame): submit {Submit}",
                  frames_, average(FrameStage::Submit));
    } else {
        Log::Info(LogUiDemo,
                  "Frame stats Window {Index} ({Frames} frames, ms/frame): input {Input}, layout "
                  "{Layout}, record {Record}, paint cache {PaintCache}, regions {Regions}, batch "
                  "{Batch}; hit candidates {Candidates}",
                  index, frames_, average(FrameStage::Input), average(FrameStage::Layout),
                  average(FrameStage::Record), average(FrameStage::PaintCache),
                  average(FrameStage::Regions), average(FrameStage::Batch), candidates_);
    }

    for (double& stage : stages_) {
        stage = 0;
    }

    frames_ = 0;
    candidates_ = 0;
    marked_ = 0;
}

} // namespace rocks::ui
