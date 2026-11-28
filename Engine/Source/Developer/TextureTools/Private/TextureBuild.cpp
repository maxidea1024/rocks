#include <Base/NumericLimits.h>
#include <Core/Math/Float16.h>
#include <DeterministicMath/DeterministicMath.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>
#include "TextureBuildSteps.h"
#include "TextureImportInternal.h"

#include <TextureTools/TextureBlockEncode.h>
#include <TextureTools/TextureBuild.h>
#include <TextureTools/TextureFormatSelect.h>
#include <TextureTools/TextureImport.h>

#include <math.h>

// 텍스처 빌드 구현이다(Docs/Design/AssetSystem 12.13절 — 규칙 12.5절·12.2절 1–5번, 출력 형식은
// 12.17.5절, UE 5.8 TextureCompressorModule.cpp와 같은 순서). 작업 픽셀은 float32 RGBA다. mip
// 평균·테두리는 UE와 같은 float 식, 재표본은 double 누적 후 float 저장이다. 곱과 합은 따로 써서
// FMA로 합쳐지지 않게 한다. 초월 함수는 DeterministicMath만 쓴다.
namespace rocks {

using graphics::PixelFormat;
namespace {

using texture_build_detail::Alpha;
using texture_build_detail::Blue;
using texture_build_detail::Channels;
using texture_build_detail::EncodeOptions;
using texture_build_detail::Green;
using texture_build_detail::Level;
using texture_build_detail::Red;
using texture_build_detail::build_encode::Clamp01NansTo0;
using texture_build_detail::build_encode::Encode;
using texture_build_detail::build_encode::HasTransparentAlpha;
using texture_build_detail::build_encode::Replicate;
using texture_build_detail::build_mips::Half;
using texture_build_detail::build_mips::NextMip;
using texture_build_detail::build_mips::NormalizeNormals;
using texture_build_detail::build_mips::RedrawBorder;
using texture_build_detail::build_resize::DecodeSource;
using texture_build_detail::build_resize::Pad;
using texture_build_detail::build_resize::Resample;
using texture_build_detail::build_resize::RoundUpPowerOfTwo32;
// 최대 크기 상한이다(12.5절 4번 — UE GetMaximumDimensionOfNonVT).
constexpr uint32 MaxDimension2D = 16384;
// 숨김 CompressionSettings다(UE TC_LQ·TC_EncodedReflectionCapture — 12.4.1절).
constexpr uint8 HiddenCompressionLq = 13;
constexpr uint8 HiddenCompressionReflection = 14;

Result<void> Fail(TextureBuildResult& out, AssetDiagnosticCode code, ErrorCode error,
                  uint64 detail) noexcept {
    out.failure.code = code;
    out.failure.error = error;
    out.failure.detail = detail;
    return Result<void>::Failure(error);
}

Result<void> Warn(TextureBuildResult& out, const TextureBuildWarning& added) noexcept {
    for (const TextureBuildWarning& warning : out.warnings) {
        if (warning.code == added.code) {
            return Result<void>::Success();
        }
    }

    if (auto pushed = out.warnings.TryPushBack(added); !pushed) {
        return Fail(out, AssetDiagnosticCode::None, pushed.Error(), 0);
    }

    return Result<void>::Success();
}

// ---- 7. 쿠킹 편향(12.5절 7번 — UE CalculateLODBias) ----

uint32 FloorLog2(uint32 value) noexcept {
    uint32 log = 0;
    while (value > 1) {
        value >>= 1;
        ++log;
    }

    return log;
}

// UE FMath::CeilLogTwo다(0과 1은 0).
int32 CeilLog2(int32 value) noexcept {
    if (value <= 1) {
        return 0;
    }

    int32 log = 0;
    int64 power = 1;
    while (power < value) {
        power <<= 1;
        ++log;
    }

    return log;
}

// UE FMath::Clamp(x < 최소면 최소, 아니면 min(x, 최대))다.
int32 UeClamp(int32 value, int32 minimum, int32 maximum) noexcept {
    return value < minimum ? minimum : (value < maximum ? value : maximum);
}

// 버릴 상위 mip 수다(W·H = 사슬 mip 0의 X·Y, chain = 사슬 길이).
uint32 CookDroppedMips(const TextureBuildSettings& settings, uint32 width, uint32 height,
                       uint32 chain) noexcept {
    if (settings.mipGenSettings == TextureMipGenSettings::NoMipmaps) {
        return 0;
    }

    const int32 textureMaxLod = static_cast<int32>(FloorLog2(width > height ? width : height));
    // textureMaxLod − bias를 int64로 구해 int32 범위로 자른다(넘침 없이).
    const int64 lowered = int64{textureMaxLod} - settings.lodBias;
    const int32 target = lowered > NumericLimits<int32>::Max()   ? NumericLimits<int32>::Max()
                         : lowered < NumericLimits<int32>::Min() ? NumericLimits<int32>::Min()
                                                                 : static_cast<int32>(lowered);
    int32 wanted = UeClamp(target, CeilLog2(settings.minLodSize), CeilLog2(settings.maxLodSize));
    wanted = UeClamp(wanted, 0, textureMaxLod);
    const uint32 dropped = static_cast<uint32>(textureMaxLod - wanted);
    return dropped < chain - 1 ? dropped : chain - 1;
}

bool IsHidden(TextureCompressionSettings compression) noexcept {
    const uint8 value = static_cast<uint8>(compression);
    return value == HiddenCompressionLq || value == HiddenCompressionReflection;
}

// 크기 계획이다(BuildTexture와 TextureBuildTopMipSize가 같은 식을 쓴다): PowerOfTwoMode 대상, 최대
// 크기를 맞춘 mip 0, 사슬 길이, 버릴 상위 mip, 쿠킹 상위 mip.
struct SizePlan {
    uint32 targetX = 0;
    uint32 targetY = 0;
    uint32 targetZ = 1;
    bool pad = false;
    uint32 fitX = 0;
    uint32 fitY = 0;
    uint32 fitZ = 1;
    uint32 chain = 1;
    uint32 dropped = 0;
    uint32 topX = 0;
    uint32 topY = 0;
};

SizePlan PlanSizes(const TextureBuildSettings& settings) noexcept {
    SizePlan plan;
    const bool volume = settings.dimension == TextureDimension::Volume;
    const TexturePowerOfTwoSetting mode = settings.powerOfTwoMode;
    const uint32 depth = volume ? settings.numSlices : 1;
    plan.targetX = settings.sizeX;
    plan.targetY = settings.sizeY;
    plan.targetZ = depth;
    plan.pad = mode == TexturePowerOfTwoSetting::PadToPowerOfTwo ||
               mode == TexturePowerOfTwoSetting::PadToSquarePowerOfTwo;
    const bool square = mode == TexturePowerOfTwoSetting::PadToSquarePowerOfTwo ||
                        mode == TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo;
    if (plan.pad || mode == TexturePowerOfTwoSetting::StretchToPowerOfTwo ||
        mode == TexturePowerOfTwoSetting::StretchToSquarePowerOfTwo) {
        plan.targetX = RoundUpPowerOfTwo32(settings.sizeX);
        plan.targetY = RoundUpPowerOfTwo32(settings.sizeY);
        plan.targetZ = volume ? RoundUpPowerOfTwo32(depth) : depth;
        if (square) {
            uint32 larger = plan.targetX > plan.targetY ? plan.targetX : plan.targetY;
            larger = volume && plan.targetZ > larger ? plan.targetZ : larger;
            plan.targetX = larger;
            plan.targetY = larger;
            plan.targetZ = volume ? larger : depth;
        }

        // Stretch·Resize는 Volume Z를 바꾸지 않는다(12.5절 3번 — UE와 다름).
        if (!plan.pad) {
            plan.targetZ = depth;
        }
    } else if (mode == TexturePowerOfTwoSetting::ResizeToSpecificResolution) {
        plan.targetX = settings.resizeX != 0 ? settings.resizeX : settings.sizeX;
        plan.targetY = settings.resizeY != 0 ? settings.resizeY : settings.sizeY;
    }

    // 4. 최대 크기(UE MaxTextureResolution — X·Y만 본다, 2×2 단계 경로는 Volume Z도 반으로).
    uint64 maximum = MaxDimension2D;
    if (settings.maxTextureSize > 0 && static_cast<uint64>(settings.maxTextureSize) < maximum) {
        maximum = static_cast<uint64>(settings.maxTextureSize);
    }

    plan.fitX = plan.targetX;
    plan.fitY = plan.targetY;
    plan.fitZ = plan.targetZ;
    const bool stepPath = volume || settings.preserveBorder;
    while (plan.fitX > maximum || plan.fitY > maximum) {
        plan.fitX = Half(plan.fitX);
        plan.fitY = Half(plan.fitY);
        plan.fitZ = stepPath && volume ? Half(plan.fitZ) : plan.fitZ;
    }

    uint32 longest = plan.fitX > plan.fitY ? plan.fitX : plan.fitY;
    longest = volume && plan.fitZ > longest ? plan.fitZ : longest;
    plan.chain =
        settings.mipGenSettings == TextureMipGenSettings::NoMipmaps ? 1 : FloorLog2(longest) + 1;
    plan.dropped = CookDroppedMips(settings, plan.fitX, plan.fitY, plan.chain);
    plan.topX = plan.fitX;
    plan.topY = plan.fitY;
    for (uint32 mip = 0; mip < plan.dropped; ++mip) {
        plan.topX = Half(plan.topX);
        plan.topY = Half(plan.topY);
    }

    return plan;
}

// settings에서 형식 선택 입력을 만든다(12.17.5 3).
TextureFormatRequest FormatRequest(const TextureBuildSettings& settings,
                                   const SizePlan& plan) noexcept {
    TextureFormatRequest request;
    request.compression = settings.compression;
    request.srgb = settings.srgb;
    request.sourceFormat = settings.format;
    request.dimension = settings.dimension;
    request.lodGroup = settings.lodGroup;
    request.compressionNone = settings.compressionNone;
    request.compressionNoAlpha = settings.compressionNoAlpha;
    request.compressionForceAlpha = settings.compressionForceAlpha;
    request.pointFilter = settings.samplerPointFilter;
    request.topSizeX = plan.topX;
    request.topSizeY = plan.topY;
    request.compressionQuality = settings.compressionQuality;
    return request;
}

TextureFormatTarget FormatTarget(const TextureBuildSettings& settings) noexcept {
    TextureFormatTarget target;
    target.platform = static_cast<graphics::GraphicsPlatform>(settings.cookPlatform);
    target.backends = settings.cookBackends;
    target.norm16Supported = settings.norm16Supported;
    return target;
}

bool ValidSize(const TextureBuildSettings& settings) noexcept {
    return settings.format != TextureSourceFormat::Invalid && settings.sizeX != 0 &&
           settings.sizeY != 0 && settings.numSlices != 0;
}

Result<void> ChooseFormat(const TextureBuildSettings& settings, bool alphaTransparent,
                          TextureFormatChoice& choice) noexcept {
    if (!ValidSize(settings) || IsHidden(settings.compression)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SizePlan plan = PlanSizes(settings);
    return SelectTextureFormat(FormatRequest(settings, plan), FormatTarget(settings),
                               alphaTransparent, choice);
}

EncodeOptions EncodeOptionsFor(const TextureBuildSettings& settings,
                               const TextureFormatChoice& choice) noexcept {
    EncodeOptions options;
    (void)settings;
    options.alphaProfile = choice.alphaProfile;
    options.astcMode = choice.astcMode;
    options.clampUnit = choice.norm16Remapped;
    return options;
}

// 작업 픽셀 byte 수가 상한 안인지다(넘침 검사 포함).
bool WithinWorkingBytes(uint64 x, uint64 y, uint64 z, uint64 layers) noexcept {
    const uint64 limit = TextureBuildMaxWorkingBytes / (Channels * sizeof(float));
    if (x == 0 || y == 0 || z == 0 || layers == 0) {
        return true;
    }

    if (x > limit || y > limit / x) {
        return false;
    }

    const uint64 plane = x * y;
    if (z > limit / plane) {
        return false;
    }

    const uint64 volume = plane * z;
    return layers <= limit / volume;
}

} // namespace

bool TextureCompressionIgnoresSrgb(TextureCompressionSettings compression) noexcept {
    // UE bPreventSRGB(Texture.cpp 772–777): Alpha·Normalmap·Masks와 HDR류(UE IsHDR)다.
    switch (compression) {
    case TextureCompressionSettings::Normalmap:
    case TextureCompressionSettings::Masks:
    case TextureCompressionSettings::Alpha:
    case TextureCompressionSettings::HDR:
    case TextureCompressionSettings::HDR_Compressed:
    case TextureCompressionSettings::HalfFloat:
    case TextureCompressionSettings::SingleFloat:
    case TextureCompressionSettings::HDR_F32:
        return true;
    default:
        return false;
    }
}

usize TextureBuildWarnings(const TextureBuildSettings& settings, bool alphaTransparent,
                           TextureBuildWarning (&out)[TextureBuildMaxWarnings]) noexcept {
    usize count = 0;
    TextureFormatChoice choice;
    const bool chosen = static_cast<bool>(ChooseFormat(settings, alphaTransparent, choice));
    if (chosen && choice.srgbIgnored) {
        out[count++] = TextureBuildWarning{AssetDiagnosticCode::SrgbIgnored, 0};
    }

    // PreserveBorder Wrap(쓰는 축 — Volume은 Z도, mip이 없으면 테두리를 다시 쓰지 않는다).
    const bool volume = settings.dimension == TextureDimension::Volume;
    if (settings.preserveBorder && settings.mipGenSettings != TextureMipGenSettings::NoMipmaps &&
        (settings.addressX == TextureAddress::Wrap || settings.addressY == TextureAddress::Wrap ||
         (volume && settings.addressZ == TextureAddress::Wrap))) {
        out[count++] = TextureBuildWarning{AssetDiagnosticCode::PreserveBorderWithWrap, 0};
    }

    for (usize index = 0; chosen && index < choice.warningCount; ++index) {
        out[count++] =
            TextureBuildWarning{choice.warnings[index].code, choice.warnings[index].detail};
    }

    return count;
}

TextureBuildOutputInfo TextureBuildOutput(const TextureBuildSettings& settings,
                                          bool alphaTransparent) noexcept {
    TextureBuildOutputInfo info;
    TextureFormatChoice choice;
    if (!ChooseFormat(settings, alphaTransparent, choice)) {
        return info;
    }

    info.pixelFormat = choice.pixelFormat;
    info.srgb = choice.srgb;
    info.encoding = choice.encoding;
    for (usize index = 0; index < 4; ++index) {
        info.swizzle[index] = choice.swizzle[index];
    }

    return info;
}

Result<void> TextureBuildTopMipSize(const TextureBuildSettings& settings, uint32& sizeX,
                                    uint32& sizeY) noexcept {
    if (!ValidSize(settings)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const SizePlan plan = PlanSizes(settings);
    sizeX = plan.topX;
    sizeY = plan.topY;
    return Result<void>::Success();
}

Result<void> ResolveTextureBuildSettings(const Texture& texture, TextureDimension dimension,
                                         const TextureLodGroupValues& group,
                                         TextureBuildSettings& out,
                                         TextureBuildResult& result) noexcept {
    result.failure = TextureBuildFailure{};
    const TextureSource& source = texture.source;
    if (source.format == TextureSourceFormat::Invalid || source.sizeX == 0 || source.sizeY == 0) {
        return Fail(result, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, 0);
    }

    const uint32 slices = source.numSlices;
    bool slicesOk = slices >= 1;
    switch (dimension) {
    case TextureDimension::Texture2D:
        slicesOk = slices == 1;
        break;
    case TextureDimension::TextureCube:
        slicesOk = slices == TextureCubeFaces;
        break;
    case TextureDimension::TextureCubeArray:
        slicesOk = slices >= TextureCubeFaces && slices % TextureCubeFaces == 0;
        break;
    default:
        break;
    }

    if (!slicesOk) {
        return Fail(result, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, slices);
    }

    if (IsHidden(texture.compressionSettings)) {
        return Fail(result, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(texture.compressionSettings));
    }

    if (texture.powerOfTwoMode == TexturePowerOfTwoSetting::ResizeToSpecificResolution &&
        dimension != TextureDimension::Texture2D) {
        return Fail(result, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(texture.powerOfTwoMode));
    }

    TextureMipGenSettings mipGen = texture.mipGenSettings;
    if (mipGen == TextureMipGenSettings::FromTextureGroup) {
        mipGen = group.mipGenSettings;
    }

    if (mipGen != TextureMipGenSettings::SimpleAverage &&
        mipGen != TextureMipGenSettings::NoMipmaps && mipGen != TextureMipGenSettings::Unfiltered) {
        return Fail(result, AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported,
                    static_cast<uint64>(mipGen));
    }

    // CPU에서 읽는 Texture2D는 mip이 없다(UE GetTextureBuildSettings).
    const bool cpuAvailable = dimension == TextureDimension::Texture2D &&
                              texture.availability == TextureAvailability::CPU;
    if (cpuAvailable) {
        mipGen = TextureMipGenSettings::NoMipmaps;
    }

    out = TextureBuildSettings{};
    out.dimension = dimension;
    out.format = source.format;
    out.sizeX = source.sizeX;
    out.sizeY = source.sizeY;
    out.numSlices = slices;
    out.compression = texture.compressionSettings;
    out.srgb = texture.srgb != 0;
    out.useLegacyGamma = texture.useLegacyGamma != 0;
    out.encodingOverride = texture.sourceColorSettings.encodingOverride;
    out.flipGreenChannel = texture.flipGreenChannel != 0;
    out.powerOfTwoMode = texture.powerOfTwoMode;
    out.paddingColor = texture.paddingColor;
    out.padWithBorderColor = texture.padWithBorderColor;
    out.resizeX =
        texture.resizeDuringBuildX > 0 ? static_cast<uint32>(texture.resizeDuringBuildX) : 0;
    out.resizeY =
        texture.resizeDuringBuildY > 0 ? static_cast<uint32>(texture.resizeDuringBuildY) : 0;
    out.mipGenSettings = mipGen;
    out.preserveBorder = texture.preserveBorder != 0;
    out.normalizeNormals = texture.normalizeNormals != 0 &&
                           texture.compressionSettings == TextureCompressionSettings::Normalmap;
    // 합은 int32 범위로 자른다(넘침 없이 — 편향은 사슬 길이로 다시 잘린다).
    const int64 bias = int64{texture.lodBias} + group.lodBias;
    out.lodBias = bias > NumericLimits<int32>::Max()   ? NumericLimits<int32>::Max()
                  : bias < NumericLimits<int32>::Min() ? NumericLimits<int32>::Min()
                                                       : static_cast<int32>(bias);
    out.maxTextureSize = texture.maxTextureSize;
    out.minLodSize = group.minLodSize;
    out.maxLodSize = group.maxLodSize;
    // CPU에서 읽는 텍스처는 압축하지 않는다(12.17.5 2 — UE는 GPU 자리표시를 BC1로 쿠킹하고 CPU
    // 사본을 따로 두지만 Rocks는 플랫폼 데이터가 CPU 사본이라 읽을 수 있어야 한다).
    out.compressionNone = texture.compressionNone != 0 || cpuAvailable;
    out.compressionNoAlpha = texture.compressionNoAlpha != 0;
    out.compressionForceAlpha = texture.compressionForceAlpha != 0;
    out.compressionQuality = texture.compressionQuality;
    out.lodGroup = texture.lodGroup;
    // 주소(12.4.2절): texture는 dimension의 클래스다(호출자 계약 — 쿠킹이 타입으로 정한다).
    switch (dimension) {
    case TextureDimension::Texture2D: {
        const auto& plane = static_cast<const Texture2D&>(texture);
        out.addressX = plane.addressX;
        out.addressY = plane.addressY;
        out.addressZ = TextureAddress::Clamp;
        break;
    }
    case TextureDimension::Texture2DArray: {
        const auto& array = static_cast<const Texture2DArray&>(texture);
        out.addressX = array.addressX;
        out.addressY = array.addressY;
        out.addressZ = array.addressZ;
        break;
    }
    case TextureDimension::Volume: {
        const auto& volume = static_cast<const VolumeTexture&>(texture);
        out.addressX = volume.addressMode;
        out.addressY = volume.addressMode;
        out.addressZ = volume.addressMode;
        break;
    }
    default:
        out.addressX = TextureAddress::Clamp;
        out.addressY = TextureAddress::Clamp;
        out.addressZ = TextureAddress::Clamp;
        break;
    }

    return Result<void>::Success();
}

// 빌드 본문이다(결정적 부동소수 환경 안에서 불린다 — BuildTexture).
static Result<void> BuildTextureBody(Allocator& allocator, const TextureBuildSettings& settings,
                                     Span<const uint8> sourcePixels, TextureBuildResult& out,
                                     const TextureBuildExecution& execution) noexcept {
    out.mips.Clear();
    out.warnings.Clear();
    out.failure = TextureBuildFailure{};
    out.pixelFormat = graphics::PixelFormat::Unknown;
    // 병렬에는 helper 작업을 할당할 scratch가 필요하다(일을 하기 전에 거부한다 — 12.17.9 3).
    if (execution.scheduler != nullptr && execution.scratch == nullptr) {
        return Fail(out, AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }
    out.srgb = false;
    const bool volume = settings.dimension == TextureDimension::Volume;
    const bool cube = settings.dimension == TextureDimension::TextureCube ||
                      settings.dimension == TextureDimension::TextureCubeArray;
    if (IsHidden(settings.compression)) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(settings.compression));
    }

    // 원본 검사(byte 수는 넘침 없이 — 각 인자가 32비트라 64비트 곱이 넘치지 않는다).
    const uint64 texelBytes = TextureSourceTexelBytes(settings.format);
    const uint64 plane = uint64{settings.sizeX} * settings.sizeY;
    const bool sized = settings.format != TextureSourceFormat::Invalid && settings.sizeX != 0 &&
                       settings.sizeY != 0 && settings.numSlices != 0 &&
                       plane <= NumericLimits<uint64>::Max() / settings.numSlices / texelBytes;
    if (!sized || plane * settings.numSlices * texelBytes != sourcePixels.Size()) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument,
                    sourcePixels.Size());
    }

