#pragma once

#include <Graphics/Device.h>

namespace rocks::graphics {

// Vulkan 1.1 장치를 만든다(Docs/Design/VulkanGraphics.ko.md). 시스템 Vulkan 로더(vulkan-1.dll,
// libvulkan.so)를 실행 중에 연다(SDK를 링크하지 않는다). 외장 → 내장 → 가상 GPU 순으로 고르고,
// 하드웨어가 없거나 desc.preferSoftware이면 CPU 장치(lavapipe·SwiftShader 같은 소프트웨어 ICD)를
// 고른다(preferSoftware인데 CPU 장치가 없으면 Unsupported). desc.enableValidation이면
// VK_LAYER_KHRONOS_validation을 켠다(설치되지 않았으면 켜지 않고 계속한다).
//
// 셰이더: 쿠킹된 SPIR-V(VULKAN_SM5 — set = 단계, binding = layout slot)를 CreateShader로 받는다
// (ShaderSystem 16.1절 SSD18). 실행 중 컴파일러는 없다.
//
// 실패: 로더·Vulkan 1.1·GRAPHICS queue가 없으면 Unsupported, Vulkan 호출 실패는
// DependencyFailed·OutOfMemory, 할당 실패는 그 오류다. report가 null이 아니면 실패 단계·원본
// VkResult(nativeCode)·설명을 쓴다(성공이면 code None). Windows·Android가 아니면 Unsupported다.
//
// 구현 규약:
// - 제출마다 VkFence 하나로 완료를 추적한다(timeline semaphore는 1.1 core가 아니라 쓰지 않는다).
// - 창 출력은 중간 RGBA8 이미지에 그리고 EndFrame에서 swapchain 이미지에 blit해 present한다.
// - 정점·index 버퍼와 텍스처는 장치 전용 메모리이고 업로드 ring·staging에서 복사한다.
// - 앞면은 화면 반시계(음수 viewport 높이 + VK_FRONT_FACE_COUNTER_CLOCKWISE)다.
//
// 사용 예:
//   graphics::DeviceCreateReport report;
//   auto created = graphics::CreateVulkanDevice(allocator, desc, &report);
//   if (!created) {
//       printf("%s: %s (VkResult %lld)\n", report.stage, report.detail,
//              static_cast<long long>(report.nativeCode));
//   }
ROCKS_VULKANGRAPHICS_API Result<UniquePtr<Device>>
CreateVulkanDevice(Allocator& allocator, const DeviceDesc& desc,
                   DeviceCreateReport* report = nullptr) noexcept;

} // namespace rocks::graphics
