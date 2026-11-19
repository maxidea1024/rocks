#include "GlesFormats.h"

#include <Platform/Memory.h>

namespace rocks::graphics::gles {

namespace {

using PF = PixelFormat;
using F = GlesFeature;
using R = GlesReadClass;

constexpr uint16 CR = GlesColorRenderable;
constexpr uint16 TF = GlesFilterable;
constexpr uint16 Img = GlesStorage;
constexpr uint16 Atom = GlesAtomic;
constexpr uint16 Comp = GlesCompressed;
constexpr uint16 Int = GlesInteger;

constexpr GlesTextureFormat Row(PF format, GLenum internal, GLenum external, GLenum type,
                                uint16 flags, GlesReadClass read, F sample = F::None,
                                F render = F::None, F filter = F::None,
                                F blend = F::None) noexcept {
    GlesTextureFormat row;
    row.format = format;
    row.internalFormat = internal;
    row.externalFormat = external;
    row.externalType = type;
    row.flags = flags;
    row.sampleFeature = sample;
    row.renderFeature = render;
    row.filterFeature = filter;
    row.blendFeature = blend;
    row.read = read;
    return row;
}

constexpr GlesTextureFormat Compressed(PF format, GLenum internal, F sample) noexcept {
    return Row(format, internal, 0, 0, TF | Comp, R::None, sample);
}

// 픽셀 형식 표다(PixelFormat.ko.md 8.6절, ES 3.2 Table 8.10·8.11·8.17·8.26과 확장 사양). 표에 없는
// 형식은 Unsupported다(다른 형식으로 대신하지 않는다, 8.3절):
// - BGRA8UnormSrgb: ES에 BGRA sRGB 내부 형식이 없다(UE의 R·B swizzle은 렌더 타깃 쓰기가 달라 따르지
//   않는다).
// - R64Uint: ES에 64-bit 정수 텍스처가 없다.
// - B5G5R5A1Unorm: ES에 UNSIGNED_SHORT_1_5_5_5_REV가 없어 비트 배치가 맞는 업로드 타입이 없다.
// - NV12·P010: ES core에 다중 plane 텍스처가 없다(외부 이미지 경로는 범위 밖).
constexpr GlesTextureFormat TextureFormats[] = {
    Row(PF::R8Unorm, GL_R8, GL_RED, GL_UNSIGNED_BYTE, CR | TF, R::Unorm8),
    Row(PF::R8UnormSrgb, GL_SR8_EXT, GL_RED, GL_UNSIGNED_BYTE, TF, R::None, F::SrgbR8),
    Row(PF::R8Uint, GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE, CR | Int, R::Uint),
    Row(PF::R8Sint, GL_R8I, GL_RED_INTEGER, GL_BYTE, CR | Int, R::Sint),
    // A8은 R8에 (0, 0, 0, R) 성분 대응을 건다. 렌더 타깃·storage는 swizzle을 쓰지 않으므로 주지
    // 않는다. readback은 저장소 R을 읽는다.
    Row(PF::A8Unorm, GL_R8, GL_RED, GL_UNSIGNED_BYTE, TF | GlesAlphaSwizzle, R::Unorm8),
    Row(PF::RG8Unorm, GL_RG8, GL_RG, GL_UNSIGNED_BYTE, CR | TF, R::Unorm8),
    Row(PF::RG8UnormSrgb, GL_SRG8_EXT, GL_RG, GL_UNSIGNED_BYTE, TF, R::None, F::SrgbRG8),
    Row(PF::RG8Snorm, GL_RG8_SNORM, GL_RG, GL_BYTE, TF, R::Snorm8, F::None, F::RenderSnorm),
    Row(PF::RG8Uint, GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE, CR | Int, R::Uint),
    Row(PF::RGB8Unorm, GL_RGB8, GL_RGB, GL_UNSIGNED_BYTE, CR | TF, R::Unorm8),
    Row(PF::RGB8UnormSrgb, GL_SRGB8, GL_RGB, GL_UNSIGNED_BYTE, TF, R::None),
    Row(PF::RGBA8Unorm, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, CR | TF | Img, R::Unorm8),
    Row(PF::RGBA8UnormSrgb, GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE, CR | TF, R::Unorm8),
    Row(PF::RGBA8Snorm, GL_RGBA8_SNORM, GL_RGBA, GL_BYTE, TF | Img, R::Snorm8, F::None,
        F::RenderSnorm),
    Row(PF::RGBA8Uint, GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE, CR | Int | Img, R::Uint),
    Row(PF::BGRA8Unorm, GL_BGRA8_EXT, GL_BGRA_EXT, GL_UNSIGNED_BYTE, CR | TF, R::Unorm8,
        F::TextureBgra8),
    Row(PF::R16Unorm, GL_R16_EXT, GL_RED, GL_UNSIGNED_SHORT, CR | TF, R::Unorm16, F::TextureNorm16),
    Row(PF::R16Uint, GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT, CR | Int, R::Uint),
    Row(PF::R16Sint, GL_R16I, GL_RED_INTEGER, GL_SHORT, CR | Int, R::Sint),
    Row(PF::R16Float, GL_R16F, GL_RED, GL_HALF_FLOAT, CR | TF, R::Float),
    Row(PF::RG16Unorm, GL_RG16_EXT, GL_RG, GL_UNSIGNED_SHORT, CR | TF, R::Unorm16,
        F::TextureNorm16),
    Row(PF::RG16Snorm, GL_RG16_SNORM_EXT, GL_RG, GL_SHORT, TF, R::Snorm16, F::TextureNorm16,
        F::RenderSnorm),
    Row(PF::RG16Uint, GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT, CR | Int, R::Uint),
    Row(PF::RG16Sint, GL_RG16I, GL_RG_INTEGER, GL_SHORT, CR | Int, R::Sint),
    Row(PF::RG16Float, GL_RG16F, GL_RG, GL_HALF_FLOAT, CR | TF, R::Float),
    Row(PF::RGBA16Unorm, GL_RGBA16_EXT, GL_RGBA, GL_UNSIGNED_SHORT, CR | TF, R::Unorm16,
        F::TextureNorm16),
    Row(PF::RGBA16Snorm, GL_RGBA16_SNORM_EXT, GL_RGBA, GL_SHORT, TF, R::Snorm16, F::TextureNorm16,
        F::RenderSnorm),
    Row(PF::RGBA16Uint, GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT, CR | Int | Img, R::Uint),
    Row(PF::RGBA16Sint, GL_RGBA16I, GL_RGBA_INTEGER, GL_SHORT, CR | Int | Img, R::Sint),
    Row(PF::RGBA16Float, GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, CR | TF | Img, R::Float),
    Row(PF::R32Uint, GL_R32UI, GL_RED_INTEGER, GL_UNSIGNED_INT, CR | Int | Img | Atom, R::Uint),
    Row(PF::R32Sint, GL_R32I, GL_RED_INTEGER, GL_INT, CR | Int | Img | Atom, R::Sint),
    // 32-bit float는 선형 필터에 GL_OES_texture_float_linear, 블렌드에 GL_EXT_float_blend가 든다.
    Row(PF::R32Float, GL_R32F, GL_RED, GL_FLOAT, CR | Img, R::Float, F::None, F::None,
        F::FloatLinearFilter, F::FloatBlend),
    Row(PF::RG32Uint, GL_RG32UI, GL_RG_INTEGER, GL_UNSIGNED_INT, CR | Int, R::Uint),
    Row(PF::RG32Float, GL_RG32F, GL_RG, GL_FLOAT, CR, R::Float, F::None, F::None,
        F::FloatLinearFilter, F::FloatBlend),
    Row(PF::RGB32Uint, GL_RGB32UI, GL_RGB_INTEGER, GL_UNSIGNED_INT, Int, R::None),
    Row(PF::RGB32Sint, GL_RGB32I, GL_RGB_INTEGER, GL_INT, Int, R::None),
    Row(PF::RGB32Float, GL_RGB32F, GL_RGB, GL_FLOAT, 0, R::None, F::None, F::None,
        F::FloatLinearFilter),
    Row(PF::RGBA32Uint, GL_RGBA32UI, GL_RGBA_INTEGER, GL_UNSIGNED_INT, CR | Int | Img, R::Uint),
    Row(PF::RGBA32Float, GL_RGBA32F, GL_RGBA, GL_FLOAT, CR | Img, R::Float, F::None, F::None,
        F::FloatLinearFilter, F::FloatBlend),
    Row(PF::R5G6B5Unorm, GL_RGB565, GL_RGB, GL_UNSIGNED_SHORT_5_6_5, CR | TF, R::Unorm8),
    Row(PF::RGB10A2Unorm, GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, CR | TF,
        R::Rgb10A2),
    Row(PF::RG11B10Ufloat, GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV, CR | TF,
        R::Float),
    Row(PF::RGB9E5Ufloat, GL_RGB9_E5, GL_RGB, GL_UNSIGNED_INT_5_9_9_9_REV, TF, R::None),
    // 깊이는 선형 필터를 받지 않는다(비교 모드 없이 LINEAR이면 불완전 텍스처, ES 3.2 8.17절).
    // D24·D24S8·D32FS8의 외부 배치는 장치가 plane을 엮어 만든다(UploadMip).
    Row(PF::D16Unorm, GL_DEPTH_COMPONENT16, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, 0,
        R::DepthStencil),
    Row(PF::D24Unorm, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT, GL_UNSIGNED_INT, 0,
        R::DepthStencil),
    Row(PF::D32Float, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, 0, R::DepthStencil),
    Row(PF::D24UnormS8Uint, GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, 0,
        R::DepthStencil),
    Row(PF::D32FloatS8Uint, GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL,
        GL_FLOAT_32_UNSIGNED_INT_24_8_REV, 0, R::DepthStencil),
    Compressed(PF::BC1Unorm, GL_COMPRESSED_RGBA_S3TC_DXT1_EXT, F::S3tc),
    Compressed(PF::BC1UnormSrgb, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT1_EXT, F::S3tcSrgb),
    Compressed(PF::BC2Unorm, GL_COMPRESSED_RGBA_S3TC_DXT3_EXT, F::S3tc),
    Compressed(PF::BC2UnormSrgb, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT3_EXT, F::S3tcSrgb),
    Compressed(PF::BC3Unorm, GL_COMPRESSED_RGBA_S3TC_DXT5_EXT, F::S3tc),
    Compressed(PF::BC3UnormSrgb, GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT, F::S3tcSrgb),
    Compressed(PF::BC4Unorm, GL_COMPRESSED_RED_RGTC1_EXT, F::Rgtc),
    Compressed(PF::BC5Unorm, GL_COMPRESSED_RED_GREEN_RGTC2_EXT, F::Rgtc),
    Compressed(PF::BC6HUfloat, GL_COMPRESSED_RGB_BPTC_UNSIGNED_FLOAT_EXT, F::Bptc),
    Compressed(PF::BC7Unorm, GL_COMPRESSED_RGBA_BPTC_UNORM_EXT, F::Bptc),
    Compressed(PF::BC7UnormSrgb, GL_COMPRESSED_SRGB_ALPHA_BPTC_UNORM_EXT, F::Bptc),
    Compressed(PF::ETC2RGB8Unorm, GL_COMPRESSED_RGB8_ETC2, F::None),
    Compressed(PF::ETC2RGB8UnormSrgb, GL_COMPRESSED_SRGB8_ETC2, F::None),
    Compressed(PF::ETC2RGBA8Unorm, GL_COMPRESSED_RGBA8_ETC2_EAC, F::None),
    Compressed(PF::ETC2RGBA8UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ETC2_EAC, F::None),
    Compressed(PF::EACR11Unorm, GL_COMPRESSED_R11_EAC, F::None),
    Compressed(PF::EACRG11Unorm, GL_COMPRESSED_RG11_EAC, F::None),
    // ASTC HDR은 LDR과 같은 내부 형식이고 블록 내용만 HDR이다(GL_KHR_texture_compression_astc_hdr).
    Compressed(PF::ASTC4x4Unorm, GL_COMPRESSED_RGBA_ASTC_4x4, F::AstcLdr),
    Compressed(PF::ASTC4x4UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_4x4, F::AstcLdr),
    Compressed(PF::ASTC4x4Hdr, GL_COMPRESSED_RGBA_ASTC_4x4, F::AstcHdr),
    Compressed(PF::ASTC6x6Unorm, GL_COMPRESSED_RGBA_ASTC_6x6, F::AstcLdr),
    Compressed(PF::ASTC6x6UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_6x6, F::AstcLdr),
    Compressed(PF::ASTC6x6Hdr, GL_COMPRESSED_RGBA_ASTC_6x6, F::AstcHdr),
    Compressed(PF::ASTC8x8Unorm, GL_COMPRESSED_RGBA_ASTC_8x8, F::AstcLdr),
    Compressed(PF::ASTC8x8UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_8x8, F::AstcLdr),
    Compressed(PF::ASTC8x8Hdr, GL_COMPRESSED_RGBA_ASTC_8x8, F::AstcHdr),
    Compressed(PF::ASTC10x10Unorm, GL_COMPRESSED_RGBA_ASTC_10x10, F::AstcLdr),
    Compressed(PF::ASTC10x10UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_10x10, F::AstcLdr),
    Compressed(PF::ASTC10x10Hdr, GL_COMPRESSED_RGBA_ASTC_10x10, F::AstcHdr),
    Compressed(PF::ASTC12x12Unorm, GL_COMPRESSED_RGBA_ASTC_12x12, F::AstcLdr),
    Compressed(PF::ASTC12x12UnormSrgb, GL_COMPRESSED_SRGB8_ALPHA8_ASTC_12x12, F::AstcLdr),
    Compressed(PF::ASTC12x12Hdr, GL_COMPRESSED_RGBA_ASTC_12x12, F::AstcHdr),
};

uint32 LoadU32(const uint8* bytes) noexcept {
    uint32 value = 0;
    platform::MemoryCopy(&value, bytes, sizeof(value));
    return value;
}

void StoreU16(uint8* bytes, uint16 value) noexcept {
    platform::MemoryCopy(bytes, &value, sizeof(value));
}

void StoreU32(uint8* bytes, uint32 value) noexcept {
    platform::MemoryCopy(bytes, &value, sizeof(value));
}

// float 비트를 exponentBits·mantissaBits의 작은 float로 옮긴다(signed면 부호 bit 포함). 작은
// float로 정확히 나타낼 수 있는 값(그 형식에서 읽은 값)은 비트 그대로 돌아온다. NaN·무한대도
// 보존한다. 나타낼 수 없는 값은 가장 가까운 값으로 자르지 않고 0 쪽으로 버린다(읽기 왕복 전용).
uint32 SmallFloat(uint32 bits, uint32 exponentBits, uint32 mantissaBits, bool sign) noexcept {
    const uint32 negative = sign ? (bits >> 31) : 0u;
    const uint32 exponent = (bits >> 23) & 0xFFu;
    const uint32 mantissa = bits & 0x7FFFFFu;
    const uint32 maxExponent = (1u << exponentBits) - 1u;
    const int32 bias = static_cast<int32>((1u << (exponentBits - 1u)) - 1u);
    uint32 result = 0;
    if (!sign && (bits >> 31) != 0 && (exponent != 0xFFu || mantissa == 0)) {
        result = 0;
    } else if (exponent == 0xFFu) {
        // NaN은 상위 mantissa(payload)를 그대로 두고, 그 비트가 모두 0이면 1로 둔다(NaN 유지).
        const uint32 payload = mantissa >> (23u - mantissaBits);
        result = (maxExponent << mantissaBits) | (mantissa != 0 && payload == 0 ? 1u : payload);
    } else {
        const int32 rebased = static_cast<int32>(exponent) - 127 + bias;
        if (rebased >= static_cast<int32>(maxExponent)) {
            result = maxExponent << mantissaBits;
        } else if (rebased > 0) {
            result =
                (static_cast<uint32>(rebased) << mantissaBits) | (mantissa >> (23u - mantissaBits));
        } else if (exponent != 0 && rebased > -static_cast<int32>(mantissaBits)) {
            // 작은 float의 비정규 수다(암묵 1을 더해 오른쪽으로 민다).
            const uint32 full = mantissa | 0x800000u;
            const uint32 shift = 23u - mantissaBits + static_cast<uint32>(1 - rebased);
            result = full >> shift;
        }
    }

    return result | (negative << (exponentBits + mantissaBits));
}

uint32 ChannelCount(const PixelFormatInfo& info) noexcept {
    uint32 count = 0;
    for (uint32 bit = 0; bit < 4; ++bit) {
        count += (info.channels >> bit) & 1u;
    }

    return count;
}

} // namespace

bool GlesFeatures::Has(GlesFeature feature) const noexcept {
    switch (feature) {
    case GlesFeature::None:
        return true;
    case GlesFeature::TextureNorm16:
        return textureNorm16;
    case GlesFeature::TextureBgra8:
        return textureBgra8;
    case GlesFeature::FloatLinearFilter:
        return floatLinearFilter;
    case GlesFeature::FloatBlend:
        return floatBlend;
    case GlesFeature::RenderSnorm:
        return renderSnorm;
    case GlesFeature::SrgbR8:
        return srgbR8;
    case GlesFeature::SrgbRG8:
        return srgbRG8;
    case GlesFeature::S3tc:
        return s3tc;
    case GlesFeature::S3tcSrgb:
        return s3tcSrgb;
    case GlesFeature::Rgtc:
        return rgtc;
    case GlesFeature::Bptc:
        return bptc;
    case GlesFeature::AstcLdr:
        return astcLdr;
    case GlesFeature::AstcHdr:
        return astcHdr;
    case GlesFeature::Anisotropic:
        return anisotropic;
    }

    return false;
}

GlesFeatures DetectFeatures(const GlesFunctions& gl, bool desktopContext) noexcept {
    GlesFeatures features;
    features.textureNorm16 = HasGlExtension(gl, "GL_EXT_texture_norm16");
    features.textureBgra8 = HasGlExtension(gl, "GL_EXT_texture_format_BGRA8888");
    features.floatLinearFilter = HasGlExtension(gl, "GL_OES_texture_float_linear");
    features.floatBlend = HasGlExtension(gl, "GL_EXT_float_blend");
    features.renderSnorm = HasGlExtension(gl, "GL_EXT_render_snorm");
    features.srgbR8 = HasGlExtension(gl, "GL_EXT_texture_sRGB_R8");
    features.srgbRG8 = HasGlExtension(gl, "GL_EXT_texture_sRGB_RG8");
    features.s3tc = HasGlExtension(gl, "GL_EXT_texture_compression_s3tc");
    features.s3tcSrgb = HasGlExtension(gl, "GL_EXT_texture_compression_s3tc_srgb");
    features.rgtc = HasGlExtension(gl, "GL_EXT_texture_compression_rgtc");
    features.bptc = HasGlExtension(gl, "GL_EXT_texture_compression_bptc");
    // ASTC LDR은 ES 3.2 core다. 데스크톱 core context에는 core가 아니므로 확장 이름을 요구한다.
    features.astcLdr = !desktopContext || HasGlExtension(gl, "GL_KHR_texture_compression_astc_ldr");
    features.astcHdr = HasGlExtension(gl, "GL_KHR_texture_compression_astc_hdr");
    features.anisotropic = HasGlExtension(gl, "GL_EXT_texture_filter_anisotropic");
    if (features.anisotropic) {
        gl.GetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &features.maxAnisotropy);
        features.anisotropic = features.maxAnisotropy >= 1.0f;
    }

