#pragma once

#include <Core/Memory/Allocator.h>
#include <Graphics/Device.h>

namespace sandbox {

// view 차원 바꾸기와 OpenGL ES 범위 view(RenderBase 4.4절 텍스처 GX2 나머지 B부)를 실제 장치로
// 확인한다. 1×1 RGBA8 Sampled 텍스처를 PS가 view로 읽은 픽셀을 본다.
// - 2D 배열(층 0 빨강·층 1 초록): 층 0을 Texture2D view로 → 빨강, 층 1을 1층 2D 배열 view로 → 초록.
// - cube(면마다 다른 색): 면 전체를 2D 배열 view로 읽은 층 4 → 면 4 색.
// - cube 배열 2개: 두 번째 cube를 1 cube짜리 cube 배열 view로 +X 방향 표본 → slice 6 색.
// - cube Storage: 면 0만 Texture2D 쓰기 view로 compute가 쓴다(다른 면은 그대로).
// 범위 view가 없는 장치(OpenGL ES, texture view 확장 없음)는 차원 바꾸기·일부 slice view가
// Unsupported인지 단언한다. device는 호출 스레드가 소유한다. 실패 수를 돌려준다.
rocks::uint32 RunViewDimensionChecks(const char* name, rocks::graphics::Device& device,
                                     rocks::Allocator& allocator) noexcept;

} // namespace sandbox
