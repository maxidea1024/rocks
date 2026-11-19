#pragma once

#include "SampleRenderer.h"

namespace sandbox {

// 렌더 그래프(RenderBase 8절 RG1 (g)·RG2 A부)를 실제 장치로 확인한다: 그래프 텍스처에 그린 장면을
// 다음 pass가 SRV로 읽어 import한 출력에 합성하고(장치 명시 전이 GX5), 쓰이지 않는 pass는 제거되며,
// export한 history를 다음 그래프가 import해 같은 결과를 내고 Retire하면 풀로 돌아온다(결정 R15).
// 같은 그래프 안에서 수명이 끝난 텍스처·버퍼를 뒤 자원이 이어 쓰고(이전 내용 무효 전이), 그래프 안
// 업로드 (버퍼 → 복사 pass, Sampled 텍스처 → 합성)의 결과가 맞는지 본다. 픽셀·바이트와 검증 계층
// 오류 0(명시 전이의 before 불일치 포함)을 본다. RG1 남은 것 B부: HZB 축소를 그래프 texture
// view(mip k 읽기 + mip k + 1 쓰기 pass)로 내고 모든 mip을 CPU 기댓값과 대조한다(OpenGL ES는 view
// 기준 mip 경로). device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunRenderGraphChecks(const char* name, rocks::graphics::Device& device,
                                   sample::Renderer& renderer,
                                   rocks::Allocator& allocator) noexcept;

} // namespace sandbox