    features.disjointTimerQuery = HasGlExtension(gl, "GL_EXT_disjoint_timer_query");
    features.srgbWriteControl = desktopContext || HasGlExtension(gl, "GL_EXT_sRGB_write_control");
    features.depthClamp = HasGlExtension(gl, "GL_EXT_depth_clamp");
    features.polygonOffsetClamp = HasGlExtension(gl, "GL_EXT_polygon_offset_clamp");
    return features;
}

const GlesTextureFormat* FindTextureFormat(PixelFormat format) noexcept {
    for (const GlesTextureFormat& entry : TextureFormats) {
        if (entry.format == format) {
            return &entry;
        }
    }

    return nullptr;
}

bool IsBufferTextureFormat(GLenum internalFormat) noexcept {
    switch (internalFormat) {
    case GL_R8:
    case GL_R8I:
    case GL_R8UI:
    case GL_RG8:
    case GL_RG8UI:
    case GL_RGBA8:
    case GL_RGBA8UI:
    case GL_R16I:
    case GL_R16UI:
    case GL_R16F:
    case GL_RG16I:
    case GL_RG16UI:
    case GL_RG16F:
    case GL_RGBA16I:
    case GL_RGBA16UI:
    case GL_RGBA16F:
    case GL_R32I:
    case GL_R32UI:
    case GL_R32F:
    case GL_RG32UI:
    case GL_RG32F:
    case GL_RGB32I:
    case GL_RGB32UI:
    case GL_RGB32F:
    case GL_RGBA32UI:
    case GL_RGBA32F:
        return true;
    default:
        return false;
    }
}

