#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// slice·mip attachment(RenderBase 4.4절 텍스처 GX2 C부)를 실제 장치로 확인한다.
// - mip 사슬 렌더 타깃: mip 0을 빨강으로 clear하고, mip 0 읽기 view를 읽으며 mip 1에 절반 값을
//   그린다(같은 텍스처의 다른 mip을 SRV와 attachment로 함께 쓴다).
// - 2D 배열 층·cube 면·3D 깊이 면 attachment: slice마다 다른 색으로 clear·그리기하고 ReadTexture로
//   그 slice만 바뀌었는지 본다.
// - 깊이 배열 층: 층마다 다른 깊이로 clear한 뒤 깊이 시험(GreaterEqual)으로 고른 층의 값이 쓰였는지
//   그린 픽셀로 확인한다.
// - resolve 대상 slice: 4x MSAA를 2D 배열 층 1에 resolve한다(4표본을 지원할 때).
// - 규칙 위반 거부: mip·slice 범위 밖, mip 크기가 다른 attachment, 3D 깊이 생성.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunSliceAttachmentChecks(const char* name, rocks::graphics::Device& device,
                                       rocks::Allocator& allocator) noexcept;

} // namespace sandbox
