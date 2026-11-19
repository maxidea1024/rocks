#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// MSAA 텍스처와 attachment resolve(RenderBase 4.4절 텍스처 GX2 B부, 결정 R17)를 실제 장치로
// 확인한다.
// - 4x MSAA RGBA8 렌더 타깃(4×1)에 x = 1.5에서 끝나는 흰 사각형을 그리고 패스 끝 resolve
//   (ColorAttachment::resolveTarget, store DontCare)로 1표본 텍스처에 낸다. 덮인 픽셀은 흰색,
//   빈 픽셀은 검정, 가장자리 픽셀은 그 사이(표본 박스 평균)다. 표본 위치는 장치마다 달라 정확한
//   비율 대신 사이 값을 단언한다.
// - PS가 MSAA 텍스처를 표본 번호로 읽은 값(HLSL Texture2DMS, GLSL ES sampler2DMS).
// - 규칙 위반 거부: 표본 수 3, MSAA Sampled 텍스처, 크기가 다른 resolve 대상, 1표본 attachment의
//   resolve, 표본 수가 다른 attachment, MSAA ReadPixels.
// 장치가 4표본을 지원하지 않으면 생성이 Unsupported인지 단언한다. device는 호출 스레드가
// 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunMsaaChecks(const char* name, rocks::graphics::Device& device,
                            rocks::Allocator& allocator) noexcept;

} // namespace sandbox