PixelFormatCaps TextureFormatCaps(const GlesTextureFormat& format,
                                  const GlesFeatures& features) noexcept {
    if (!features.Has(format.sampleFeature)) {
        return PixelFormatCaps::None;
    }

    const PixelFormatKind kind = GetPixelFormatInfo(format.format).kind;
    const bool depth = kind == PixelFormatKind::Depth || kind == PixelFormatKind::DepthStencil;
    // 2D·cube·mip은 표의 모든 형식이 된다(TexStorage2D, ES 3.2 8.18절). gather는 ES 3.1 core다.
    PixelFormatCaps caps = PixelFormatCaps::Sampled | PixelFormatCaps::Mipmaps |
                           PixelFormatCaps::TextureCube | PixelFormatCaps::Gather;
    // 3D는 비압축 색 형식만 공개한다(깊이는 3D 불가, ETC2·EAC·ASTC LDR은 3D 불가, S3TC·RGTC·BPTC·
    // ASTC HDR의 3D 조건은 확장 사양 미대조).
    if (!depth && !format.Is(GlesCompressed)) {
        caps = caps | PixelFormatCaps::Texture3D;
    }

    if (format.Is(GlesFilterable) ||
        (format.filterFeature != GlesFeature::None && features.Has(format.filterFeature))) {
        caps = caps | PixelFormatCaps::Filterable;
    }

    const bool renderTarget =
        format.Is(GlesColorRenderable) ||
        (format.renderFeature != GlesFeature::None && features.Has(format.renderFeature));
    if (renderTarget) {
        caps = caps | PixelFormatCaps::RenderTarget;
        if (!format.Is(GlesInteger) && features.Has(format.blendFeature)) {
            caps = caps | PixelFormatCaps::Blendable;
        }
    }

    if (depth) {
        caps = caps | PixelFormatCaps::DepthStencil;
    }

    if (format.Is(GlesStorage)) {
        caps = caps | PixelFormatCaps::StorageRead | PixelFormatCaps::StorageWrite;
    }

    if (format.Is(GlesAtomic)) {
        caps = caps | PixelFormatCaps::StorageAtomic;
    }

    // texel buffer(GL_TEXTURE_BUFFER)는 ES 3.2 버퍼 텍스처 내부 형식 표(Table 8.18, Khronos
    // glTexBuffer ES3 refpage와 대조)에 있는 형식만이다. R16·RG16·RGBA16 UNORM은 표에 있지만 ES
    // core 내부 형식이 아니고(GL_EXT_texture_norm16) 그 확장의 버퍼 텍스처 조건을 대조하지 않아
    // 공개하지 않는다. 쓰기(imageBuffer)는 image 형식(Table 8.26)이면서 버퍼 텍스처 형식인 것,
    // atomic은 R32UI·R32I다(RQ9). 성분 대응(swizzle)으로 흉내 내는 형식(A8)은 버퍼 텍스처에 대응을
    // 걸 수 없어 (R, 0, 0, 1)을 돌려주므로 공개하지 않는다(Vulkan의 항등 대응 규칙과 같다).
    if (IsBufferTextureFormat(format.internalFormat) && !format.Is(GlesAlphaSwizzle)) {
        caps = caps | PixelFormatCaps::TexelBufferRead;
        if (format.Is(GlesStorage)) {
            caps = caps | PixelFormatCaps::TexelBufferWrite;
        }

        if (format.Is(GlesAtomic)) {
            caps = caps | PixelFormatCaps::TexelBufferAtomic;
        }
    }

    return caps;
}

