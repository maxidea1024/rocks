#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 텍스처 view 범위·쓰기 텍스처·compute 텍스처 묶기(RenderBase 4.4절 텍스처 GX2 A부)를 실제 장치로
// 확인한다.
// - HZB 패턴: Storage R32Float 8×8(mip 4)의 mip 0을 compute가 쓰기 view로 채우고, mip k 읽기 view와
//   mip k + 1 쓰기 view를 한 dispatch에 묶어 2×2 최댓값으로 줄인다. 모든 mip을 ReadTexture로 읽어
//   CPU 기댓값과 대조한다. 쓰기 텍스처를 묶지 않은 dispatch는 무시된다.
// - PS가 mip 범위 읽기 view를 view 기준 mip(TextureViewBaseMip, 그리기별 상수)으로 읽은 픽셀.
// - 2D 배열 slice 범위 view: 범위 view를 지원하면 view의 첫 층을 읽고, 아니면(OpenGL ES)
//   Unsupported다.
// - 규칙 위반 거부(Sampled 텍스처의 쓰기 view, mip 둘인 쓰기 view, 범위 밖).
// 장치가 형식 능력(Sampled·StorageWrite·Mipmaps)이 없으면 Storage 생성이 Unsupported인지 단언한다.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunTextureViewChecks(const char* name, rocks::graphics::Device& device,
                                   rocks::Allocator& allocator) noexcept;

} // namespace sandbox
