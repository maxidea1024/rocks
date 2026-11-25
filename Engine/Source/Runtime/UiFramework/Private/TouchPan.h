#pragma once
#include <UiCore/Geometry.h>
namespace rocks::ui {
// 두 창 좌표를 스크롤 영역의 논리 좌표로 변환한다. 주축 이동이 5픽셀을 넘고 그 방향에
// 실제 남은 콘텐츠가 있을 때만 탭 캡처를 스크롤에 넘긴다.
bool TryTouchPan(const Geometry& geometry, bool vertical, Vec2 origin, Vec2 position, double offset,
                 double maximum, float& start, float& current) noexcept;
} // namespace rocks::ui