GlesReadPair CanonicalReadPair(GlesReadClass read) noexcept {
    switch (read) {
    case GlesReadClass::Unorm8:
        return {GL_RGBA, GL_UNSIGNED_BYTE, 4};
    case GlesReadClass::Snorm8:
        return {GL_RGBA, GL_BYTE, 4};
    case GlesReadClass::Unorm16:
        return {GL_RGBA, GL_UNSIGNED_SHORT, 8};
    case GlesReadClass::Snorm16:
        return {GL_RGBA, GL_SHORT, 8};
    case GlesReadClass::Uint:
        return {GL_RGBA_INTEGER, GL_UNSIGNED_INT, 16};
    case GlesReadClass::Sint:
        return {GL_RGBA_INTEGER, GL_INT, 16};
    case GlesReadClass::Float:
    case GlesReadClass::DepthStencil:
        return {GL_RGBA, GL_FLOAT, 16};
    case GlesReadClass::Rgb10A2:
        return {GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, 4};
    case GlesReadClass::None:
        break;
    }

    return {};
}

void ConvertReadTexels(const GlesTextureFormat& format, const uint8* source, uint8* destination,
                       uint64 texels) noexcept {
    const PixelFormatInfo& info = GetPixelFormatInfo(format.format);
    const GlesReadPair pair = CanonicalReadPair(format.read);
    const uint32 element = pair.texelBytes / 4u;
    const uint32 channels = ChannelCount(info);
    for (uint64 texel = 0; texel < texels; ++texel) {
        const uint8* in = source + texel * pair.texelBytes;
        uint8* out = destination + texel * info.blockBytes;
        if (format.format == PixelFormat::BGRA8Unorm) {
            out[0] = in[2];
            out[1] = in[1];
            out[2] = in[0];
            out[3] = in[3];
        } else if (format.format == PixelFormat::R5G6B5Unorm) {
            // 8-bit로 넓힌 값(round(v × 255 / 31))을 되돌린다. 간격이 1보다 넓어 정확하다.
            const uint32 red = (in[0] * 31u + 127u) / 255u;
            const uint32 green = (in[1] * 63u + 127u) / 255u;
            const uint32 blue = (in[2] * 31u + 127u) / 255u;
            StoreU16(out, static_cast<uint16>((red << 11) | (green << 5) | blue));
        } else if (format.format == PixelFormat::RG11B10Ufloat) {
            const uint32 red = SmallFloat(LoadU32(in), 5, 6, false);
            const uint32 green = SmallFloat(LoadU32(in + 4), 5, 6, false);
            const uint32 blue = SmallFloat(LoadU32(in + 8), 5, 5, false);
            StoreU32(out, red | (green << 11) | (blue << 22));
        } else if (format.read == GlesReadClass::Float && info.blockBytes / channels == 2) {
            for (uint32 channel = 0; channel < channels; ++channel) {
                StoreU16(out + channel * 2,
                         static_cast<uint16>(SmallFloat(LoadU32(in + channel * 4), 5, 10, true)));
            }
        } else {
            // 채널 고르기와 좁히기다(little-endian 하위 byte가 그 채널 값이다).
            const uint32 width = info.blockBytes / channels;
            for (uint32 channel = 0; channel < channels; ++channel) {
                platform::MemoryCopy(out + channel * width, in + channel * element, width);
            }
        }
    }
}

