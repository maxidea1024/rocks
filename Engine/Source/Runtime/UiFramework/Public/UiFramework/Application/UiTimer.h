#pragma once
#include <Core/Delegate.h>
#include <UiCore/Element.h>

namespace rocks::ui {

// 기다릴 타이머가 없다는 표지다(Application::SecondsUntilNextTimer).
inline constexpr double NoTimerDeadline = 1.0e300;

// Application이 발행하는 반복 타이머의 수명 토큰이다. 생성은 Application::StartTimer를 사용한다.
class ROCKS_UIFRAMEWORK_API UiTimer final {
public:
    UiTimer(const SharedPtr<Element>& owner, double interval, Delegate<void()>&& callback,
            double start) noexcept;

    UiTimer(const SharedPtr<Element>& owner, Delegate<void(double)>&& frame, double start) noexcept;

private:
    friend class Application;
    void Advance(double now, const Element& root) noexcept;
    WeakPtr<Element> owner_;
    double interval_;
    double due_;
    Delegate<void()> callback_;
    Delegate<void(double)> frame_;
};

} // namespace rocks::ui