    if (cube && settings.sizeX != settings.sizeY) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument,
                    settings.sizeX);
    }

    // 직접 채운 설정도 Resolve와 같은 규칙을 지킨다(slice 수·mipGen 셋).
    const uint32 slices = settings.numSlices;
    const bool slicesOk =
        settings.dimension == TextureDimension::Texture2D          ? slices == 1
        : settings.dimension == TextureDimension::TextureCube      ? slices == TextureCubeFaces
        : settings.dimension == TextureDimension::TextureCubeArray ? slices % TextureCubeFaces == 0
                                                                   : true;
    if (!slicesOk) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::InvalidArgument, slices);
    }

    if (settings.mipGenSettings != TextureMipGenSettings::SimpleAverage &&
        settings.mipGenSettings != TextureMipGenSettings::NoMipmaps &&
        settings.mipGenSettings != TextureMipGenSettings::Unfiltered) {
        return Fail(out, AssetDiagnosticCode::UnsupportedSetting, ErrorCode::Unsupported,
                    static_cast<uint64>(settings.mipGenSettings));
    }

    // PowerOfTwoMode 대상 크기(UE GetPowerOfTwoTargetTextureSize — 작업 byte를 먼저 판정한다).
    const uint32 depth = volume ? settings.numSlices : 1;
    const uint32 layers = volume ? 1 : settings.numSlices;
    const SizePlan plan = PlanSizes(settings);
    const uint32 targetX = plan.targetX;
    const uint32 targetY = plan.targetY;
    const uint32 targetZ = plan.targetZ;
    const bool pad = plan.pad;
    if (cube && targetX != targetY) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    static_cast<uint64>(settings.powerOfTwoMode));
    }

    const uint32 widestX = targetX > settings.sizeX ? targetX : settings.sizeX;
    const uint32 widestY = targetY > settings.sizeY ? targetY : settings.sizeY;
    const uint32 deepest = targetZ > depth ? targetZ : depth;
    if (!WithinWorkingBytes(widestX, widestY, deepest, layers)) {
        return Fail(out, AssetDiagnosticCode::InvalidSource, ErrorCode::CapacityExceeded,
                    sourcePixels.Size());
    }

    // 형식 선택 입력이 맞는지 먼저 본다(대상 backend의 보장 대체가 없으면 InvalidSetting — TD12).
    const TextureFormatRequest request = FormatRequest(settings, plan);
    const TextureFormatTarget target = FormatTarget(settings);
    TextureFormatChoice choice;
    if (!SelectTextureFormat(request, target, false, choice)) {
        return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                    settings.cookBackends);
    }

    // 1. 선형화.
    Level level(allocator);
    if (auto decoded = DecodeSource(settings, sourcePixels, level); !decoded) {
        return Fail(out, AssetDiagnosticCode::None, decoded.Error(), 0);
    }

    // 알파 판정(AutoDXT·BC7일 때만 — TextureFormatNeedsAlpha): 원본의 모든
    // slice(PowerOfTwoMode·크기 조정 전)다 — UE 주 경로의 원본 층
    // ColorMin·ColorMax(TextureDerivedData.cpp 886–894 → DetermineAlphaChannelTransparency)와 같은
    // 대상이다(12.17.5 3).
    const bool alphaTransparent =
        TextureFormatNeedsAlpha(request, target) && HasTransparentAlpha(level);

    // 3. PowerOfTwoMode.
    if (targetX != level.width || targetY != level.height || targetZ != level.depth) {
        Result<void> changed = Result<void>::Success();
        if (pad) {
            // 채움 색은 8비트 sRGB로 디코딩한다(UE FLinearColor(FColor) — 원본 형식과 무관).
            const Color color = settings.paddingColor;
            const float fill[Channels] = {deterministic::SrgbByteToLinear(color.r),
                                          deterministic::SrgbByteToLinear(color.g),
                                          deterministic::SrgbByteToLinear(color.b),
                                          static_cast<float>(color.a) * (1.0f / 255.0f)};
            changed =
                Pad(allocator, level, targetX, targetY, targetZ, settings.padWithBorderColor, fill);
        } else {
            changed = Resample(allocator, level, targetX, targetY);
        }

        if (!changed) {
            return Fail(out, AssetDiagnosticCode::None, changed.Error(), 0);
        }
    }

    const bool unfiltered = settings.mipGenSettings == TextureMipGenSettings::Unfiltered;
    // 4. 최대 크기(UE MaxTextureResolution — X·Y만 본다).
    uint64 maximum = MaxDimension2D;
    if (settings.maxTextureSize > 0 && static_cast<uint64>(settings.maxTextureSize) < maximum) {
        maximum = static_cast<uint64>(settings.maxTextureSize);
    }

    if (level.width > maximum || level.height > maximum) {
        uint32 fitX = level.width;
        uint32 fitY = level.height;
        while (fitX > maximum || fitY > maximum) {
            fitX = Half(fitX);
            fitY = Half(fitY);
        }

        if (!volume && !settings.preserveBorder) {
            if (auto resized = Resample(allocator, level, fitX, fitY); !resized) {
                return Fail(out, AssetDiagnosticCode::None, resized.Error(), 0);
            }
        } else {
            while (level.width > maximum || level.height > maximum) {
                Level next(allocator);
                if (auto made = NextMip(level, volume, unfiltered, next); !made) {
                    return Fail(out, AssetDiagnosticCode::None, made.Error(), 0);
                }

                if (settings.preserveBorder) {
                    RedrawBorder(level, volume, next);
                }

                level = rocks::Move(next);
            }
        }
    }

    // flipGreen(UE — Pad·Stretch·최대 크기를 거친 mip 0에서 G = 1 − Clamp(G, 0, 1)).
    if (settings.flipGreenChannel) {
        for (usize index = 0; index < level.texels.Size(); index += Channels) {
            level.texels[index + Green] = 1.0f - Clamp01NansTo0(level.texels[index + Green]);
        }
    }

    const bool noMips = settings.mipGenSettings == TextureMipGenSettings::NoMipmaps;

    // 5. mip 사슬(Volume은 Z 포함 — NoMipmaps는 mip 0만).
    uint32 longest = level.width > level.height ? level.width : level.height;
    longest = volume && level.depth > longest ? level.depth : longest;
    const uint32 chain = noMips ? 1 : FloorLog2(longest) + 1;
    const uint32 dropped = CookDroppedMips(settings, level.width, level.height, chain);
    // 크기 계획과 실제 사슬이 같아야 한다(상위 mip 크기 규칙이 이 값으로 형식을 골랐다).
    if (chain != plan.chain || dropped != plan.dropped || level.width != plan.fitX ||
        level.height != plan.fitY) {
        return Fail(out, AssetDiagnosticCode::None, ErrorCode::InvalidArgument, 0);
    }
    if (auto reserved = out.mips.TryReserve(chain - dropped); !reserved) {
        return Fail(out, AssetDiagnosticCode::None, reserved.Error(), 0);
    }

    Array<Level> mips(allocator);
    if (auto reserved = mips.TryReserve(chain); !reserved) {
        return Fail(out, AssetDiagnosticCode::None, reserved.Error(), 0);
    }

    (void)mips.TryPushBack(rocks::Move(level));
    for (uint32 mip = 1; mip < chain; ++mip) {
        Level next(allocator);
        if (auto made = NextMip(mips[mip - 1], volume, unfiltered, next); !made) {
            return Fail(out, AssetDiagnosticCode::None, made.Error(), 0);
        }

        if (settings.preserveBorder) {
            RedrawBorder(mips[mip - 1], volume, next);
        }

        (void)mips.TryPushBack(rocks::Move(next));
    }

    // 6. NormalizeNormals(사슬을 다 만든 뒤 mip 0 포함 — UE NormalizeMip).
    if (settings.normalizeNormals) {
        for (Level& mip : mips) {
            NormalizeNormals(mip);
        }
    }

    // 6b. 채널 복제(UE — NormalizeNormals 뒤 모든 mip, 12.17절 TD6).
    for (Level& mip : mips) {
        Replicate(mip, choice.replicate);
    }

    // 6c. 원본 알파 판정 결과로 형식을 다시 고른다(AutoDXT 형식·BC7 프로필).
    if (alphaTransparent) {
        if (!SelectTextureFormat(request, target, alphaTransparent, choice)) {
            return Fail(out, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                        settings.cookBackends);
        }
    }

    TextureBuildWarning warnings[TextureBuildMaxWarnings];
    const usize warningCount = TextureBuildWarnings(settings, alphaTransparent, warnings);
    for (usize index = 0; index < warningCount; ++index) {
        if (auto warned = Warn(out, warnings[index]); !warned) {
            return warned;
        }
    }

    // 7·8. 쿠킹 편향과 출력.
    EncodeOptions options = EncodeOptionsFor(settings, choice);
    options.execution = execution;

    for (uint32 mip = dropped; mip < chain; ++mip) {
        const Level& current = mips[mip];
        TextureBuiltMip built(allocator);
        built.sizeX = current.width;
        built.sizeY = current.height;
        built.sizeZ = volume ? current.depth : 1;
        if (auto encoded = Encode(allocator, current, choice, options, built.bytes); !encoded) {
            return Fail(out, AssetDiagnosticCode::None, encoded.Error(), 0);
        }

        (void)out.mips.TryPushBack(rocks::Move(built));
    }

    out.pixelFormat = choice.pixelFormat;
    out.srgb = choice.srgb;
    for (usize index = 0; index < 4; ++index) {
        out.swizzle[index] = choice.swizzle[index];
    }

    out.alphaTransparent = alphaTransparent;
    out.numSlices = volume ? 1 : settings.numSlices;
    return Result<void>::Success();
}

