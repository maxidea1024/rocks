#pragma once

#include <Graphics/Device.h>

namespace sandbox {

// 이 장치가 지금 세션에서 창(스왑체인)에 내보낼 수 없는 이유다. 내보낼 수 있으면 nullptr이다.
//
// 지금 해당하는 경우는 하나다. mesa 소프트웨어 Vulkan(lavapipe)의 Windows WSI는 present를 GDI
// StretchBlt로 창 DC에 그린다(mesa 26.2.7 src/vulkan/wsi/wsi_common_win32.cpp:823–832). 끊긴
// 데스크톱 세션에서는 숨긴 창으로의 StretchBlt가 실패해 VK_ERROR_MEMORY_MAP_FAILED가 되고, 그
// swapchain의 뒤 acquire도 같은 오류다(:736). 창 경로 검사는 이 이유를 출력하고 NOT RUN으로 둔다.
// 하드웨어 backend(D3D12·D3D11·OpenGL ES·Vulkan)의 present는 끊긴 세션에서도 동작하므로 그대로
// 돌린다. 이유 문자열은 영어이고 정적 수명이다.
//
// 사용 예:
//   if (const char* reason = WindowPresentUnavailableReason(device)) {
//       printf("  [NOT RUN] %s swap chain: %s\n", name, reason);
//       return;
//   }
const char* WindowPresentUnavailableReason(rocks::graphics::Device& device) noexcept;

} // namespace sandbox
