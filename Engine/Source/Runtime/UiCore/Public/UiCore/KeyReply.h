#pragma once

#include <Core/Optional.h>
#include <UiCore/NavigationRule.h>

namespace rocks::ui {

// 키 이벤트 응답이다(UE FReply의 Handled·SetNavigation — Docs/Design/UiToolkit 20.7절). navigation
// 응답이 나오면 라우팅을 멈추고(조상은 키를 받지 않는다) 그 사용자의 navigation을 실행한다.
//
// 사용 예(키를 쓰는 override):
//   if (key.input.key == input::Key::Enter) { Commit(); return KeyReply::Handled(); }
//   return Super::OnKeyEvent(key);   // 처리하지 않으면 기본 처리(초점 가능한 요소는 navigation)
class KeyReply {
public:
    static KeyReply Unhandled() noexcept {
        return KeyReply{};
    }

    static KeyReply Handled() noexcept {
        KeyReply reply;
        reply.handled_ = true;
        return reply;
    }

    static KeyReply FromHandled(bool handled) noexcept {
        return handled ? Handled() : Unhandled();
    }

    static KeyReply Navigate(FocusDirection direction) noexcept {
        KeyReply reply;
        reply.handled_ = true;
        reply.navigation_ = direction;
        return reply;
    }

    // 처리했는지다(navigation 응답도 처리한 것이다).
    bool IsHandled() const noexcept {
        return handled_;
    }

    const Optional<FocusDirection>& Navigation() const noexcept {
        return navigation_;
    }

private:
    bool handled_ = false;
    Optional<FocusDirection> navigation_{};
};

} // namespace rocks::ui