namespace {

// 유니티 빌드에서 같은 묶음의 이름과 부딪히지 않게 이름 있는 namespace에 둔다.
namespace build_environment {

struct Job {
    Allocator* allocator;
    const TextureBuildSettings* settings;
    Span<const uint8> pixels;
    TextureBuildResult* out;
    const TextureBuildExecution* execution;
    Result<void> result;
};

void Run(void* opaque) noexcept {
    Job& job = *static_cast<Job*>(opaque);
    job.result =
        BuildTextureBody(*job.allocator, *job.settings, job.pixels, *job.out, *job.execution);
}

} // namespace build_environment

} // namespace

// 원본 decode·재표본·mip 생성·변환의 float·double 연산도 결정적 부동소수 환경 안에서 한다(12.17.9 6
// — 호출자의 FTZ·DAZ·반올림 설정이 결과를 바꾸지 않게. 인코더 batch는 따로 같은 환경을 건다).
Result<void> BuildTexture(Allocator& allocator, const TextureBuildSettings& settings,
                          Span<const uint8> sourcePixels, TextureBuildResult& out,
                          const TextureBuildExecution& execution) noexcept {
    build_environment::Job job{&allocator, &settings,  sourcePixels,
                               &out,       &execution, Result<void>::Success()};
    platform::RunWithDeterministicFloatingPoint(&build_environment::Run, &job);
    return rocks::Move(job.result);
}

namespace texture_build_detail {

bool WithinTextureWorkingBytes(uint64 x, uint64 y, uint64 z, uint64 layers) noexcept {
    return WithinWorkingBytes(x, y, z, layers);
}

} // namespace texture_build_detail

} // namespace rocks