GlesVertexAttribute VertexAttributeFormat(VertexFormat format) noexcept {
    switch (format) {
    case VertexFormat::Float1:
        return {GL_FLOAT, 1, false, false, true};
    case VertexFormat::Float2:
        return {GL_FLOAT, 2, false, false, true};
    case VertexFormat::Float3:
        return {GL_FLOAT, 3, false, false, true};
    case VertexFormat::Float4:
        return {GL_FLOAT, 4, false, false, true};
    case VertexFormat::Byte4Snorm:
        return {GL_BYTE, 4, true, false, true};
    case VertexFormat::UByte4:
        return {GL_UNSIGNED_BYTE, 4, false, true, true};
    case VertexFormat::UByte4Unorm:
        return {GL_UNSIGNED_BYTE, 4, true, false, true};
    case VertexFormat::Bgra8Unorm:
        return {};
    case VertexFormat::Short2:
        return {GL_SHORT, 2, false, true, true};
    case VertexFormat::Short4:
        return {GL_SHORT, 4, false, true, true};
    case VertexFormat::Short2Snorm:
        return {GL_SHORT, 2, true, false, true};
    case VertexFormat::Short4Snorm:
        return {GL_SHORT, 4, true, false, true};
    case VertexFormat::Half2:
        return {GL_HALF_FLOAT, 2, false, false, true};
    case VertexFormat::Half4:
        return {GL_HALF_FLOAT, 4, false, false, true};
    case VertexFormat::UShort2:
        return {GL_UNSIGNED_SHORT, 2, false, true, true};
    case VertexFormat::UShort4:
        return {GL_UNSIGNED_SHORT, 4, false, true, true};
    case VertexFormat::UShort2Unorm:
        return {GL_UNSIGNED_SHORT, 2, true, false, true};
    case VertexFormat::UShort4Unorm:
        return {GL_UNSIGNED_SHORT, 4, true, false, true};
    case VertexFormat::Rgb10A2Unorm:
        return {GL_UNSIGNED_INT_2_10_10_10_REV, 4, true, false, true};
    case VertexFormat::UInt1:
        return {GL_UNSIGNED_INT, 1, false, true, true};
    }

    return {};
}

