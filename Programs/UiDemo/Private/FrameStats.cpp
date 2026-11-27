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
    last_ = now;
}

void FrameStats::NoteCandidates(usize count) noexcept {
    candidates_ = count > candidates_ ? count : candidates_;
}

void FrameStats::EndFrame(StringView window) noexcept {
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
    Log::Info(LogUiDemo,
              "Frame stats {Window} ({Frames} frames, ms/frame): input {Input}, layout {Layout}, "
              "record {Record}, paint cache {PaintCache}, regions {Regions}, batch {Batch}, "
              "submit {Submit}; hit candidates {Candidates}",
              window, frames_, average(FrameStage::Input), average(FrameStage::Layout),
              average(FrameStage::Record), average(FrameStage::PaintCache),
              average(FrameStage::Regions), average(FrameStage::Batch), average(FrameStage::Submit),
              candidates_);
    for (double& stage : stages_) {
        stage = 0;
    }

    frames_ = 0;
    candidates_ = 0;
}

} // namespace rocks::ui
