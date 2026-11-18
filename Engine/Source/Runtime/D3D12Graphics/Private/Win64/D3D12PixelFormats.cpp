#include "D3D12PixelFormats.h"

#include <Graphics/DeviceCaps.h>

namespace rocks::graphics::d3d12 {

namespace {

using PF = PixelFormat;

// 자원·SRV·RTV·UAV가 같은 형식인 비압축 색 형식이다. 실제 지원은 능력 질의가 정한다.
constexpr FormatMapping Plain(DXGI_FORMAT format) noexcept {
    FormatMapping mapping;
    mapping.resource = format;
    mapping.srv = format;
    mapping.rtv = format;
    mapping.uav = format;
    return mapping;
}

// typeless 자원을 view 형식으로 읽는 색 형식이다(sRGB 짝). sRGB view는 UAV가 없다.
constexpr FormatMapping Typed(DXGI_FORMAT resource, DXGI_FORMAT view, bool srgb) noexcept {
    FormatMapping mapping;
    mapping.resource = resource;
    mapping.srv = view;
    mapping.rtv = view;
    mapping.uav = srgb ? DXGI_FORMAT_UNKNOWN : view;
    return mapping;
}

// 블록 압축 형식이다(샘플 전용, 렌더 타깃·UAV 없음).
constexpr FormatMapping Block(DXGI_FORMAT resource, DXGI_FORMAT view) noexcept {
    FormatMapping mapping;
    mapping.resource = resource;
    mapping.srv = view;
    return mapping;
}

// 깊이(·스텐실) 형식이다. 자원은 typeless, 깊이 SRV·DSV·스텐실 SRV를 따로 둔다.
constexpr FormatMapping DepthFormat(DXGI_FORMAT resource, DXGI_FORMAT depthSrv, DXGI_FORMAT dsv,
                                    DXGI_FORMAT stencilSrv) noexcept {
    FormatMapping mapping;
    mapping.resource = resource;
    mapping.srv = depthSrv;
    mapping.planeSrv = stencilSrv;
    mapping.dsv = dsv;
    return mapping;
}

// 4:2:0 2-plane 비디오 형식이다. plane별 SRV로 휘도·색차를 읽는다.
constexpr FormatMapping Video(DXGI_FORMAT resource, DXGI_FORMAT luma, DXGI_FORMAT chroma) noexcept {
    FormatMapping mapping;
    mapping.resource = resource;
    mapping.srv = luma;
    mapping.planeSrv = chroma;
    return mapping;
}

struct Support {
    bool valid = false;
    UINT support1 = 0;
    UINT support2 = 0;
};

Support Query(ID3D12Device* device, DXGI_FORMAT format) noexcept {
    Support result;
    if (format == DXGI_FORMAT_UNKNOWN) {
        return result;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT data = {};
    data.Format = format;
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &data, sizeof(data)))) {
        result.valid = true;
        result.support1 = static_cast<UINT>(data.Support1);
        result.support2 = static_cast<UINT>(data.Support2);
    }

    return result;
}

constexpr bool Has1(const Support& support, D3D12_FORMAT_SUPPORT1 flag) noexcept {
    return (support.support1 & static_cast<UINT>(flag)) != 0;
}

constexpr bool Has2(const Support& support, D3D12_FORMAT_SUPPORT2 flag) noexcept {
    return (support.support2 & static_cast<UINT>(flag)) != 0;
}

void Add(PixelFormatCaps& caps, bool condition, PixelFormatCaps flag) noexcept {
    if (condition) {
        caps = caps | flag;
    }
}

} // namespace

