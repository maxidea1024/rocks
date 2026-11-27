#pragma once

#include <Core/Text/StringView.h>

namespace rocks::ui {

// 창 프레임의 단계다(Docs/Design/UiToolkit 19.5절 — --frame-stats).
enum class FrameStage : uint8 {
    Input,
    Layout,
    Record,
    PaintCache,
    Regions,
    Batch,
    Submit,
    Count
};

// 단계별 시간을 모아 ReportInterval 프레임마다 평균을 로그로 남긴다(진단용 — 시험은 시간에 기대지
// 않는다). 꺼져 있으면 시계를 읽지 않는다.
//
// 사용 예:
//   stats.Begin();
//   RouteInput();
//   stats.Mark(FrameStage::Input);
//   ...
//   stats.EndFrame(windowIndex);
class FrameStats {
public:
    // 평균을 내는 프레임 수다.
    static constexpr uint32 ReportInterval = 60;

    void SetEnabled(bool enabled) noexcept;
    bool IsEnabled() const noexcept;
    // 프레임 측정을 시작한다(다음 Mark의 기준 시각).
    void Begin() noexcept;
    // 직전 표시부터 지금까지를 stage에 더한다.
    void Mark(FrameStage stage) noexcept;
    // hit 후보 수(영역 지도 입력 크기)를 기록한다(구간 최대).
    void NoteCandidates(usize count) noexcept;
    // 프레임 하나를 끝낸다. 구간이 차면 로그를 남기고 비운다. 창 프레임(입력~batch 단계)은 "Window
    // <index>"로, 제출만 잰 기록(모든 창을 한 번에 — Submit 단계)은 "Submit"으로 남는다. 재지 않은
    // 단계는 적지 않는다.
    void EndFrame(uint32 index) noexcept;

private:
    double stages_[static_cast<usize>(FrameStage::Count)]{};
    double last_ = 0;
    uint32 frames_ = 0;
    usize candidates_ = 0;
    // 이번 구간에 Mark한 단계의 비트다(제출 기록과 창 기록의 로그 형식을 가른다).
    uint32 marked_ = 0;
    bool enabled_ = false;
    bool running_ = false;
};

} // namespace rocks::ui
