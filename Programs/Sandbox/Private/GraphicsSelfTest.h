#pragma once

#include <Core/Memory/Allocator.h>

namespace sandbox {

// 창 없이 그래픽 backend를 검사한다. D3D12(하드웨어·WARP)와 OpenGL에서 offscreen 렌더 타깃에 내장
// 삼각형·텍스처 사각형을 그리고 readback 픽셀(꼭짓점 색·UV 방향), 두 backend의 결과 일치, 검증 계층
// 오류 0을 확인한다. 사용할 수 없는 backend는 NOT RUN으로 보고한다. 실패가 하나라도 있으면 1을
// 반환한다.
int RunGraphicsSelfTest(rocks::Allocator& allocator) noexcept;

} // namespace sandbox