bool ShaderTypeOf(GLenum type, VertexShaderType& out) noexcept {
    switch (type) {
    case GL_FLOAT:
    case GL_FLOAT_VEC2:
    case GL_FLOAT_VEC3:
    case GL_FLOAT_VEC4:
        out = VertexShaderType::Float;
        return true;
    case GL_INT:
    case GL_INT_VEC2:
    case GL_INT_VEC3:
    case GL_INT_VEC4:
        out = VertexShaderType::Int;
        return true;
    case GL_UNSIGNED_INT:
    case GL_UNSIGNED_INT_VEC2:
    case GL_UNSIGNED_INT_VEC3:
    case GL_UNSIGNED_INT_VEC4:
        out = VertexShaderType::Uint;
        return true;
    default:
        return false;
    }
}

GLenum TextureTarget(TextureDimension dimension) noexcept {
    switch (dimension) {
    case TextureDimension::Texture2D:
        return GL_TEXTURE_2D;
    case TextureDimension::Texture2DArray:
        return GL_TEXTURE_2D_ARRAY;
    case TextureDimension::Texture3D:
        return GL_TEXTURE_3D;
    case TextureDimension::TextureCube:
        return GL_TEXTURE_CUBE_MAP;
    case TextureDimension::TextureCubeArray:
        return GL_TEXTURE_CUBE_MAP_ARRAY;
    }

    return GL_TEXTURE_2D;
}

