#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 렌더 lane(RenderBase RB1)을 실제 장치로 확인한다. 헤드리스 Engine 루프가 매 프레임 장면을 추출해
// 프레임 packet으로 넘기고 Renderer가 렌더 lane 안에서 그린다. 숨김 창 스왑체인
// 생성·resize·present는 표면 작업이고, 게임 lane은 slot 대기 중 창 메시지를 펌프한다. Engine 종료
// hook(CP6)이 렌더 lane 종료 순서(5.5절)를 실행한 뒤 장치 소유권이 돌아왔는지, 마지막 프레임 픽셀이
// 맞는지 본다. Thread·Inline 두 모드를 돌린다. device는 호출 스레드가 소유하고 있어야 한다. 실패
// 수를 돌려준다(진단은 stdout).
rocks::uint32 RunRenderLaneChecks(const char* name, rocks::graphics::Device& device,
                                  rocks::Allocator& allocator) noexcept;

} // namespace sandbox
