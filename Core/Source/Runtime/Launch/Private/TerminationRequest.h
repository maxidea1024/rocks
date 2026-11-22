#pragma once

// Launch 종료 요청 표시의 시험용 되돌림이다(Application.cpp). 제품 코드는 부르지 않는다. 표시는 한
// 번 켜지면 프로세스가 끝날 때까지 켜져 있는 것이 계약이므로, 같은 프로세스에서 다른 시험이 이어 돌
// 시험 실행기만 끝에서 되돌린다.
namespace rocks::launch::detail {

ROCKS_LAUNCH_API void ClearTerminationRequest() noexcept;

} // namespace rocks::launch::detail