FormatMapping MappingOf(PixelFormat format) noexcept {
    switch (format) {
    case PF::R8Unorm:
        return Plain(DXGI_FORMAT_R8_UNORM);
    case PF::R8Uint:
        return Plain(DXGI_FORMAT_R8_UINT);
    case PF::R8Sint:
        return Plain(DXGI_FORMAT_R8_SINT);
    case PF::A8Unorm:
        return Plain(DXGI_FORMAT_A8_UNORM);
    case PF::RG8Unorm:
        return Plain(DXGI_FORMAT_R8G8_UNORM);
    case PF::RG8Snorm:
        return Plain(DXGI_FORMAT_R8G8_SNORM);
    case PF::RG8Uint:
        return Plain(DXGI_FORMAT_R8G8_UINT);
    case PF::RGBA8Unorm:
        return Typed(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM, false);
    case PF::RGBA8UnormSrgb:
        return Typed(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, true);
    case PF::RGBA8Snorm:
        return Plain(DXGI_FORMAT_R8G8B8A8_SNORM);
    case PF::RGBA8Uint:
        return Plain(DXGI_FORMAT_R8G8B8A8_UINT);
    case PF::BGRA8Unorm:
        return Typed(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM, false);
    case PF::BGRA8UnormSrgb:
        return Typed(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, true);
    case PF::R16Unorm:
        return Plain(DXGI_FORMAT_R16_UNORM);
    case PF::R16Uint:
        return Plain(DXGI_FORMAT_R16_UINT);
    case PF::R16Sint:
        return Plain(DXGI_FORMAT_R16_SINT);
    case PF::R16Float:
        return Plain(DXGI_FORMAT_R16_FLOAT);
    case PF::RG16Unorm:
        return Plain(DXGI_FORMAT_R16G16_UNORM);
    case PF::RG16Snorm:
        return Plain(DXGI_FORMAT_R16G16_SNORM);
    case PF::RG16Uint:
        return Plain(DXGI_FORMAT_R16G16_UINT);
    case PF::RG16Sint:
        return Plain(DXGI_FORMAT_R16G16_SINT);
    case PF::RG16Float:
        return Plain(DXGI_FORMAT_R16G16_FLOAT);
    case PF::RGBA16Unorm:
        return Plain(DXGI_FORMAT_R16G16B16A16_UNORM);
    case PF::RGBA16Snorm:
        return Plain(DXGI_FORMAT_R16G16B16A16_SNORM);
    case PF::RGBA16Uint:
        return Plain(DXGI_FORMAT_R16G16B16A16_UINT);
    case PF::RGBA16Sint:
        return Plain(DXGI_FORMAT_R16G16B16A16_SINT);
    case PF::RGBA16Float:
        return Plain(DXGI_FORMAT_R16G16B16A16_FLOAT);
    case PF::R32Uint:
        return Plain(DXGI_FORMAT_R32_UINT);
    case PF::R32Sint:
        return Plain(DXGI_FORMAT_R32_SINT);
    case PF::R32Float:
        return Plain(DXGI_FORMAT_R32_FLOAT);
    case PF::RG32Uint:
        return Plain(DXGI_FORMAT_R32G32_UINT);
    case PF::RG32Float:
        return Plain(DXGI_FORMAT_R32G32_FLOAT);
    case PF::RGB32Uint:
        return Plain(DXGI_FORMAT_R32G32B32_UINT);
    case PF::RGB32Sint:
        return Plain(DXGI_FORMAT_R32G32B32_SINT);
    case PF::RGB32Float:
        return Plain(DXGI_FORMAT_R32G32B32_FLOAT);
    case PF::RGBA32Uint:
        return Plain(DXGI_FORMAT_R32G32B32A32_UINT);
    case PF::RGBA32Float:
        return Plain(DXGI_FORMAT_R32G32B32A32_FLOAT);
    case PF::R5G6B5Unorm:
        return Plain(DXGI_FORMAT_B5G6R5_UNORM);
    case PF::B5G5R5A1Unorm:
        return Plain(DXGI_FORMAT_B5G5R5A1_UNORM);
    case PF::RGB10A2Unorm:
        return Plain(DXGI_FORMAT_R10G10B10A2_UNORM);
    case PF::RG11B10Ufloat:
        return Plain(DXGI_FORMAT_R11G11B10_FLOAT);
    case PF::RGB9E5Ufloat:
        return Plain(DXGI_FORMAT_R9G9B9E5_SHAREDEXP);
    case PF::D16Unorm:
        return DepthFormat(DXGI_FORMAT_R16_TYPELESS, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_D16_UNORM,
                           DXGI_FORMAT_UNKNOWN);
    case PF::D24Unorm:
        // DXGI에 스텐실 없는 24비트 깊이가 없어 D24S8 자원의 깊이 plane만 쓴다.
        return DepthFormat(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                           DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_UNKNOWN);
    case PF::D32Float:
        return DepthFormat(DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_D32_FLOAT,
                           DXGI_FORMAT_UNKNOWN);
    case PF::D24UnormS8Uint:
        return DepthFormat(DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
                           DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_X24_TYPELESS_G8_UINT);
    case PF::D32FloatS8Uint:
        return DepthFormat(DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
                           DXGI_FORMAT_D32_FLOAT_S8X24_UINT, DXGI_FORMAT_X32_TYPELESS_G8X24_UINT);
    case PF::BC1Unorm:
        return Block(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM);
    case PF::BC1UnormSrgb:
        return Block(DXGI_FORMAT_BC1_TYPELESS, DXGI_FORMAT_BC1_UNORM_SRGB);
    case PF::BC2Unorm:
        return Block(DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM);
    case PF::BC2UnormSrgb:
        return Block(DXGI_FORMAT_BC2_TYPELESS, DXGI_FORMAT_BC2_UNORM_SRGB);
    case PF::BC3Unorm:
        return Block(DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM);
    case PF::BC3UnormSrgb:
        return Block(DXGI_FORMAT_BC3_TYPELESS, DXGI_FORMAT_BC3_UNORM_SRGB);
    case PF::BC4Unorm:
        return Block(DXGI_FORMAT_BC4_UNORM, DXGI_FORMAT_BC4_UNORM);
    case PF::BC5Unorm:
        return Block(DXGI_FORMAT_BC5_UNORM, DXGI_FORMAT_BC5_UNORM);
    case PF::BC6HUfloat:
        return Block(DXGI_FORMAT_BC6H_UF16, DXGI_FORMAT_BC6H_UF16);
    case PF::BC7Unorm:
        return Block(DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM);
    case PF::BC7UnormSrgb:
        return Block(DXGI_FORMAT_BC7_TYPELESS, DXGI_FORMAT_BC7_UNORM_SRGB);
    case PF::NV12:
        return Video(DXGI_FORMAT_NV12, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8G8_UNORM);
    case PF::P010:
        return Video(DXGI_FORMAT_P010, DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R16G16_UNORM);
    // DXGI에 장치 형식이 없다(PixelFormat.ko.md 8.4절).
    case PF::Unknown:
    case PF::R8UnormSrgb:
    case PF::RG8UnormSrgb:
    case PF::RGB8Unorm:
    case PF::RGB8UnormSrgb:
    case PF::R64Uint:
    case PF::ETC2RGB8Unorm:
    case PF::ETC2RGB8UnormSrgb:
    case PF::ETC2RGBA8Unorm:
    case PF::ETC2RGBA8UnormSrgb:
    case PF::EACR11Unorm:
    case PF::EACRG11Unorm:
    case PF::ASTC4x4Unorm:
    case PF::ASTC4x4UnormSrgb:
    case PF::ASTC4x4Hdr:
    case PF::ASTC6x6Unorm:
    case PF::ASTC6x6UnormSrgb:
    case PF::ASTC6x6Hdr:
    case PF::ASTC8x8Unorm:
    case PF::ASTC8x8UnormSrgb:
    case PF::ASTC8x8Hdr:
    case PF::ASTC10x10Unorm:
    case PF::ASTC10x10UnormSrgb:
    case PF::ASTC10x10Hdr:
    case PF::ASTC12x12Unorm:
    case PF::ASTC12x12UnormSrgb:
    case PF::ASTC12x12Hdr:
        break;
    }

    return FormatMapping{};
}

