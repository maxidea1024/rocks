#include "TextureImportInternal.h"

#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Texture/Texture.generated.h>
#include <TextureTools/TextureImport.h>

// 텍스처 설정 정규화다(Docs/Design/AssetSystem 12.12절 — UE Texture.cpp PostEditChangeProperty
// 866–885의 LODGroup 처리와 ValidateSettingsAfterImportOrEdit 616–797, 가상 텍스처 규칙은 그 단계로
// 미룬다).
namespace rocks {
namespace {

// UE RHIMaximumDimension(GetMaximumDimension을 GetMaximumDimensionOfNonVT 16384로 자른 값)이다 —
// Volume은 UVolumeTexture::GetMaximumDimension(GMaxVolumeTextureDimensions 2048, 12.5절 차원 최대).
constexpr int32 MaxTextureDimension = 16384;
constexpr int32 MaxVolumeTextureDimension = 2048;

constexpr StringView LodGroupField = "lodGroup";

// UE UE::TextureDefines::IsHDR(TextureCompressionSettings)다.
bool IsHdrCompression(TextureCompressionSettings compression) noexcept {
    return compression == TextureCompressionSettings::HDR ||
           compression == TextureCompressionSettings::HDR_F32 ||
           compression == TextureCompressionSettings::HDR_Compressed ||
           compression == TextureCompressionSettings::HalfFloat ||
           compression == TextureCompressionSettings::SingleFloat;
}

// value 이상의 가장 작은 2의 거듭제곱이다(value ≥ 1 — UE FMath::RoundUpToPowerOfTwo).
int64 RoundUpToPowerOfTwo(int64 value) noexcept {
    int64 power = 1;
    while (power < value) {
        power <<= 1;
    }

    return power;
}

int32 ClampDimension(int32 value, int32 maximum) noexcept {
    return value < 0 ? 0 : (value > maximum ? maximum : value);
}

Result<void> NormalizeTextureHook(const AssetTypeHookContext& context,
                                  Span<const StringView> assignedFields) noexcept {
    auto lease = context.registry.AcquireType(context.access, context.object);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    const TextureDimension dimension =
        lease.Value().IsA(TypeIdOf<Texture2D>())          ? TextureDimension::Texture2D
        : lease.Value().IsA(TypeIdOf<VolumeTexture>())    ? TextureDimension::Volume
        : lease.Value().IsA(TypeIdOf<TextureCube>())      ? TextureDimension::TextureCube
        : lease.Value().IsA(TypeIdOf<TextureCubeArray>()) ? TextureDimension::TextureCubeArray
                                                          : TextureDimension::Texture2DArray;
    auto borrowed = context.registry.ResolveForWrite<Texture>(context.access, context.object);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    NormalizeTextureSettings(*value.Value(), dimension, assignedFields);
    return Result<void>::Success();
}

} // namespace

void NormalizeTextureSettings(Texture& texture, TextureDimension dimension,
                              Span<const StringView> assignedFields) noexcept {
    const bool isTexture2D = dimension == TextureDimension::Texture2D;
    const int32 maximum =
        dimension == TextureDimension::Volume ? MaxVolumeTextureDimension : MaxTextureDimension;
    // 1. LODGroup 대입(UE PostEditChangeProperty — 그 필드를 바꿨을 때만).
    bool lodGroupAssigned = false;
    for (StringView field : assignedFields) {
        lodGroupAssigned = lodGroupAssigned || EqualsIgnoringAsciiCase(field, LodGroupField);
    }

    if (lodGroupAssigned && (texture.lodGroup == TextureGroup::EightBitData ||
                             texture.lodGroup == TextureGroup::SixteenBitData)) {
        texture.compressionSettings = texture.lodGroup == TextureGroup::EightBitData
                                          ? TextureCompressionSettings::VectorDisplacementmap
                                          : TextureCompressionSettings::HDR;
        texture.srgb = 0;
        texture.filter = TextureFilter::Default;
        texture.mipGenSettings = TextureMipGenSettings::FromTextureGroup;
    }

    // 2. 원본이 있을 때(UE Source.IsValid()).
    if (texture.source.format != TextureSourceFormat::Invalid) {
        if (texture.mipGenSettings == TextureMipGenSettings::LeaveExistingMips &&
            texture.powerOfTwoMode != TexturePowerOfTwoSetting::None) {
            texture.powerOfTwoMode = TexturePowerOfTwoSetting::None;
        }

        if (texture.powerOfTwoMode == TexturePowerOfTwoSetting::ResizeToSpecificResolution &&
            !isTexture2D) {
            texture.powerOfTwoMode = TexturePowerOfTwoSetting::None;
        }

        texture.resizeDuringBuildX = ClampDimension(texture.resizeDuringBuildX, maximum);
        texture.resizeDuringBuildY = ClampDimension(texture.resizeDuringBuildY, maximum);
        if (texture.lodGroup == TextureGroup::ColorLookupTable) {
            texture.mipGenSettings = TextureMipGenSettings::NoMipmaps;
            texture.srgb = 0;
        }
    }

    // 3. 원본과 무관하게: 선형 데이터 종류의 SRGB, legacy gamma, 최대 크기, cinematic mip.
    const TextureCompressionSettings compression = texture.compressionSettings;
    if (compression == TextureCompressionSettings::Alpha ||
        compression == TextureCompressionSettings::Normalmap ||
        compression == TextureCompressionSettings::Masks || IsHdrCompression(compression)) {
        texture.srgb = 0;
    }

    if (texture.srgb == 0) {
        texture.useLegacyGamma = 0;
    }

    if (texture.maxTextureSize <= 0) {
        texture.maxTextureSize = 0;
    } else {
        const int64 rounded = RoundUpToPowerOfTwo(texture.maxTextureSize);
        texture.maxTextureSize = static_cast<int32>(rounded < maximum ? rounded : maximum);
    }

    if (texture.numCinematicMipLevels < 0) {
        texture.numCinematicMipLevels = 0;
    }
}

Result<void> RegisterTextureTypeHooks(void* state, AssetTypeHookTable& table,
                                      Array<AssetTypeHookRegistration>& tokens) noexcept {
    (void)state;
    if (auto reserved = tokens.TryReserve(tokens.Size() + 1); !reserved) {
        return reserved;
    }

    AssetTypeHooks hooks;
    hooks.normalizeSettings = &NormalizeTextureHook;
    auto registered = table.Register(TypeIdOf<Texture>(), hooks);
    if (!registered) {
        return Result<void>::Failure(registered.Error());
    }

    (void)tokens.TryPushBack(rocks::Move(registered.Value()));
    // 원본 묶음 재구성(12.14절 A5j2).
    return texture_import::RegisterTextureSourceRebuildHooks(table, tokens);
}

} // namespace rocks
