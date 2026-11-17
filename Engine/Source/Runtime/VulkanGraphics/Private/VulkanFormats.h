#pragma once

#include <Graphics/DeviceTypes.h>
#include <vulkan/vulkan_core.h>

// Rocks 형식 → Vulkan 형식 대응이다(Docs/Design/VulkanGraphics.ko.md 6·7절, PixelFormat.ko.md).
// 형식마다 한 곳(switch 한 행)에서 정하므로 대응 형식을 늘릴 때 행만 더한다. 대응이 없는 형식은
// VK_FORMAT_UNDEFINED이고 호출자가 Unsupported·InvalidArgument로 공개한다(다른 형식으로 대체하지
// 않는다).
namespace rocks::graphics::vulkan {

// 텍스처 형식 대응이다. 형식 기능 bit는 장치가 vkGetPhysicalDeviceFormatProperties로 확인한다.
struct TextureFormatInfo {
    // 이미지와 기본 view의 형식이다.
    VkFormat format = VK_FORMAT_UNDEFINED;
    // RenderTarget으로 쓸 때 장치가 줘야 하는 optimal tiling 기능이다.
    VkFormatFeatureFlags renderTargetFeatures = 0;
    // Sampled로 쓸 때 장치가 줘야 하는 optimal tiling 기능이다.
    VkFormatFeatureFlags sampledFeatures = 0;
};

TextureFormatInfo TextureFormat(PixelFormat format) noexcept;

// 정점 원소 형식의 Vulkan 형식이다(RenderBase 10.4절 표의 Vulkan 열).
VkFormat VertexAttributeFormat(VertexFormat format) noexcept;

} // namespace rocks::graphics::vulkan
