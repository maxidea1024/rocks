#pragma once

#include "SampleRenderer.h"

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 좌표·앞면 규약(ShaderSystem 7.2절 6·8항, 좌표계 UE 전환 C7)을 실제 장치로 확인한다.
// - 컬 모드: 화면 반시계 삼각형이 앞면이다. None은 둘 다, Back은 반시계만, Front는 시계만 보인다.
// - 음수 행렬식: 샘플 렌더러가 음수 scale 항목·거울 view의 컬을 뒤집어 보이게 하고, 둘 다면
//   원래대로다.
// - 행 우선 행렬: 이동·비균등 scale을 담은 비대칭 행렬이 기대한 화면 영역에 그려진다.
// 모든 장면을 창(스왑체인, CopyPassTarget으로 읽음)과 offscreen 두 경로로 그려 같은 결과인지 본다.
// renderer는 RGBA8UnormSrgb로 초기화된 것이다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunCullingChecks(const char* name, rocks::graphics::Device& device,
                               sample::Renderer& renderer, rocks::Allocator& allocator) noexcept;

} // namespace sandbox
