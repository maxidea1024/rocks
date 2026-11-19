#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 용도 비트 묶음과 cube Storage(RenderBase 4.4절 텍스처 GX2 나머지 A부)를 실제 장치로 확인한다.
// - RenderTarget | Storage(R32Float 4×4): 0.25로 clear한 뒤 compute가 쓰기 view로 x < 2를 0.5로
//   덮고, 같은 텍스처를 load attachment로 다시 열어 x ≥ 3에 0.75를 그린다. ReadTexture가 열마다
//   0.5·0.5·0.25·0.75다.
// - cube Storage(R32Float 4×4): 모든 면을 layered 쓰기 view로 0으로 채운 뒤 면 2 하나만 고른 쓰기
//   view로 2를 쓴다. cube 배열(cube 2개)은 layered 쓰기 view로 면 6만 6으로 쓴다. ReadTexture로
//   그 면만 바뀐 것을 본다.
// 형식 능력·compute가 없으면 생성이 Unsupported인지 단언한다. device는 호출 스레드가 소유한다. 실패
// 수를 돌려준다.
rocks::uint32 RunStorageTargetChecks(const char* name, rocks::graphics::Device& device,
                                     rocks::Allocator& allocator) noexcept;

} // namespace sandbox
