#pragma once

#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 그래픽 backend 이름표다. 장치 계약(Graphics)과 장치 없이 backend를 다루는 도구(쿠커의 쿠킹
// 보장 샘플링 표 — Docs/Design/PixelFormat 7.1절)가 함께 쓴다.
namespace rocks::graphics {

// 그래픽 backend다. D3D12·D3D11·OpenGL·Vulkan은 구현 모듈(D3D12Graphics·D3D11Graphics·
// OpenGLGraphics·VulkanGraphics)이 있고, Metal은 자리만 둔다(구현 모듈이 아직 없어 선택하면
// Unsupported). 열거자 값은 0부터 빈틈 없이 쓴다(BackendCount개).
//
// 사용 예:
//   auto parsed = ParseBackendName(profile.graphicsBackends[0].View());  // "D3D12" → D3D12
//   printf("%s\n", BackendName(Backend::Vulkan));                        // "Vulkan"
enum class Backend : uint8 {
    D3D12,
    D3D11,
    OpenGL,
    Vulkan,
    Metal
};

// Backend 열거자 수다.
inline constexpr uint32 BackendCount = 5;

// "D3D12" 같은 이름이다(프로필 graphicsBackends·쿠킹 매니페스트 backend 어휘와 같은 철자). 범위
// 밖이면 "Unknown"이다.
ROCKS_GRAPHICSTYPES_API const char* BackendName(Backend backend) noexcept;

// BackendName의 이름을 backend로 읽는다(대소문자 구분). 실패: 모르는 이름("Unknown" 포함)
// NotFound.
ROCKS_GRAPHICSTYPES_API Result<Backend> ParseBackendName(StringView name) noexcept;

} // namespace rocks::graphics
