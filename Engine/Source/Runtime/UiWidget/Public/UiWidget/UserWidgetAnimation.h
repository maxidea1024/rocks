#pragma once

#include <UiFramework/Application/Application.h>
#include <UiWidget/WidgetAnimation.h>
#include <UiWidget/WidgetTransform.h>

namespace rocks::ui {

class UserWidget;
class UserWidgetAnimationSet;
enum class WidgetAnimationMode : uint8 {
    Forward,
    Reverse,
    PingPong
};
enum class WidgetAnimationEnd : uint8 {
    Finished,
    Stopped,
    Failed
};
enum class WidgetAnimationStatus : uint8 {
    Queued,
    Playing,
    Paused,
    Ended
};

struct WidgetAnimationPlayback {
    WidgetAnimationMode mode = WidgetAnimationMode::Forward;
    // 시작 방향의 끝점으로부터 잰 초다. 0회는 무한 반복이고 왕복에서는 한 왕복이 한 회다.
    float startAt = 0;
    uint32 loops = 1;
    float speed = 1;
    bool restoreState = false;
    Delegate<void()> onStarted{};
    Delegate<void(WidgetAnimationEnd)> onEnded{};
};

// 재생 한 번의 제어 핸들이다. 호출자가 핸들을 놓아도 UserWidget이 종료까지 소유한다.
// 재생 중인 프레임에서만 곡선을 평가하며 일시 정지 또는 종료하면 프레임 예약을 해제한다.
class ROCKS_UIWIDGET_API UserWidgetAnimation final {
public:
    explicit UserWidgetAnimation(Allocator& allocator) noexcept;
    WidgetAnimationStatus Status() const noexcept;
    float CurrentTime() const noexcept;
    bool IsReversed() const noexcept;
    Result<void> Pause() noexcept;
    Result<void> Resume() noexcept;
    void Stop() noexcept;
    Result<void> Reverse() noexcept;
    Result<void> Seek(float seconds) noexcept;
    Result<void> SetSpeed(float speed) noexcept;
    // 고정 시간 시뮬레이션도 같은 시간 경계·보간·종료 경로를 사용할 수 있다.
    Result<void> Advance(double deltaSeconds) noexcept;

private:
    friend class UserWidget;
    struct Target {
        ObjectHandle handle{};
        WidgetTransform original{}, next{};
        float originalOpacity = 1, nextOpacity = 1;
        uint8 mask = 0;
    };
    struct Channel {
        explicit Channel(Allocator& allocator) noexcept : curve(allocator) {
        }

        usize target = 0;
        WidgetAnimationChannel property = WidgetAnimationChannel::Opacity;
        TangentCurve curve;
    };
    Result<void> Bind(ObjectRegistry& objects, Application& app, UserWidget& owner,
                      const WidgetAnimation& clip, WidgetAnimationPlayback&& options,
                      const SharedPtr<UserWidgetAnimation>& self) noexcept;
    Result<void> Schedule() noexcept;
    Result<void> Apply(bool restore) noexcept;
    Result<void> Sample(bool restore) noexcept;
    void Begin() noexcept;
    void End(WidgetAnimationEnd reason) noexcept;
    void OnFrame(double delta) noexcept;

    Array<Target> targets_;
    Array<Channel> channels_;
    ObjectRegistry* objects_ = nullptr;
    WeakPtr<Application*> app_;
    WeakPtr<Element> root_;
    WeakPtr<UserWidgetAnimation> self_;
    WeakPtr<UserWidgetAnimationSet> set_;
    SharedPtr<UiTimer> timer_;
    WidgetAnimationPlayback options_;
    WidgetAnimationStatus status_ = WidgetAnimationStatus::Queued;
    double time_ = 0;
    double duration_ = 0;
    uint64 remainingLegs_ = 0;
    bool reversed_ = false;
    bool started_ = false;
    bool applying_ = false;
    bool applied_ = false;
    bool stopRequested_ = false;
};

} // namespace rocks::ui