DXGI_FORMAT ViewSrvFormat(PixelFormat texture, uint32 plane, PixelFormat viewFormat) noexcept {
    const FormatMapping base = MappingOf(texture);
    if (base.resource == DXGI_FORMAT_UNKNOWN) {
        return DXGI_FORMAT_UNKNOWN;
    }
    if (GetPixelFormatInfo(texture).planeCount == 2) {
        return plane == 0 ? base.srv : (plane == 1 ? base.planeSrv : DXGI_FORMAT_UNKNOWN);
    }
    if (plane != 0) {
        return DXGI_FORMAT_UNKNOWN;
    }
    // 단일 plane은 같은 typeless 자원을 공유하는 형식(자기 자신 또는 sRGB 짝)만 읽는다.
    const FormatMapping view = MappingOf(viewFormat);
    return view.resource == base.resource ? view.srv : DXGI_FORMAT_UNKNOWN;
}

uint32 FormatPlaneCount(ID3D12Device* device, DXGI_FORMAT format) noexcept {
    D3D12_FEATURE_DATA_FORMAT_INFO info = {};
    info.Format = format;
    if (device == nullptr || format == DXGI_FORMAT_UNKNOWN ||
        FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info)))) {
        return 1;
    }

    return info.PlaneCount;
}

PixelFormatCaps QueryFormatCaps(ID3D12Device* device, PixelFormat format) noexcept {
    const FormatMapping mapping = MappingOf(format);
    if (device == nullptr || mapping.resource == DXGI_FORMAT_UNKNOWN) {
        return PixelFormatCaps::None;
    }
    const PixelFormatInfo& info = GetPixelFormatInfo(format);
    // typeless 자원 형식의 질의가 실패하는 장치는 view 형식의 차원 지원을 쓴다.
    Support resource = Query(device, mapping.resource);
    const Support srv = Query(device, mapping.srv);
    if (!resource.valid) {
        resource = srv;
    }
    const Support rtv = Query(device, mapping.rtv);
    const Support dsv = Query(device, mapping.dsv);
    const Support uav = Query(device, mapping.uav);

    PixelFormatCaps caps = PixelFormatCaps::None;
    // 다중 plane(비디오)은 plane SRV가 아니라 자원 형식 자체의 셰이더 지원으로 판정한다.
    const Support& shader = info.kind == PixelFormatKind::Planar ? resource : srv;
    const bool depth =
        info.kind == PixelFormatKind::Depth || info.kind == PixelFormatKind::DepthStencil;
    // 깊이 형식은 DEPTH_STENCIL 플래그로 만들므로 DSV 지원이 있어야 자원을 만들 수 있다.
    const bool creatable = Has1(resource, D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
                           (!depth || Has1(dsv, D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL));
    const bool sampled = creatable && (Has1(shader, D3D12_FORMAT_SUPPORT1_SHADER_LOAD) ||
                                       Has1(shader, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE));
    Add(caps, sampled, PixelFormatCaps::Sampled);
    Add(caps,
        sampled && Has1(resource, D3D12_FORMAT_SUPPORT1_TEXTURE3D) && !depth &&
            info.planeCount == 1,
        PixelFormatCaps::Texture3D);
    Add(caps, sampled && Has1(resource, D3D12_FORMAT_SUPPORT1_TEXTURECUBE),
        PixelFormatCaps::TextureCube);
    Add(caps, sampled && Has1(shader, D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE),
        PixelFormatCaps::Filterable);
    Add(caps, sampled && Has1(shader, D3D12_FORMAT_SUPPORT1_SHADER_GATHER),
        PixelFormatCaps::Gather);
    Add(caps, sampled && Has1(resource, D3D12_FORMAT_SUPPORT1_MIP), PixelFormatCaps::Mipmaps);
    const bool renderTarget = Has1(resource, D3D12_FORMAT_SUPPORT1_TEXTURE2D) &&
                              Has1(rtv, D3D12_FORMAT_SUPPORT1_RENDER_TARGET);
    Add(caps, renderTarget, PixelFormatCaps::RenderTarget);
    Add(caps, renderTarget && Has1(rtv, D3D12_FORMAT_SUPPORT1_BLENDABLE),
        PixelFormatCaps::Blendable);
    Add(caps, creatable && depth, PixelFormatCaps::DepthStencil);
    const bool typedUav = Has1(uav, D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW);
    Add(caps, typedUav && Has2(uav, D3D12_FORMAT_SUPPORT2_UAV_TYPED_LOAD),
        PixelFormatCaps::StorageRead);
    Add(caps, typedUav && Has2(uav, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
        PixelFormatCaps::StorageWrite);
    Add(caps, typedUav && Has2(uav, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE),
        PixelFormatCaps::StorageAtomic);
    const bool buffer = Has1(srv, D3D12_FORMAT_SUPPORT1_BUFFER) && info.planeCount == 1 && !depth;
    Add(caps, buffer, PixelFormatCaps::TexelBufferRead);
    Add(caps, buffer && typedUav && Has2(uav, D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE),
        PixelFormatCaps::TexelBufferWrite);
    Add(caps, buffer && typedUav && Has2(uav, D3D12_FORMAT_SUPPORT2_UAV_ATOMIC_EXCHANGE),
        PixelFormatCaps::TexelBufferAtomic);
    return caps;
}

uint32 QuerySampleCounts(ID3D12Device* device, PixelFormat format, PixelFormatCaps caps) noexcept {
    const FormatMapping mapping = MappingOf(format);
    DXGI_FORMAT target = DXGI_FORMAT_UNKNOWN;
    if (HasPixelFormatCaps(caps, PixelFormatCaps::RenderTarget)) {
        target = mapping.rtv;
    } else if (HasPixelFormatCaps(caps, PixelFormatCaps::DepthStencil)) {
        target = mapping.dsv;
    }

    if (device == nullptr || target == DXGI_FORMAT_UNKNOWN) {
        return 0;
    }

    uint32 counts = SampleCountBit(1);
    if (!Has1(Query(device, target), D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RENDERTARGET)) {
        return counts;
    }

    for (uint32 samples = 2; samples <= D3D12_MAX_MULTISAMPLE_SAMPLE_COUNT; samples *= 2) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels = {};
        levels.Format = target;
        levels.SampleCount = samples;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                                  sizeof(levels))) &&
            levels.NumQualityLevels > 0) {
            counts |= SampleCountBit(samples);
        }
    }

    return counts;
}

} // namespace rocks::graphics::d3d12
