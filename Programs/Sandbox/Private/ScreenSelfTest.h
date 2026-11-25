#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

namespace sandbox {

// 엔진 창 렌더 경로를 실제 장치로 확인한다(Docs/Design/RenderBase 18.5절 5항 — SM5). 숨긴 창과
// Sandbox 게임(GameInstance·SandboxPawn)으로 엔진을 몇 프레임 돌리고 ScreenHost가 렌더 lane으로
// 그린다. 단언: 화면의 HDR cube·과노출 cube·배경·Pending 텍스처 cube 픽셀 = CPU 기준 tone map(채널
// ±2), Thread·Inline 모드 픽셀 동일, 검증 오류 0, 남은 lane 작업·handle·장치 회수 0. device는 호출
// 스레드가 소유한다. 실패 수를 돌려준다(진단은 stdout).
rocks::uint32 RunScreenChecks(const char* name, rocks::graphics::Device& device,
                              rocks::Allocator& allocator, rocks::ModuleLoader& modules) noexcept;

// 카메라 입력 경로를 헤드리스 엔진으로 확인한다(18.5절 5항 (e)): W를 누른 채 돌린 두 프레임 사이
// 카메라 이동 = 1200 cm/s × 프레임 수 × 고정 delta, 오른쪽 화살표 회전율 45 °/s. 실패 수를
// 돌려준다.
rocks::uint32 RunPawnInputChecks(rocks::Allocator& allocator,
                                 rocks::ModuleLoader& modules) noexcept;

} // namespace sandbox
