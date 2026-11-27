#include <GraphicsTypes/CookSampling.h>
#include <TextureTools/TextureFormatSelect.h>

// 쿠킹 텍스처 형식 선택이다(Docs/Design/AssetSystem 12.17.5 2). 이름은 UE 형식 이름(G8·BGRA8·
// AutoDXT 등)을 그대로 쓰는 내부 단계 값이고, 마지막에 PixelFormat으로 바꾼다.
namespace rocks {

namespace {

// 유니티 빌드에서 같은 묶음의 이름(TextureBuild.cpp SetSwizzle 등)과 부딪히지 않게 이름 있는
// namespace에 둔다.
namespace format_select {

using graphics::Backend;
using graphics::BackendCount;
using graphics::GraphicsPlatform;
using graphics::PixelFormat;
using graphics::PixelFormatCaps;

// UE 형식 이름이다(Texture.cpp 4208–4227의 부분집합 — LQ·DXT5n·XGXR8·VU8은 Rocks에 없다).
enum class FormatName : uint8 {
    None,
    Dxt1,
    Dxt5,
    AutoDxt,
    Bc4,
    Bc5,
    Bc6h,
    Bc7,
    Bgra8,
    G8,
    G16,
    Rgba16f,
    Rgba32f,
    R16f,
    R32f,
};

bool IsCompressedName(FormatName name) noexcept {
    switch (name) {
    case FormatName::Dxt1:
    case FormatName::Dxt5:
    case FormatName::AutoDxt:
    case FormatName::Bc4:
    case FormatName::Bc5:
    case FormatName::Bc6h:
    case FormatName::Bc7:
        return true;
    default:
        return false;
    }
}

bool IsHdrSource(TextureSourceFormat format) noexcept {
    return format == TextureSourceFormat::RGBA16F || format == TextureSourceFormat::RGBA32F ||
           format == TextureSourceFormat::R16F || format == TextureSourceFormat::R32F;
}

bool IsHdrCompression(TextureCompressionSettings compression) noexcept {
    switch (compression) {
    case TextureCompressionSettings::HDR:
    case TextureCompressionSettings::HDR_F32:
    case TextureCompressionSettings::HDR_Compressed:
    case TextureCompressionSettings::HalfFloat:
    case TextureCompressionSettings::SingleFloat:
        return true;
    default:
        return false;
    }
}

// UE bPreventSRGB(Texture.cpp 772–777)와 ColorLookupTable 그룹(759–767)이다.
bool PreventsSrgb(const TextureFormatRequest& request) noexcept {
    return request.compression == TextureCompressionSettings::Alpha ||
           request.compression == TextureCompressionSettings::Normalmap ||
           request.compression == TextureCompressionSettings::Masks ||
           IsHdrCompression(request.compression) ||
           request.lodGroup == TextureGroup::ColorLookupTable;
}

bool IsKnownCompression(TextureCompressionSettings compression) noexcept {
    const uint32 value = static_cast<uint32>(compression);
    return value <= static_cast<uint32>(TextureCompressionSettings::HDR_F32) &&
           compression != TextureCompressionSettings::LQ &&
           compression != TextureCompressionSettings::EncodedReflectionCapture;
}

bool IsKnownSource(TextureSourceFormat format) noexcept {
    return format != TextureSourceFormat::Invalid &&
           static_cast<uint32>(format) <= static_cast<uint32>(TextureSourceFormat::R32F);
}

// CompressionSettings별 이름이다(Texture.cpp 4283–4378).
FormatName NameFor(const TextureFormatRequest& request) noexcept {
    switch (request.compression) {
    case TextureCompressionSettings::HDR:
        return FormatName::Rgba16f;
    case TextureCompressionSettings::HDR_F32:
        return FormatName::Rgba32f;
    case TextureCompressionSettings::Normalmap:
        return FormatName::Bc5;
    case TextureCompressionSettings::VectorDisplacementmap:
        return FormatName::Bgra8;
    case TextureCompressionSettings::Grayscale:
    case TextureCompressionSettings::Displacementmap:
        return request.sourceFormat == TextureSourceFormat::G16 ? FormatName::G16 : FormatName::G8;
    case TextureCompressionSettings::Alpha:
        return FormatName::Bc4;
    case TextureCompressionSettings::DistanceFieldFont:
        return FormatName::G8;
    case TextureCompressionSettings::HDR_Compressed:
        return FormatName::Bc6h;
    case TextureCompressionSettings::BC7:
        return FormatName::Bc7;
    case TextureCompressionSettings::HalfFloat:
        return FormatName::R16f;
    case TextureCompressionSettings::SingleFloat:
        return FormatName::R32f;
    case TextureCompressionSettings::Default:
    case TextureCompressionSettings::Masks:
        return request.compressionNoAlpha ? FormatName::Dxt1 : FormatName::AutoDxt;
    default:
        // EditorIcon(비압축 강제 — 아래 대체가 정한다).
        return FormatName::None;
    }
}

// 압축 이름을 쓸 수 없을 때의 비압축 이름이다(Texture.cpp 4392–4434).
FormatName UncompressedFallback(const TextureFormatRequest& request) noexcept {
    const bool grayscale = request.compression == TextureCompressionSettings::Grayscale ||
                           request.compression == TextureCompressionSettings::Displacementmap;
    if (request.compression == TextureCompressionSettings::HDR_Compressed ||
        IsHdrSource(request.sourceFormat)) {
        return FormatName::Rgba16f;
    }

    if (grayscale && request.sourceFormat == TextureSourceFormat::G16) {
        return FormatName::G16;
    }

    if (grayscale && request.sourceFormat == TextureSourceFormat::G8) {
        return FormatName::G8;
    }

    return FormatName::Bgra8;
}

// 압축을 끄는 조건이다(Texture.cpp 4237–4263). Android 볼륨은 비압축이다(12.16.2 — GLES는 ETC2·EAC
// 3D가 없고 ASTC 3D는 확장이 있어야 한다, UE의 ETC2 대체와 다르다).
bool NoCompression(const TextureFormatRequest& request,
                   const TextureFormatTarget& target) noexcept {
    if (request.compressionNone || request.compression == TextureCompressionSettings::EditorIcon ||
        request.lodGroup == TextureGroup::ColorLookupTable ||
        request.lodGroup == TextureGroup::Bokeh ||
        request.lodGroup == TextureGroup::IESLightProfile ||
        (target.platform == GraphicsPlatform::Android &&
         request.dimension == TextureDimension::Volume)) {
        return true;
    }

    // 데스크톱 대상은 압축 볼륨을 지원한다(Windows·Mac BC 3D). 상위 mip 크기 규칙은 X·Y만 본다.
    constexpr uint32 Block = 4;
    return request.topSizeX < Block || request.topSizeY < Block || request.topSizeX % Block != 0 ||
           request.topSizeY % Block != 0;
}

uint32 BackendList(uint32 mask, Backend (&out)[BackendCount]) noexcept {
    uint32 count = 0;
    for (uint32 index = 0; index < BackendCount; ++index) {
        if ((mask & (uint32{1} << index)) != 0) {
            out[count++] = static_cast<Backend>(index);
        }
    }

    return count;
}

// 대상 backend 모두가 format을 required로 보장하는지다(검증을 끈 대상은 참).
bool Guaranteed(const TextureFormatTarget& target, PixelFormat format,
                PixelFormatCaps required) noexcept {
    if (target.backends == 0) {
        return true;
    }

    Backend backends[BackendCount];
    const uint32 count = BackendList(target.backends, backends);
    const PixelFormatCaps caps = graphics::CookSamplingGuarantee(
        Span<const Backend>(backends, count), target.platform, format);
    return graphics::HasPixelFormatCaps(caps, required);
}

PixelFormat LinearFormat(FormatName name) noexcept {
    switch (name) {
    case FormatName::Dxt1:
        return PixelFormat::BC1Unorm;
    case FormatName::Dxt5:
        return PixelFormat::BC3Unorm;
    case FormatName::Bc4:
        return PixelFormat::BC4Unorm;
    case FormatName::Bc5:
        return PixelFormat::BC5Unorm;
    case FormatName::Bc6h:
        return PixelFormat::BC6HUfloat;
    case FormatName::Bc7:
        return PixelFormat::BC7Unorm;
    case FormatName::Bgra8:
        return PixelFormat::BGRA8Unorm;
    case FormatName::G8:
        return PixelFormat::R8Unorm;
    case FormatName::G16:
        return PixelFormat::R16Unorm;
    case FormatName::Rgba16f:
        return PixelFormat::RGBA16Float;
    case FormatName::Rgba32f:
        return PixelFormat::RGBA32Float;
    case FormatName::R16f:
        return PixelFormat::R16Float;
    case FormatName::R32f:
        return PixelFormat::R32Float;
    default:
        return PixelFormat::Unknown;
    }
}

// UE ini 기본 ASTC 크기 단계다(DefaultASTCQualityBySize·DefaultASTCQualityBySizeHQ — 12.17.8 3).
// 단계 0–4는 12×12·10×10·8×8·6×6·4×4다.
constexpr uint32 AstcSizeStepDefault = 3;
constexpr uint32 AstcSizeStepDefaultHq = 4;
// 노멀은 품질과 무관하게 6×6이다(UE GetQualityFormat).
constexpr uint32 AstcSizeStepNormal = 3;

PixelFormat AstcFormatOf(uint32 step) noexcept {
    constexpr PixelFormat Formats[] = {PixelFormat::ASTC12x12Unorm, PixelFormat::ASTC10x10Unorm,
                                       PixelFormat::ASTC8x8Unorm, PixelFormat::ASTC6x6Unorm,
                                       PixelFormat::ASTC4x4Unorm};
    return Formats[step];
}

// 품질 → 크기 단계다(UE GetQualityFormat — Default는 ini 기본, 그 밖은 품질 − 1).
uint32 AstcSizeStep(TextureCompressionQuality quality, bool hq) noexcept {
    if (quality == TextureCompressionQuality::Default) {
        return hq ? AstcSizeStepDefaultHq : AstcSizeStepDefault;
    }

    return static_cast<uint32>(quality) - 1;
}

// Android 재대응이다(12.17.8 6 — UE AndroidTexFormat::ASTCRemap, BC5는 NormalLA, BC6H는 ASTC HDR이
// 없어 ASTC RGB). 압축 이름이 아니면 Unknown이다.
PixelFormat AndroidFormat(FormatName name, const TextureFormatRequest& request, bool alpha,
                          TextureAstcMode& mode) noexcept {
    const uint32 step = AstcSizeStep(request.compressionQuality, false);
    switch (name) {
    case FormatName::Dxt1:
        mode = TextureAstcMode::Rgb;
        return AstcFormatOf(step);
    case FormatName::Dxt5:
        mode = alpha ? TextureAstcMode::Rgba : TextureAstcMode::Rgb;
        return AstcFormatOf(step);
    case FormatName::Bc7:
        mode = alpha ? TextureAstcMode::Rgba : TextureAstcMode::Rgb;
        return AstcFormatOf(AstcSizeStep(request.compressionQuality, true));
    case FormatName::Bc5:
        mode = TextureAstcMode::NormalLa;
        return AstcFormatOf(AstcSizeStepNormal);
    case FormatName::Bc6h:
        mode = TextureAstcMode::Rgb;
        return AstcFormatOf(step);
    case FormatName::Bc4:
        return PixelFormat::EACR11Unorm;
    default:
        return PixelFormat::Unknown;
    }
}

bool IsAstcFormat(PixelFormat format) noexcept {
    switch (format) {
    case PixelFormat::ASTC4x4Unorm:
    case PixelFormat::ASTC4x4UnormSrgb:
    case PixelFormat::ASTC6x6Unorm:
    case PixelFormat::ASTC6x6UnormSrgb:
    case PixelFormat::ASTC8x8Unorm:
    case PixelFormat::ASTC8x8UnormSrgb:
    case PixelFormat::ASTC10x10Unorm:
    case PixelFormat::ASTC10x10UnormSrgb:
    case PixelFormat::ASTC12x12Unorm:
    case PixelFormat::ASTC12x12UnormSrgb:
        return true;
    default:
        return false;
    }
}

TextureFormatEncoding EncodingOf(PixelFormat format) noexcept {
    if (IsAstcFormat(format)) {
        return TextureFormatEncoding::Astc;
    }

    switch (format) {
    case PixelFormat::EACR11Unorm:
        return TextureFormatEncoding::EacR11;
    case PixelFormat::BC1Unorm:
    case PixelFormat::BC1UnormSrgb:
        return TextureFormatEncoding::Bc1;
    case PixelFormat::BC3Unorm:
    case PixelFormat::BC3UnormSrgb:
        return TextureFormatEncoding::Bc3;
    case PixelFormat::BC4Unorm:
        return TextureFormatEncoding::Bc4;
    case PixelFormat::BC5Unorm:
        return TextureFormatEncoding::Bc5;
    case PixelFormat::BC6HUfloat:
        return TextureFormatEncoding::Bc6h;
    case PixelFormat::BC7Unorm:
    case PixelFormat::BC7UnormSrgb:
        return TextureFormatEncoding::Bc7;
    default:
        return TextureFormatEncoding::Uncompressed;
    }
}

bool IsFloatFormat(PixelFormat format) noexcept {
    return format == PixelFormat::RGBA16Float || format == PixelFormat::RGBA32Float ||
           format == PixelFormat::R16Float || format == PixelFormat::R32Float ||
           format == PixelFormat::BC6HUfloat;
}

void SetSwizzle(TextureFormatChoice& out, TextureSwizzle r, TextureSwizzle g, TextureSwizzle b,
                TextureSwizzle a) noexcept {
    out.swizzle[0] = r;
    out.swizzle[1] = g;
    out.swizzle[2] = b;
    out.swizzle[3] = a;
}

void AddWarning(TextureFormatChoice& out, AssetDiagnosticCode code, PixelFormat before) noexcept {
    if (out.warningCount < TextureFormatMaxWarnings) {
        out.warnings[out.warningCount++] =
            TextureFormatWarning{code, graphics::PixelFormatNumber(before)};
    }
}

Result<void> Select(const TextureFormatRequest& request, const TextureFormatTarget& target,
                    bool alphaTransparent, TextureFormatChoice& out) noexcept {
    out = TextureFormatChoice{};
    if (!IsKnownCompression(request.compression) || !IsKnownSource(request.sourceFormat) ||
        request.topSizeX == 0 || request.topSizeY == 0 ||
        static_cast<uint32>(request.compressionQuality) >
            static_cast<uint32>(TextureCompressionQuality::Highest) ||
        static_cast<uint32>(target.platform) >= graphics::GraphicsPlatformCount ||
        target.backends >= (uint32{1} << BackendCount)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 1. sRGB 막음.
    const bool prevented = PreventsSrgb(request);
    const bool srgb = request.srgb && !prevented;

    // 2–4. 비압축 강제, 이름 표, 비압축 대체.
    FormatName name = NameFor(request);
    if ((NoCompression(request, target) && IsCompressedName(name)) || name == FormatName::None) {
        name = UncompressedFallback(request);
    }

    // 유효 알파(ForceNoAlpha가 ForceAlpha보다 먼저 — GetTextureExpectsAlphaInPixelFormat)다.
    // AutoDXT의 형식과 BC7 프로필이 쓴다.
    const bool alpha =
        !request.compressionNoAlpha && (request.compressionForceAlpha || alphaTransparent);
    if (name == FormatName::AutoDxt) {
        name = alpha ? FormatName::Dxt5 : FormatName::Dxt1;
    }

    // 5. 마무리: G8 + sRGB → BGRA8, 32F 필터, norm16.
    if (name == FormatName::G8 && srgb) {
        name = FormatName::Bgra8;
    }

    if ((name == FormatName::R32f || name == FormatName::Rgba32f) && !request.pointFilter) {
        const PixelFormat wide = LinearFormat(name);
        // 검증을 끈 대상은 플랫폼 기본이다(UE — Android만 32F 필터를 못 한다,
        // AndroidTargetPlatformControls.cpp 500).
        const bool filterable = target.backends == 0
                                    ? target.platform != GraphicsPlatform::Android
                                    : Guaranteed(target, wide, graphics::CookSamplingCaps);
        if (!filterable) {
            name = name == FormatName::R32f ? FormatName::R16f : FormatName::Rgba16f;
        }
    }

    bool norm16Remapped = false;
    if (name == FormatName::G16 && !target.norm16Supported) {
        name = FormatName::R16f;
        norm16Remapped = true;
    }

    // PixelFormat(sRGB 짝이 있으면 그것). 7. Android는 압축 이름을 ASTC·EAC R11로 바꾼다(12.17.8 6
    // — 2–4번 비압축 강제·4의 배수 규칙 뒤, UE GetDefaultTextureFormatName 뒤 ASTCRemap과 같은
    // 순서).
    TextureAstcMode astcMode = TextureAstcMode::Rgba;
    PixelFormat format = LinearFormat(name);
    if (target.platform == GraphicsPlatform::Android && IsCompressedName(name)) {
        format = AndroidFormat(name, request, alpha, astcMode);
    }

    const PixelFormat pair = graphics::GetPixelFormatInfo(format).srgbPair;
    if (srgb && pair != PixelFormat::Unknown) {
        format = pair;
    }

    // 8. 샘플링 검증(TD12).
    const PixelFormatCaps required =
        request.pointFilter ? PixelFormatCaps::Sampled : graphics::CookSamplingCaps;
    if (!Guaranteed(target, format, required)) {
        Backend backends[BackendCount];
        const uint32 count = BackendList(target.backends, backends);
        const PixelFormat fallback = graphics::CookSamplingFallback(
            Span<const Backend>(backends, count), target.platform, format, required);
        if (fallback == PixelFormat::Unknown) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        AddWarning(out, AssetDiagnosticCode::PlatformFormatFallback, format);
        format = fallback;
    }

    // 블록 형식인데 인코더가 없는 형식(ASTC HDR·ETC2 등 — 지금 선택·대체 표가 고르지 않는다)은
    // 비압축 byte로 쓸 수 없으므로 거부한다.
    if (graphics::GetPixelFormatInfo(format).compression != graphics::PixelCompression::None &&
        EncodingOf(format) == TextureFormatEncoding::Uncompressed) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    out.pixelFormat = format;
    out.srgb = graphics::GetPixelFormatInfo(format).srgb;
    out.srgbIgnored =
        request.srgb && (prevented || (IsHdrSource(request.sourceFormat) && IsFloatFormat(format)));
    out.encoding = EncodingOf(format);
    out.alphaProfile = out.encoding == TextureFormatEncoding::Bc7 && alpha;
    out.astcMode = out.encoding == TextureFormatEncoding::Astc ? astcMode : TextureAstcMode::Rgba;
    out.norm16Remapped = norm16Remapped;

    // swizzle(sampler type 계약 — CompressionSettings로만)과 채널 복제.
    switch (request.compression) {
    case TextureCompressionSettings::Grayscale:
    case TextureCompressionSettings::Alpha:
    case TextureCompressionSettings::DistanceFieldFont:
        SetSwizzle(out, TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::R, TextureSwizzle::R);
        break;
    case TextureCompressionSettings::Displacementmap:
        SetSwizzle(out, TextureSwizzle::R, TextureSwizzle::Zero, TextureSwizzle::Zero,
                   TextureSwizzle::One);
        break;
    case TextureCompressionSettings::Normalmap:
        // 형식에 달린 예외 하나(12.17.8 6): ASTC 노멀 LA는 X = R, Y = A다.
        SetSwizzle(out, TextureSwizzle::R,
                   out.astcMode == TextureAstcMode::NormalLa ? TextureSwizzle::A
                                                             : TextureSwizzle::G,
                   TextureSwizzle::Zero, TextureSwizzle::One);
        break;
    default:
        break;
    }

    if (request.compression == TextureCompressionSettings::Grayscale ||
        request.compression == TextureCompressionSettings::Alpha) {
        out.replicate = TextureChannelReplicate::Red;
    } else if (request.compression == TextureCompressionSettings::Displacementmap ||
               request.compression == TextureCompressionSettings::DistanceFieldFont) {
        out.replicate = TextureChannelReplicate::Alpha;
    }

    return Result<void>::Success();
}

bool NeedsAlpha(const TextureFormatRequest& request, const TextureFormatTarget& target) noexcept {
    TextureFormatChoice opaque;
    TextureFormatChoice transparent;
    if (!Select(request, target, false, opaque) || !Select(request, target, true, transparent)) {
        return false;
    }

    return opaque.pixelFormat != transparent.pixelFormat ||
           opaque.alphaProfile != transparent.alphaProfile ||
           opaque.astcMode != transparent.astcMode;
}

} // namespace format_select

} // namespace

Result<void> SelectTextureFormat(const TextureFormatRequest& request,
                                 const TextureFormatTarget& target, bool alphaTransparent,
                                 TextureFormatChoice& out) noexcept {
    return format_select::Select(request, target, alphaTransparent, out);
}

bool TextureFormatNeedsAlpha(const TextureFormatRequest& request,
                             const TextureFormatTarget& target) noexcept {
    return format_select::NeedsAlpha(request, target);
}

} // namespace rocks
