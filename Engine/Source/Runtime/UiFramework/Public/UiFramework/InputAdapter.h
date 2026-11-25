#pragma once

#include <Input/InputEvent.h>
#include <UiFramework/Application.h>

namespace rocks::ui {

// 기존 Input의 단일 창 마우스·터치·키 이벤트를 UI에 전달한다.
// 입력 큐와 같은 Game lane에서 호출한다. 위치가 없는 마우스 버튼은 첫 위치 수신 전까지 전달하지
// 않는다.
class ROCKS_UIFRAMEWORK_API InputAdapter {
public:
    explicit InputAdapter(Application& application) noexcept;
    input::MouseCursor MouseCursor() const noexcept;

    // UI가 이벤트를 소비했으면 true다. 포커스 상실 시 capture와 마지막 위치를 지운다.
    Result<bool> Route(const input::InputEvent& event) noexcept;
    Result<bool> RouteWithin(const input::InputEvent& event,
                             const SharedPtr<Element>& scope) noexcept;

    // 큐 순서대로 처리하고 이벤트마다 소비 여부를 기록한다. droppedCount가 이전 값과 다르면
    // 포인터·focus 상태를 해제하고 CapacityExceeded를 반환한다. 호출자는 해당 배치를 버리고
    // gameplay 입력 상태도 재동기화해야 한다. 중간 실패 시 앞서 처리한 결과만 유효하다.
    Result<void> RouteBatch(Span<const input::InputEvent> events, Span<uint8> consumed,
                            uint64 droppedCount) noexcept;

private:
    Application& application_;
    Vec2 position_{};
    uint32 mouseUserId_ = 0;
    bool hasPosition_ = false;
    uint32 gamepadButtons_[input::MaxGamepads]{};
    uint64 droppedCount_ = 0;
};

} // namespace rocks::ui