GLint SwizzleParameter(SwizzleSource source) noexcept {
    switch (source) {
    case SwizzleSource::Red:
        return static_cast<GLint>(GL_RED);
    case SwizzleSource::Green:
        return static_cast<GLint>(GL_GREEN);
    case SwizzleSource::Blue:
        return static_cast<GLint>(GL_BLUE);
    case SwizzleSource::Alpha:
        return static_cast<GLint>(GL_ALPHA);
    case SwizzleSource::Zero:
        return static_cast<GLint>(GL_ZERO);
    case SwizzleSource::One:
        return static_cast<GLint>(GL_ONE);
    }

    return static_cast<GLint>(GL_ZERO);
}

// UE OpenGLState.cpp의 대응과 같다. AnisotropicPoint는 UE가 AnisotropicLinear로 넘기지만 Rocks는
// 최근접 mip(LINEAR_MIPMAP_NEAREST)에 이방성을 더한다(필터를 바꾸지 않는다).
GLint MinFilter(Filter filter) noexcept {
    switch (filter) {
    case Filter::Point:
        return static_cast<GLint>(GL_NEAREST_MIPMAP_NEAREST);
    case Filter::Bilinear:
    case Filter::AnisotropicPoint:
        return static_cast<GLint>(GL_LINEAR_MIPMAP_NEAREST);
    case Filter::Trilinear:
    case Filter::AnisotropicLinear:
        return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
    }

    return static_cast<GLint>(GL_LINEAR_MIPMAP_LINEAR);
}

