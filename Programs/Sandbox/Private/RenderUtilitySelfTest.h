#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// RenderBase 그리기 유틸리티(Docs/Design/RenderBase 11.4절, RB6)를 실제 장치로 확인한다: 엔진
// 셰이더 FullscreenTriangleVertex·CopyTexturePixel로 그린 전체화면 복사의 픽셀(장치 직접·렌더
// 그래프 AddFullscreenPass 두 경로), RenderBase 경로 그리기 전후의 NullTextureBindings가 같음과
// null 묶기의 계수, 내장 자원 팩(11.4절 5–7항)의 텍스처·버퍼 값, 블렌드 프리셋 픽셀, 기본 메시를
// fallback 셰이더로 그린 컬·깊이 프리셋 픽셀, 검증 계층 오류 0. device는 호출 스레드가 소유한다.
// 실패 수를 돌려준다.
rocks::uint32 RunRenderUtilityChecks(const char* name, rocks::graphics::Device& device,
                                     rocks::Allocator& allocator) noexcept;

} // namespace sandbox
