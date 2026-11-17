#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 정점 배치(RenderBase 10.4절 GX3a)를 실제 장치로 확인한다. 정점 원소 형식 20종 각각으로 색을 넣어
// 그린 픽셀을 기대값과 대조하고(OpenGL의 Bgra8Unorm은 Unsupported 음성 대조), 다중 stream·인스턴스
// stream·indexed(UInt16·UInt32)·정점 버퍼 없는 그리기와 셰이더 입력 대조 음성 대조, 묶이지 않은
// stream 진단, 검증 계층 오류 0을 본다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunVertexChecks(const char* name, rocks::graphics::Device& device,
                              rocks::Allocator& allocator) noexcept;

} // namespace sandbox
