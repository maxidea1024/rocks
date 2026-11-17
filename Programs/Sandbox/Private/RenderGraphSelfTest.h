#pragma once

#include "SampleRenderer.h"

namespace sandbox {

// 렌더 그래프(RenderBase 8절 RG1 (g))를 실제 장치로 확인한다: 그래프 텍스처에 그린 장면을 다음
// pass가 SRV로 읽어 import한 출력에 합성하고(장치 명시 전이 GX5), 쓰이지 않는 pass는 제거되며,
// export한 history를 다음 그래프가 import해 같은 결과를 낸다. 픽셀과 검증 계층 오류 0(명시 전이의
// before 불일치 포함)을 본다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunRenderGraphChecks(const char* name, rocks::graphics::Device& device,
                                   sample::Renderer& renderer,
                                   rocks::Allocator& allocator) noexcept;

} // namespace sandbox
