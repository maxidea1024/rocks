#pragma once

#include <Core/Math/Rect.h>
#include <Core/Memory/SharedPtr.h>
#include <UiCore/FrameTime.h>
#include <UiCore/TextInputService.h>

namespace rocks::ui {

// 창 호스트가 UI에 주는 서비스 묶음이다(Docs/Design/UiToolkit 20.2절). Application마다 하나이고
// Application이 루트(툴팁 임시 호스트 포함)에 단다. Element는 Element::FindUiHostServices로
// 루트까지 올라가 찾는다. 트리에 붙기 전에는 못 찾는다(시각 0, 서비스 없음).
//
// - 팝업 허용 영역(9.1절 UW7d): 창 좌표 지점이 놓인 모니터의 작업 영역을 창 좌표로 준다.
// - 텍스트 입력 서비스(19.2절): 편집 요소가 초점 때 Activate한다.
// - 프레임 시각(20.3절): Application::BeginFrame이 정한다. 그리기 밖(속성 갱신·배치·입력)에서도
//   같은 값을 읽는다.
//
// 스레드: 게임 lane에서만 쓴다.
class ROCKS_UICORE_API UiHostServices {
public:
    using PopupWorkArea = Delegate<bool(Vec2 window, Box2<float>& area)>;

    void SetPopupWorkArea(PopupWorkArea&& provider) noexcept;
    // 공급자가 없거나 모르면 false다.
    bool QueryPopupWorkArea(Vec2 window, Box2<float>& area) const noexcept;

    void SetTextInputService(SharedPtr<TextInputService> service) noexcept;
    const SharedPtr<TextInputService>& TextInput() const noexcept;

    void SetFrameTime(const FrameTime& time) noexcept;
    const FrameTime& Time() const noexcept;

private:
    PopupWorkArea popupWorkArea_;
    SharedPtr<TextInputService> textInput_;
    FrameTime time_{};
};

} // namespace rocks::ui
