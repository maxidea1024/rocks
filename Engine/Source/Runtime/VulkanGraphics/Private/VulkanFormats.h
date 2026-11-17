#pragma once

#include <Graphics/DeviceTypes.h>
#include <vulkan/vulkan_core.h>

// Rocks 형식 → Vulkan 형식 대응이다(Docs/Design/VulkanGraphics.ko.md 6·7절, PixelFormat.ko.md).
// 형식마다 한 곳(switch 한 행)에서 정하므로 대응 형식을 늘릴 때 행만 더한다. 대응이 없는 형식은
// VK_FORMAT_UNDEFINED이고 호출자가 Unsupported·InvalidArgument로 공개한다(다른 형식으로 대체하지
// 않는다).
namespace rocks::graphics::vulkan {

// 텍스처 형식 대응이다(PixelFormat.ko.md 8.5절). 형식 기능 bit는 장치가
// vkGetPhysicalDeviceFormatProperties로 확인한다(VulkanDevice::QueryFormatCaps).
struct TextureFormatInfo {
    // 이미지 형식이다.
    VkFormat format = VK_FORMAT_UNDEFINED;
    // 이미지 전체의 aspect다(barrier). 색·비디오는 COLOR, 깊이는 DEPTH, 깊이·스텐실은
    // DEPTH|STENCIL.
    VkImageAspectFlags aspect = 0;
    // 기본 view의 성분 대응이다(A8Unorm은 R8 + (0, 0, 0, R)).
    VkComponentMapping components = {};
    // 비디오 형식의 plane별 view 형식이다(multi-planar 형식의 plane-compatible 형식).
    VkFormat planeFormats[2] = {};
    // view 형식이 이미지 형식과 다를 수 있다(sRGB 짝, 비디오 plane view, VK_IMAGE_CREATE_MUTABLE).
    bool mutableFormat = false;
};

TextureFormatInfo TextureFormat(PixelFormat format) noexcept;

// plane 하나를 복사할 때의 aspect다(깊이·스텐실 plane 0·1은 DEPTH·STENCIL, 비디오는 PLANE_0·1).
VkImageAspectFlags PlaneAspect(PixelFormat format, uint32 plane) noexcept;

// view 하나의 형식·aspect·성분 대응이다.
struct ViewFormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageAspectFlags aspect = 0;
    VkComponentMapping components = {};
};

// texture 형식의 plane을 viewFormat으로 읽는 view다(viewFormat은 ResolveTextureViewFormat이 확인한
// 값). 스텐실 plane은 값을 G 채널에서 읽도록 (R, R, 0, 1)로 대응한다(PixelFormat.ko.md 10절).
// 만들 수 없으면 format이 UNDEFINED다.
ViewFormatInfo ViewFormat(PixelFormat texture, uint32 plane, PixelFormat viewFormat) noexcept;

// 정점 원소 형식의 Vulkan 형식이다(RenderBase 10.4절 표의 Vulkan 열).
VkFormat VertexAttributeFormat(VertexFormat format) noexcept;

} // namespace rocks::graphics::vulkan
