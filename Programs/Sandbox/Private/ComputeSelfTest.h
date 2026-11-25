#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 렌더 쪽 compute 경로(RenderBase 4.4절 "GX3·GX4 compute 연결 설계")를 실제 장치로 확인한다.
// - 파이프라인 캐시가 셰이더 집합의 CS 항목 셋으로 compute 파이프라인을 만들고, 두 번째 그래프는
//   모두 적중한다.
// - 그래프 compute pass 도우미(AddComputePass 캐시 판): 직접 dispatch가 그래프 구조화 버퍼에 수열을
//   쓰고, compute가 그래프 raw 버퍼에 간접 인자를 쓰고, 간접 dispatch가 수열을 버퍼 SRV view로 읽어
//   R32Uint Storage 텍스처에 무늬를 쓴다. 버퍼 byte·텍스처 pixel을 CPU 기댓값과 정확히 대조한다
//   (모든 backend가 같은 값이어야 한다, 결과 hash를 출력한다).
// - 같은 그래프를 Compute pass(graphics pipe)와 AsyncCompute pass(GraphAsyncCompute::Always)로 돌린
//   결과가 같다. 장치 한도 밖 그룹 수·Raster 종류는 도우미가 거부한다. 검증 계층 오류는 0이다.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunComputeChecks(const char* name, rocks::graphics::Device& device,
                               rocks::Allocator& allocator) noexcept;

} // namespace sandbox
