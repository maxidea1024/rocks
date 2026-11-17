#include "VulkanFormats.h"

namespace rocks::graphics::vulkan {

namespace {

// 색 렌더 타깃(그리기·CopyPassTarget 복사·readback·샘플링)과 Sampled 텍스처(업로드·필터 샘플링)가
// 쓰는 기능이다.
constexpr VkFormatFeatureFlags ColorTargetFeatures =
    VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
constexpr VkFormatFeatureFlags FilteredSampledFeatures =
    VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;

} // namespace

// 지금은 RGBA8 두 형식만 대응한다. 나머지 유효 형식은 VK_FORMAT_UNDEFINED이고 장치가 Unsupported로
// 공개한다(PixelFormat.ko.md 8.3절, 대체하지 않는다). Vulkan 대응 주의점(BC1은 RGBA 블록, V8U8은
// SNORM, A8은 R8 + swizzle)은 PixelFormat.ko.md 15·17절이다.
TextureFormatInfo TextureFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::RGBA8Unorm:
        return {VK_FORMAT_R8G8B8A8_UNORM, ColorTargetFeatures, FilteredSampledFeatures};
    case PixelFormat::RGBA8UnormSrgb:
        return {VK_FORMAT_R8G8B8A8_SRGB, ColorTargetFeatures, FilteredSampledFeatures};
    default:
        break;
    }

    return {};
}

VkFormat VertexAttributeFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return VK_FORMAT_R32_SFLOAT;
    case VertexFormat::Float2:
        return VK_FORMAT_R32G32_SFLOAT;
    case VertexFormat::Float3:
        return VK_FORMAT_R32G32B32_SFLOAT;
    case VertexFormat::Float4:
        return VK_FORMAT_R32G32B32A32_SFLOAT;
    case VertexFormat::Byte4Snorm:
        return VK_FORMAT_R8G8B8A8_SNORM;
    case VertexFormat::UByte4:
        return VK_FORMAT_R8G8B8A8_UINT;
    case VertexFormat::UByte4Unorm:
        return VK_FORMAT_R8G8B8A8_UNORM;
    case VertexFormat::Bgra8Unorm:
        return VK_FORMAT_B8G8R8A8_UNORM;
    case VertexFormat::Short2:
        return VK_FORMAT_R16G16_SINT;
    case VertexFormat::Short4:
        return VK_FORMAT_R16G16B16A16_SINT;
    case VertexFormat::Short2Snorm:
        return VK_FORMAT_R16G16_SNORM;
    case VertexFormat::Short4Snorm:
        return VK_FORMAT_R16G16B16A16_SNORM;
    case VertexFormat::Half2:
        return VK_FORMAT_R16G16_SFLOAT;
    case VertexFormat::Half4:
        return VK_FORMAT_R16G16B16A16_SFLOAT;
    case VertexFormat::UShort2:
        return VK_FORMAT_R16G16_UINT;
    case VertexFormat::UShort4:
        return VK_FORMAT_R16G16B16A16_UINT;
    case VertexFormat::UShort2Unorm:
        return VK_FORMAT_R16G16_UNORM;
    case VertexFormat::UShort4Unorm:
        return VK_FORMAT_R16G16B16A16_UNORM;
    case VertexFormat::Rgb10A2Unorm:
        return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case VertexFormat::UInt1:
        return VK_FORMAT_R32_UINT;
    }

    return VK_FORMAT_UNDEFINED;
}

} // namespace rocks::graphics::vulkan
