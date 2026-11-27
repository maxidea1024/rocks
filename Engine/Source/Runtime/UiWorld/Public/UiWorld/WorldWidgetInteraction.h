#pragma once

#include <UiWorld/WorldWidgetComponent.h>
#include <World/World.h>

namespace rocks::ui {

// 로컬 플레이어의 컨트롤러·레이 포인터 하나다. 공간 검색 결과만 넘기면 월드 위젯의 평면,
// 피벗과 실제 히트를 검사한다. 누른 위젯은 레이가 다른 위젯 위로 가더라도 해제까지 유지한다.
class ROCKS_UIWORLD_API WorldWidgetInteraction {
public:
    WorldWidgetInteraction(uint32 userId, uint32 pointerId) noexcept;
    Result<bool> Route(World& world, Span<const ObjectHandle> candidates, Vec3d origin,
                       Vec3d direction, PointerEventType type,
                       input::MouseButton button = input::MouseButton::Left,
                       float wheel = 0) noexcept;
    Result<bool> RouteKey(World& world, const input::InputEvent& event) noexcept;
    void Reset(World& world) noexcept;

private:
    uint32 userId_;
    uint32 pointerId_;
    ObjectHandle pressed_{};
    ObjectHandle focus_{};
    ObjectHandle hovered_{};
};

} // namespace rocks::ui
