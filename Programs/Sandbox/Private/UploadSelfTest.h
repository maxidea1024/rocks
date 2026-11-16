#pragma once

#include "SampleRenderer.h"

namespace sandbox {

// 업로드·readback(RenderBase 4.4절 GX8)을 실제 장치로 확인한다: 텍스처·버퍼 내용 갱신, 업로드 ring
// wrap, 스트리밍 staging 업로드와 fence 뒤 회수·재사용, 비동기 readback(ReadPixels와 같은 결과),
// 다른 스레드의 자원 생성(D3D12 허용, OpenGL WrongThread). device는 호출 스레드가 소유한다. 실패
// 수를 돌려준다.
rocks::uint32 RunUploadChecks(const char* name, rocks::graphics::Device& device,
                              sample::Renderer& renderer, rocks::Allocator& allocator) noexcept;

} // namespace sandbox
