#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 상수 버퍼(RenderBase 9.11절 GX6)를 실제 장치로 확인한다: SingleFrame·MultiFrame handle과
// SetConstantData로 그린 픽셀, 기록 중 갱신의 기록 순서(앞 그리기는 옛 내용), 파이프라인 전환 뒤
// 묶음 유지, 안 묶은·선언보다 작은 버퍼의 그리기 무시, 정수 상수 버퍼를 읽는 compute 결과, 단계마다
// 상수 버퍼 12개(D3D12는 14개 — root 예산을 넘겨 CBV table로 내리는 경로 — 와 PS 진단 버퍼
// 퍼뮤테이션) 그리기의 slot별 픽셀, 검증 계층 오류 0. device는 호출 스레드가 소유한다. 실패 수를
// 돌려준다.
rocks::uint32 RunConstantChecks(const char* name, rocks::graphics::Device& device,
                                rocks::Allocator& allocator) noexcept;

} // namespace sandbox
