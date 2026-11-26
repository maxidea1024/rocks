#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 생성 셰이더 파라미터 구조체(Docs/Design/ShaderSystem 16.2절 SSD58 3)를 실제 장치로 확인한다:
// SandboxShaderParameters.h의 cbuffer 구조체(행렬·3성분 뒤 스칼라·int 슬롯·배열·중첩)와 루트 상수
// 구조체를 C++ 값 그대로 SetConstantData·SetDrawConstants로 올려 그린 픽셀이 CPU로 계산한 값과
// 같은지, 값 1의 상수 버퍼 identity가 생성 표와 같은지 본다. device는 호출 스레드가 소유한다. 실패
// 수를 돌려준다.
rocks::uint32 RunShaderParameterChecks(const char* name, rocks::graphics::Device& device,
                                       rocks::Allocator& allocator) noexcept;

} // namespace sandbox
