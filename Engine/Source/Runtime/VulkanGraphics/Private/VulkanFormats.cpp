#include "VulkanFormats.h"

namespace rocks::graphics::vulkan {

namespace {

constexpr VkComponentMapping Identity = {
    VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY,
    VK_COMPONENT_SWIZZLE_IDENTITY};

bool Paired(PixelFormat format) noexcept {
    return GetPixelFormatInfo(format).srgbPair != PixelFormat::Unknown;
}

TextureFormatInfo Color(VkFormat format, bool srgbPair) noexcept {
    TextureFormatInfo info;
    info.format = format;
    info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    info.components = Identity;
    info.mutableFormat = srgbPair;
    return info;
}

TextureFormatInfo Depth(VkFormat format, VkImageAspectFlags aspect) noexcept {
    TextureFormatInfo info;
    info.format = format;
    info.aspect = aspect;
    info.components = Identity;
    return info;
}

TextureFormatInfo Video(VkFormat format, VkFormat luma, VkFormat chroma) noexcept {
    TextureFormatInfo info;
    info.format = format;
    info.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    info.components = Identity;
    info.planeFormats[0] = luma;
    info.planeFormats[1] = chroma;
    info.mutableFormat = true;
    return info;
}

} // namespace

// 대응 주의점(PixelFormat.ko.md 8.5절): BC1은 RGBA 블록(D3D 의미), RG8Snorm(UE V8U8)은 SNORM, A8은
// R8 + (0, 0, 0, R) 대응(샘플 전용), RGB9E5·NV12·P010은 UE에 없는 Rocks 대응이다. P010의 plane
// view는 10X6 형식이다(상위 10비트, 하위 6비트는 정의되지 않는다).
TextureFormatInfo TextureFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::R8Unorm:
        return Color(VK_FORMAT_R8_UNORM, Paired(PixelFormat::R8Unorm));
    case PixelFormat::R8UnormSrgb:
        return Color(VK_FORMAT_R8_SRGB, Paired(PixelFormat::R8UnormSrgb));
    case PixelFormat::R8Uint:
        return Color(VK_FORMAT_R8_UINT, Paired(PixelFormat::R8Uint));
    case PixelFormat::R8Sint:
        return Color(VK_FORMAT_R8_SINT, Paired(PixelFormat::R8Sint));
    case PixelFormat::RG8Unorm:
        return Color(VK_FORMAT_R8G8_UNORM, Paired(PixelFormat::RG8Unorm));
    case PixelFormat::RG8UnormSrgb:
        return Color(VK_FORMAT_R8G8_SRGB, Paired(PixelFormat::RG8UnormSrgb));
    case PixelFormat::RG8Snorm:
        return Color(VK_FORMAT_R8G8_SNORM, Paired(PixelFormat::RG8Snorm));
    case PixelFormat::RG8Uint:
        return Color(VK_FORMAT_R8G8_UINT, Paired(PixelFormat::RG8Uint));
    case PixelFormat::RGB8Unorm:
        return Color(VK_FORMAT_R8G8B8_UNORM, Paired(PixelFormat::RGB8Unorm));
    case PixelFormat::RGB8UnormSrgb:
        return Color(VK_FORMAT_R8G8B8_SRGB, Paired(PixelFormat::RGB8UnormSrgb));
    case PixelFormat::RGBA8Unorm:
        return Color(VK_FORMAT_R8G8B8A8_UNORM, Paired(PixelFormat::RGBA8Unorm));
    case PixelFormat::RGBA8UnormSrgb:
        return Color(VK_FORMAT_R8G8B8A8_SRGB, Paired(PixelFormat::RGBA8UnormSrgb));
    case PixelFormat::RGBA8Snorm:
        return Color(VK_FORMAT_R8G8B8A8_SNORM, Paired(PixelFormat::RGBA8Snorm));
    case PixelFormat::RGBA8Uint:
        return Color(VK_FORMAT_R8G8B8A8_UINT, Paired(PixelFormat::RGBA8Uint));
    case PixelFormat::BGRA8Unorm:
        return Color(VK_FORMAT_B8G8R8A8_UNORM, Paired(PixelFormat::BGRA8Unorm));
    case PixelFormat::BGRA8UnormSrgb:
        return Color(VK_FORMAT_B8G8R8A8_SRGB, Paired(PixelFormat::BGRA8UnormSrgb));
    case PixelFormat::R16Unorm:
        return Color(VK_FORMAT_R16_UNORM, Paired(PixelFormat::R16Unorm));
    case PixelFormat::R16Uint:
        return Color(VK_FORMAT_R16_UINT, Paired(PixelFormat::R16Uint));
    case PixelFormat::R16Sint:
        return Color(VK_FORMAT_R16_SINT, Paired(PixelFormat::R16Sint));
    case PixelFormat::R16Float:
        return Color(VK_FORMAT_R16_SFLOAT, Paired(PixelFormat::R16Float));
    case PixelFormat::RG16Unorm:
        return Color(VK_FORMAT_R16G16_UNORM, Paired(PixelFormat::RG16Unorm));
    case PixelFormat::RG16Snorm:
        return Color(VK_FORMAT_R16G16_SNORM, Paired(PixelFormat::RG16Snorm));
    case PixelFormat::RG16Uint:
        return Color(VK_FORMAT_R16G16_UINT, Paired(PixelFormat::RG16Uint));
    case PixelFormat::RG16Sint:
        return Color(VK_FORMAT_R16G16_SINT, Paired(PixelFormat::RG16Sint));
    case PixelFormat::RG16Float:
        return Color(VK_FORMAT_R16G16_SFLOAT, Paired(PixelFormat::RG16Float));
    case PixelFormat::RGBA16Unorm:
        return Color(VK_FORMAT_R16G16B16A16_UNORM, Paired(PixelFormat::RGBA16Unorm));
    case PixelFormat::RGBA16Snorm:
        return Color(VK_FORMAT_R16G16B16A16_SNORM, Paired(PixelFormat::RGBA16Snorm));
    case PixelFormat::RGBA16Uint:
        return Color(VK_FORMAT_R16G16B16A16_UINT, Paired(PixelFormat::RGBA16Uint));
    case PixelFormat::RGBA16Sint:
        return Color(VK_FORMAT_R16G16B16A16_SINT, Paired(PixelFormat::RGBA16Sint));
    case PixelFormat::RGBA16Float:
        return Color(VK_FORMAT_R16G16B16A16_SFLOAT, Paired(PixelFormat::RGBA16Float));
    case PixelFormat::R32Uint:
        return Color(VK_FORMAT_R32_UINT, Paired(PixelFormat::R32Uint));
    case PixelFormat::R32Sint:
        return Color(VK_FORMAT_R32_SINT, Paired(PixelFormat::R32Sint));
    case PixelFormat::R32Float:
        return Color(VK_FORMAT_R32_SFLOAT, Paired(PixelFormat::R32Float));
    case PixelFormat::RG32Uint:
        return Color(VK_FORMAT_R32G32_UINT, Paired(PixelFormat::RG32Uint));
    case PixelFormat::RG32Float:
        return Color(VK_FORMAT_R32G32_SFLOAT, Paired(PixelFormat::RG32Float));
    case PixelFormat::RGB32Uint:
        return Color(VK_FORMAT_R32G32B32_UINT, Paired(PixelFormat::RGB32Uint));
    case PixelFormat::RGB32Sint:
        return Color(VK_FORMAT_R32G32B32_SINT, Paired(PixelFormat::RGB32Sint));
    case PixelFormat::RGB32Float:
        return Color(VK_FORMAT_R32G32B32_SFLOAT, Paired(PixelFormat::RGB32Float));
    case PixelFormat::RGBA32Uint:
        return Color(VK_FORMAT_R32G32B32A32_UINT, Paired(PixelFormat::RGBA32Uint));
    case PixelFormat::RGBA32Float:
        return Color(VK_FORMAT_R32G32B32A32_SFLOAT, Paired(PixelFormat::RGBA32Float));
    case PixelFormat::R64Uint:
        return Color(VK_FORMAT_R64_UINT, Paired(PixelFormat::R64Uint));
    case PixelFormat::R5G6B5Unorm:
        return Color(VK_FORMAT_R5G6B5_UNORM_PACK16, Paired(PixelFormat::R5G6B5Unorm));
    case PixelFormat::B5G5R5A1Unorm:
        return Color(VK_FORMAT_A1R5G5B5_UNORM_PACK16, Paired(PixelFormat::B5G5R5A1Unorm));
    case PixelFormat::RGB10A2Unorm:
        return Color(VK_FORMAT_A2B10G10R10_UNORM_PACK32, Paired(PixelFormat::RGB10A2Unorm));
    case PixelFormat::RG11B10Ufloat:
        return Color(VK_FORMAT_B10G11R11_UFLOAT_PACK32, Paired(PixelFormat::RG11B10Ufloat));
    case PixelFormat::RGB9E5Ufloat:
        return Color(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32, Paired(PixelFormat::RGB9E5Ufloat));
    case PixelFormat::BC1Unorm:
        return Color(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, Paired(PixelFormat::BC1Unorm));
    case PixelFormat::BC1UnormSrgb:
        return Color(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, Paired(PixelFormat::BC1UnormSrgb));
    case PixelFormat::BC2Unorm:
        return Color(VK_FORMAT_BC2_UNORM_BLOCK, Paired(PixelFormat::BC2Unorm));
    case PixelFormat::BC2UnormSrgb:
        return Color(VK_FORMAT_BC2_SRGB_BLOCK, Paired(PixelFormat::BC2UnormSrgb));
    case PixelFormat::BC3Unorm:
        return Color(VK_FORMAT_BC3_UNORM_BLOCK, Paired(PixelFormat::BC3Unorm));
    case PixelFormat::BC3UnormSrgb:
        return Color(VK_FORMAT_BC3_SRGB_BLOCK, Paired(PixelFormat::BC3UnormSrgb));
    case PixelFormat::BC4Unorm:
        return Color(VK_FORMAT_BC4_UNORM_BLOCK, Paired(PixelFormat::BC4Unorm));
    case PixelFormat::BC5Unorm:
        return Color(VK_FORMAT_BC5_UNORM_BLOCK, Paired(PixelFormat::BC5Unorm));
    case PixelFormat::BC6HUfloat:
        return Color(VK_FORMAT_BC6H_UFLOAT_BLOCK, Paired(PixelFormat::BC6HUfloat));
    case PixelFormat::BC7Unorm:
        return Color(VK_FORMAT_BC7_UNORM_BLOCK, Paired(PixelFormat::BC7Unorm));
    case PixelFormat::BC7UnormSrgb:
        return Color(VK_FORMAT_BC7_SRGB_BLOCK, Paired(PixelFormat::BC7UnormSrgb));
    case PixelFormat::ETC2RGB8Unorm:
        return Color(VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK, Paired(PixelFormat::ETC2RGB8Unorm));
    case PixelFormat::ETC2RGB8UnormSrgb:
        return Color(VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK, Paired(PixelFormat::ETC2RGB8UnormSrgb));
    case PixelFormat::ETC2RGBA8Unorm:
        return Color(VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK, Paired(PixelFormat::ETC2RGBA8Unorm));
    case PixelFormat::ETC2RGBA8UnormSrgb:
        return Color(VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK, Paired(PixelFormat::ETC2RGBA8UnormSrgb));
    case PixelFormat::EACR11Unorm:
        return Color(VK_FORMAT_EAC_R11_UNORM_BLOCK, Paired(PixelFormat::EACR11Unorm));
    case PixelFormat::EACRG11Unorm:
        return Color(VK_FORMAT_EAC_R11G11_UNORM_BLOCK, Paired(PixelFormat::EACRG11Unorm));
    case PixelFormat::ASTC4x4Unorm:
        return Color(VK_FORMAT_ASTC_4x4_UNORM_BLOCK, Paired(PixelFormat::ASTC4x4Unorm));
    case PixelFormat::ASTC4x4UnormSrgb:
        return Color(VK_FORMAT_ASTC_4x4_SRGB_BLOCK, Paired(PixelFormat::ASTC4x4UnormSrgb));
    case PixelFormat::ASTC4x4Hdr:
        return Color(VK_FORMAT_ASTC_4x4_SFLOAT_BLOCK, Paired(PixelFormat::ASTC4x4Hdr));
    case PixelFormat::ASTC6x6Unorm:
        return Color(VK_FORMAT_ASTC_6x6_UNORM_BLOCK, Paired(PixelFormat::ASTC6x6Unorm));
    case PixelFormat::ASTC6x6UnormSrgb:
        return Color(VK_FORMAT_ASTC_6x6_SRGB_BLOCK, Paired(PixelFormat::ASTC6x6UnormSrgb));
    case PixelFormat::ASTC6x6Hdr:
        return Color(VK_FORMAT_ASTC_6x6_SFLOAT_BLOCK, Paired(PixelFormat::ASTC6x6Hdr));
    case PixelFormat::ASTC8x8Unorm:
        return Color(VK_FORMAT_ASTC_8x8_UNORM_BLOCK, Paired(PixelFormat::ASTC8x8Unorm));
    case PixelFormat::ASTC8x8UnormSrgb:
        return Color(VK_FORMAT_ASTC_8x8_SRGB_BLOCK, Paired(PixelFormat::ASTC8x8UnormSrgb));
    case PixelFormat::ASTC8x8Hdr:
        return Color(VK_FORMAT_ASTC_8x8_SFLOAT_BLOCK, Paired(PixelFormat::ASTC8x8Hdr));
    case PixelFormat::ASTC10x10Unorm:
        return Color(VK_FORMAT_ASTC_10x10_UNORM_BLOCK, Paired(PixelFormat::ASTC10x10Unorm));
    case PixelFormat::ASTC10x10UnormSrgb:
        return Color(VK_FORMAT_ASTC_10x10_SRGB_BLOCK, Paired(PixelFormat::ASTC10x10UnormSrgb));
    case PixelFormat::ASTC10x10Hdr:
        return Color(VK_FORMAT_ASTC_10x10_SFLOAT_BLOCK, Paired(PixelFormat::ASTC10x10Hdr));
    case PixelFormat::ASTC12x12Unorm:
        return Color(VK_FORMAT_ASTC_12x12_UNORM_BLOCK, Paired(PixelFormat::ASTC12x12Unorm));
    case PixelFormat::ASTC12x12UnormSrgb:
        return Color(VK_FORMAT_ASTC_12x12_SRGB_BLOCK, Paired(PixelFormat::ASTC12x12UnormSrgb));
    case PixelFormat::ASTC12x12Hdr:
        return Color(VK_FORMAT_ASTC_12x12_SFLOAT_BLOCK, Paired(PixelFormat::ASTC12x12Hdr));
    case PixelFormat::A8Unorm: {
        TextureFormatInfo info = Color(VK_FORMAT_R8_UNORM, false);
        info.components = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO,
                           VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R};
        return info;
    }
    case PixelFormat::D16Unorm:
        return Depth(VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT);
    case PixelFormat::D24Unorm:
        return Depth(VK_FORMAT_X8_D24_UNORM_PACK32, VK_IMAGE_ASPECT_DEPTH_BIT);
    case PixelFormat::D32Float:
        return Depth(VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
    case PixelFormat::D24UnormS8Uint:
        return Depth(VK_FORMAT_D24_UNORM_S8_UINT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    case PixelFormat::D32FloatS8Uint:
        return Depth(VK_FORMAT_D32_SFLOAT_S8_UINT,
                     VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    case PixelFormat::NV12:
        return Video(VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM);
    case PixelFormat::P010:
        return Video(VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16,
                     VK_FORMAT_R10X6_UNORM_PACK16, VK_FORMAT_R10X6G10X6_UNORM_2PACK16);
    case PixelFormat::Unknown:
        break;
    }

    return {};
}

VkImageAspectFlags PlaneAspect(PixelFormat format, uint32 plane) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    switch (info.kind) {
    case PixelFormatKind::Depth:
        return VK_IMAGE_ASPECT_DEPTH_BIT;
    case PixelFormatKind::DepthStencil:
        return plane == 0 ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_STENCIL_BIT;
    case PixelFormatKind::Planar:
        return plane == 0 ? VK_IMAGE_ASPECT_PLANE_0_BIT : VK_IMAGE_ASPECT_PLANE_1_BIT;
    default:
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

ViewFormatInfo ViewFormat(PixelFormat texture, uint32 plane, PixelFormat viewFormat) noexcept {
    const TextureFormatInfo base = TextureFormat(texture);
    const PixelFormatInfo& info = GetPixelFormatInfo(texture);
    ViewFormatInfo view;
    if (base.format == VK_FORMAT_UNDEFINED || plane >= info.planeCount) {
        return view;
    }
    switch (info.kind) {
    case PixelFormatKind::Planar:
        view.format = base.planeFormats[plane];
        view.aspect = PlaneAspect(texture, plane);
        view.components = Identity;
        return view;
    case PixelFormatKind::Depth:
    case PixelFormatKind::DepthStencil:
        view.format = base.format;
        view.aspect = PlaneAspect(texture, plane);
        if (plane == 1) {
            view.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                               VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE};
        } else {
            view.components = Identity;
        }
        return view;
    default:
        break;
    }
    // 단일 plane 색은 자기 형식이나 sRGB 짝(같은 이미지에 MUTABLE로 만든다)만 읽는다.
    if (viewFormat != texture && !base.mutableFormat) {
        return view;
    }
    const TextureFormatInfo other = TextureFormat(viewFormat);
    view.format = other.format;
    view.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    view.components = other.components;
    return view;
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

VkComponentMapping ComposeComponents(const ChannelSwizzle& outer,
                                     const VkComponentMapping& inner) noexcept {
    const VkComponentSwizzle channels[4] = {inner.r, inner.g, inner.b, inner.a};
    auto pick = [&channels](SwizzleSource source) noexcept {
        switch (source) {
        case SwizzleSource::Zero:
            return VK_COMPONENT_SWIZZLE_ZERO;
        case SwizzleSource::One:
            return VK_COMPONENT_SWIZZLE_ONE;
        case SwizzleSource::Red:
        case SwizzleSource::Green:
        case SwizzleSource::Blue:
        case SwizzleSource::Alpha:
            break;
        }

        const uint32 index = static_cast<uint32>(source);
        const VkComponentSwizzle selected = channels[index];
        // IDENTITY는 그 성분 자신이다(R = 3부터 R·G·B·A 순서).
        return selected == VK_COMPONENT_SWIZZLE_IDENTITY
                   ? static_cast<VkComponentSwizzle>(VK_COMPONENT_SWIZZLE_R + index)
                   : selected;
    };
    return VkComponentMapping{pick(outer.red), pick(outer.green), pick(outer.blue),
                              pick(outer.alpha)};
}

VkImageViewType ViewTypeOf(TextureDimension dimension) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        return VK_IMAGE_VIEW_TYPE_2D;
    case TextureDimension::Texture2DArray:
        return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    case TextureDimension::Texture3D:
        return VK_IMAGE_VIEW_TYPE_3D;
    case TextureDimension::TextureCube:
        return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureDimension::TextureCubeArray:
        return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    }

    return VK_IMAGE_VIEW_TYPE_2D;
}

} // namespace rocks::graphics::vulkan
