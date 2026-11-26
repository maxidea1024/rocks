#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 엔진 셰이더 SL0 GPU probe 검사다(Docs/Design/EngineShaders 7.6·7.7·8장, 결정 SLD19). 활성 셰이더
// 집합의 probe 셰이더(/Plugin/EngineShaderTests)를 전체 화면 삼각형(엔진
// FullscreenTriangleVertex)으로 RGBA32Float·R32Uint 타깃에 그려 ReadTexture로 읽고 CPU double 기준
// 구현과 대조한다. 결과를 장치 사이 대조용으로 기록한다. device는 호출 스레드가 소유하고
// DeviceShaders가 열려 있어야 한다. 실패 수를 돌려준다.
rocks::uint32 RunEngineShaderChecks(const char* name, rocks::graphics::Device& device,
                                    rocks::Allocator& allocator) noexcept;

// 장치 사이 대조 기록을 비운다(self-test 시작 때).
void ResetEngineShaderRecords() noexcept;

// 모든 장치 뒤 장치 사이 대조를 찍는다(정수 probe는 bit 단위로 같아야 한다 — 실패 수, 실수 probe는
// 장치 사이 최대 차이를 기록한다 — 8.5절). coverage 사유 목록(7.7절 5항)도 찍는다.
rocks::uint32 ReportEngineShaderCrossDevice() noexcept;

} // namespace sandbox