GLint MagFilter(Filter filter) noexcept {
    return static_cast<GLint>(filter == Filter::Point ? GL_NEAREST : GL_LINEAR);
}

GLint WrapMode(AddressMode mode) noexcept {
    switch (mode) {
    case AddressMode::Wrap:
        return static_cast<GLint>(GL_REPEAT);
    case AddressMode::Clamp:
        return static_cast<GLint>(GL_CLAMP_TO_EDGE);
    case AddressMode::Mirror:
        return static_cast<GLint>(GL_MIRRORED_REPEAT);
    case AddressMode::Border:
        return static_cast<GLint>(GL_CLAMP_TO_BORDER);
    }

    return static_cast<GLint>(GL_REPEAT);
}

void Diagnostic::Put(char character) noexcept {
    if (length_ + 1 < sizeof(buffer_)) {
        buffer_[length_++] = character;
        buffer_[length_] = 0;
    } else {
        truncated_ = true;
    }
}

Diagnostic& Diagnostic::Text(const char* text) noexcept {
    while (text != nullptr && *text != 0) {
        Put(*text++);
    }

    return *this;
}

Diagnostic& Diagnostic::Text(StringView text) noexcept {
    for (usize index = 0; index < text.ByteCount(); ++index) {
        Put(text.Data()[index]);
    }

    return *this;
}

Diagnostic& Diagnostic::Number(uint64 value) noexcept {
    char digits[24] = {};
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);

    while (count > 0) {
        Put(digits[--count]);
    }

    return *this;
}

Diagnostic& Diagnostic::Hex(uint64 value) noexcept {
    constexpr char HexDigits[] = "0123456789ABCDEF";
    char digits[20] = {};
    usize count = 0;
    do {
        digits[count++] = HexDigits[value & 0xF];
        value >>= 4;
    } while (value != 0);

    Text("0x");
    while (count > 0) {
        Put(digits[--count]);
    }

    return *this;
}

} // namespace rocks::graphics::gles
