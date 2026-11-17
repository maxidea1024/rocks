#pragma once

#include "SampleRenderer.h"

namespace sandbox {

// GPU 계측(RenderBase 13.2절 GX9)을 실제 장치로 확인한다: 그리기를 감싼 timestamp 한 쌍이 GPU 완료
// 뒤 회수되어 순서·크기가 맞는지, 여러 프레임에 걸친 프레임 slot 재사용, calibration pair로 옮긴
// GPU 시각이 앞뒤 CPU 시각 사이에 놓이는지, debug marker의 짝 맞춤(남은 구간 자동 닫기·짝 없는
// pop 무시), 기록 밖 쓰기 거부. timestamp를 지원하지 않는 장치면 "없음" 공개(Unsupported)를 본다.
// device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunTimingChecks(const char* name, rocks::graphics::Device& device,
                              sample::Renderer& renderer, rocks::Allocator& allocator) noexcept;

} // namespace sandbox
