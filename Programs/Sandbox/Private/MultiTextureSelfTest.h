#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 다중 텍스처·샘플러 slot과 자원 묶음(RenderBase 9.13절 RR12)을 실제 장치로 확인한다: 자원 지도·
// ParameterBatch로 VS 텍스처 하나와 PS 텍스처 넷(2D·2D 배열·공유 샘플러 2D·정수 2D)을 한 pass에
// 묶어 그린 픽셀, 자기 샘플러 값 지정(Wrap → Clamp)과 파이프라인 전환 뒤 묶음 유지, 공유 샘플러를
// 비운 그리기의 무시 계수, null 대체 계수 0, 검증 계층 오류 0. device는 호출 스레드가 소유한다.
// 실패 수를 돌려준다.
rocks::uint32 RunMultiTextureChecks(const char* name, rocks::graphics::Device& device,
                                    rocks::Allocator& allocator) noexcept;

} // namespace sandbox
