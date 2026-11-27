#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// 엔진 셰이더 SL1 유틸리티 pass(Docs/Design/EngineShaders 14.4·14.5절·14.6절 3항)를 실제 장치로
// 확인한다: 밉 생성(형식 × 차원 × 장치가 받는 경로, mip k + 1 = 그 장치가 쓴 mip k의 CPU 면적
// 박스), UAV clear(형식 × 차원, 범위 안 값·밖 보존)와 typed 버퍼 clear, 부분 clear(색
// float·uint·깊이·스텐실), 간접 인자 초기화, 기본 텍스처 캐시(형식 × 차원 값, 같은 키 같은 handle),
// 깊이 복사(색·깊이 대상), 자리표시(단색·바둑판). 활성 엔진 셰이더 집합(shaders::ActiveSet)을 쓴다.
// 실패 수를 돌려준다.
rocks::uint32 RunUtilityPassChecks(const char* name, rocks::graphics::Device& device,
                                   rocks::Allocator& allocator) noexcept;

} // namespace sandbox
