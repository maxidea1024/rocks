#pragma once

#include <SplashScreen/SplashTypes.h>

// 스플래시 화면에 보일 값이다(Docs/Design/SplashScreen 7.2·7.3절): 칸 글자·표시 여부, 진행 문구
// 서식, 막대 보간과 불확정 표시. OS를 모르고 시각은 호출자가 넘긴다(주입 시계 — 결정적 시험). 한
// 스레드(스플래시 스레드)가 쓴다.
namespace rocks::splash {

// 진행 원천을 당기는 간격이다.
inline constexpr uint64 SplashPollIntervalNanoseconds = 100'000'000;

// 막대 보간·불확정 표시 중 깨어나는 간격(약 60 Hz)이다.
inline constexpr uint64 SplashAnimationIntervalNanoseconds = 16'666'667;

// 막대 보간 시간 상수(초)다: B ← B + (D − B)·(1 − e^(−Δt/τ)).
inline constexpr double SplashBarEaseSeconds = 0.15;

// 보간을 멈추고 목표에 맞추는 차이(막대 1 px 미만)다.
inline constexpr double SplashBarSettleEpsilon = 0.001;

// 불확정 막대의 조각이 한 번 지나가는 시간이다.
inline constexpr uint64 SplashMarqueePeriodNanoseconds = 1'200'000'000;

// 시작 작업이 끝나기 전에 보이는 최대 백분율이다(다 된 것처럼 보이고 멈추는 일을 막는다).
inline constexpr uint32 SplashMaxUnfinishedPercent = 99;

// 막대 표시 방식이다.
enum class SplashBarMode : uint8 {
    Hidden,
    Indeterminate,
    Determinate,
};

// 사용 예(스플래시 스레드 루프):
//   SplashModel model(now);
//   model.SetText(SplashTextSlot::Title, "Rocks Sandbox");
//   ...
//   if (source.poll(source.context, status)) { model.ApplyStatus(status); }
//   model.Advance(now);
//   if (model.ConsumeDirty()) { presenter.Present(model); }
//   presenter.Wait(model.NextWake(now, nextPoll) - now);
class ROCKS_SPLASHSCREEN_API SplashModel {
public:
    explicit SplashModel(uint64 nowNanoseconds) noexcept;

    // 칸을 보이거나 숨긴다(기본: 모두 보임).
    void SetVisible(SplashTextSlot slot, bool visible) noexcept;
    bool IsVisible(SplashTextSlot slot) const noexcept;

    // 막대를 보이거나 숨긴다(기본: 보임). 숨기면 BarMode가 Hidden이다.
    void SetBarVisible(bool visible) noexcept;

    // 칸 글자를 바꾼다(UTF-8, MaxSplashLineBytes를 넘으면 문자 경계에서 자른다). Progress 칸에 쓰면
    // 진행 원천의 문구를 덮는다(다음 ApplyStatus까지 — 원천이 보고하면 진행 칸은 다시 원천의
    // 것이다).
    void SetText(SplashTextSlot slot, StringView utf8) noexcept;
    StringView Text(SplashTextSlot slot) const noexcept;

    // 진행 원천의 상태를 반영한다: 목표 비율, 끝남, 문구. 진행 칸은 "<percent>%  <문구>"(불확정이면
    // 문구만)다.
    void ApplyStatus(const SplashStatus& status) noexcept;

    // 시각을 now로 옮긴다(막대 보간, 불확정 조각 위치). 보이는 것이 바뀌면 dirty가 선다.
    void Advance(uint64 nowNanoseconds) noexcept;

    // 다음에 깨어날 시각이다: 보간·불확정 표시 중이면 now + 애니메이션 간격과 nextPoll 중 이른 쪽,
    // 아니면 nextPoll.
    uint64 NextWake(uint64 nowNanoseconds, uint64 nextPollNanoseconds) const noexcept;

    SplashBarMode BarMode() const noexcept;
    // 보이는 막대 값 B(0..1)다(Determinate).
    double BarValue() const noexcept;
    // 목표 값 D(0..1)다.
    double BarTarget() const noexcept;
    // 불확정 조각의 위치(0..1, 한 바퀴의 진행)다.
    double MarqueePhase() const noexcept;

    // 마지막 ConsumeDirty 뒤 보이는 것이 바뀌었으면 true이고 표시를 지운다.
    bool ConsumeDirty() noexcept;

private:
    struct Line {
        char bytes[MaxSplashLineBytes];
        uint32 count;
    };

    void StoreLine(Line& line, StringView utf8) noexcept;
    void RefreshProgressText() noexcept;

    Line text_[SplashTextSlotCount]{};
    bool visible_[SplashTextSlotCount]{true, true, true, true};
    Line statusMessage_{};
    bool barVisible_ = true;
    bool determinate_ = false;
    bool finished_ = false;
    bool statusOwnsProgress_ = false;
    double target_ = 0.0;
    double value_ = 0.0;
    double marqueePhase_ = 0.0;
    uint64 now_ = 0;
    uint64 marqueeStart_ = 0;
    bool dirty_ = true;
};

} // namespace rocks::splash
