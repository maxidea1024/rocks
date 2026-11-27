#pragma once

#include <Base/Types.h>

namespace rocks::ui {

// 활성 화면의 입력 설정을 게임 쪽에 적용하는 호스트다(UE CommonUI ApplyUIInputConfig가 부르는
// PlayerController·GameViewportClient — Docs/Design/UiToolkit 21.3절). World·Gameplay가 구현하고,
// UiScreens는 Gameplay에 의존하지 않는다. ScreenInputRouter가 설정이 바뀔 때만 부른다.
//
// 사용 예:
//   class PlayerScreenInput final : public ScreenInputHost { ... };
//   router.SetInputHost(&playerInput);
class ROCKS_UISCREENS_API ScreenInputHost {
public:
    virtual ~ScreenInputHost() noexcept = default;
    // 이동·시선 입력 무시다(UE SetIgnoreMoveInput·SetIgnoreLookInput). 앞서 true를 부른 적이 없으면
    // false는 오지 않는다.
    virtual void SetIgnoreMoveInput(bool ignore) noexcept = 0;
    virtual void SetIgnoreLookInput(bool ignore) noexcept = 0;
    // 마우스 붙잡기(창 안에 가둠)와 커서 보이기다. 붙잡지 않으면 커서는 늘 보인다(UE
    // EMouseCaptureMode NoCapture → SetShowMouseCursor(true)).
    virtual void SetMouseCapture(bool captured, bool cursorVisible) noexcept = 0;
    // 눌린 채 남은 게임 입력을 푼다(UE FlushPressedKeys — Menu로 바뀔 때 한 번).
    virtual void FlushPressedKeys() noexcept = 0;
};

} // namespace rocks::ui
