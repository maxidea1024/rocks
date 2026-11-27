#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 감시 재컴파일(Docs/Design/ShaderSystem 16.3절 SH4, SSD78)을 실제 장치로 확인한다. 시험 루트
// /Plugin/SandboxReload의 include 파일 ReloadColor.rsh를 장치마다 다른 색으로 두 번 고쳐 쓰고(임시
// 파일 → Replace — 반쯤 쓴 원문을 감시가 보지 않게), 공급자 재컴파일 통지를 gate로 기다린 뒤
// 프레임 경계 교체(EngineRenderResources::ApplyShaderUpdates)로 전체화면 픽셀이 새 색이 되는지
// 본다. 이어 문법 오류를 쓰면 통지가 실패와 진단 위치(그 파일·줄)를 싣고 교체가 없어 픽셀이
// 그대로인지, 고치면(같은 byte) revision이 그대로인지 본다. 저장 → 교체 시간은 재서 찍는다(단언하지
// 않는다 — 완료 기준 10초는 측정값으로 본다). 공급자(Development)가 없으면 NOT RUN이다. index는
// 장치 순번(색을 장치마다 다르게 고른다)이다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunShaderReloadChecks(const char* name, rocks::graphics::Device& device,
                                    rocks::Allocator& allocator, rocks::uint32 index) noexcept;

} // namespace sandbox
