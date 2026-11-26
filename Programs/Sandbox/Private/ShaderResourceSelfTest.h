#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 생성 셰이더 파라미터 구조체의 자원 멤버(Docs/Design/ShaderSystem 16.2절 SSD66 4)를 실제 장치로
// 확인한다: SandboxShaderParameters.h GeneratedResourceParameters(상수 + 장치 텍스처 둘 + 장치
// 버퍼)의 생성 표로 자원 지도를 만들고 ParameterBatch::SetParameters로 묶어 그린 픽셀이 기대값과
// 같은지, 짝 샘플러 override가 텍스처 desc 샘플러를 바꾸는지, 값 1 자원 항목의 구조체 identity·멤버
// 오프셋이 표와 같은지 본다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunShaderResourceChecks(const char* name, rocks::graphics::Device& device,
                                      rocks::Allocator& allocator) noexcept;

} // namespace sandbox
