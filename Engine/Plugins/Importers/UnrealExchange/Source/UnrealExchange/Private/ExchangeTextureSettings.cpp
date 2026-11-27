#include <UnrealExchange/ExchangeTextureSettings.h>

// 텍스처 설정 번역 표다(Docs/Design/UnrealExchange 4.6절 3번).

namespace rocks {

namespace exchange_texture_settings {

using Kind = ExchangeSettingKind;

// 행을 만든다(모든 칸을 채운다).
constexpr ExchangeSettingRow Row(StringView key, Kind kind, uint32 classes = 0) noexcept {
    return ExchangeSettingRow{key, kind, classes, nullptr, 0, StringView(), StringView()};
}

constexpr ExchangeSettingRow StructRow(StringView key, Kind kind, const ExchangeSettingRow* members,
                                       usize count) noexcept {
    return ExchangeSettingRow{key, kind, 0, members, count, StringView(), StringView()};
}

constexpr ExchangeSettingRow UnsupportedRow(StringView key, StringView defaultValue) noexcept {
    return ExchangeSettingRow{key, Kind::Unsupported, 0, nullptr, 0, defaultValue, StringView()};
}

constexpr uint32 Texture2DBit = ExchangeClassBit(ExchangeClass::Texture2D);
constexpr uint32 Texture2DArrayBit = ExchangeClassBit(ExchangeClass::Texture2DArray);
constexpr uint32 TextureCubeArrayBit = ExchangeClassBit(ExchangeClass::TextureCubeArray);
constexpr uint32 VolumeTextureBit = ExchangeClassBit(ExchangeClass::VolumeTexture);

// UE FTextureFormatSettings(층별 형식 설정 — 계약) 멤버다.
constexpr ExchangeSettingRow LayerFormatMembers[] = {
    Row("CompressionSettings", Kind::Enum),   Row("CompressionNoAlpha", Kind::Same),
    Row("CompressionForceAlpha", Kind::Same), Row("CompressionNone", Kind::Same),
    Row("CompressionYCoCg", Kind::Same),      Row("SRGB", Kind::Same),
};

// UE FTextureSourceColorSettings 멤버다.
constexpr ExchangeSettingRow SourceColorMembers[] = {
    Row("EncodingOverride", Kind::Enum),           Row("ColorSpace", Kind::Enum),
    Row("RedChromaticityCoordinate", Kind::Same),  Row("GreenChromaticityCoordinate", Kind::Same),
    Row("BlueChromaticityCoordinate", Kind::Same), Row("WhiteChromaticityCoordinate", Kind::Same),
    Row("ChromaticAdaptationMethod", Kind::Enum),
};

constexpr usize LayerFormatMemberCount = sizeof(LayerFormatMembers) / sizeof(LayerFormatMembers[0]);
constexpr usize SourceColorMemberCount = sizeof(SourceColorMembers) / sizeof(SourceColorMembers[0]);

// 4.6절 3번 표 순서다(행 번호 = 배열 번호).
constexpr ExchangeSettingRow TextureRows[] = {
    // 0–7: 보정(계약 — 판정은 CheckTextureSettings).
    Row("AdjustBrightness", Kind::Same),
    Row("AdjustBrightnessCurve", Kind::Same),
    Row("AdjustVibrance", Kind::Same),
    Row("AdjustSaturation", Kind::Same),
    Row("AdjustRGBCurve", Kind::Same),
    Row("AdjustHue", Kind::Same),
    Row("AdjustMinAlpha", Kind::Same),
    Row("AdjustMaxAlpha", Kind::Same),
    // 8–10
    Row("CompressionNoAlpha", Kind::Same),
    Row("CompressionForceAlpha", Kind::Same),
    Row("CompressionNone", Kind::Same),
    // 11–17
    Row("LossyCompressionAmount", Kind::Enum),
    Row("OodleTextureSdkVersion", Kind::Same),
    Row("MaxTextureSize", Kind::Same),
    Row("CompressionQuality", Kind::Enum),
    Row("CompressionCacheId", Kind::Guid),
    Row("bDoScaleMipsForAlphaCoverage", Kind::Same),
    Row("AlphaCoverageThresholds", Kind::Same),
    // 18–20
    Row("bUseNewMipFilter", Kind::Same),
    Row("bPreserveBorder", Kind::Same),
    Row("bFlipGreenChannel", Kind::Same),
    // 21–28
    Row("PowerOfTwoMode", Kind::Enum),
    Row("PaddingColor", Kind::Same),
    Row("bPadWithBorderColor", Kind::Same),
    Row("ResizeDuringBuildX", Kind::Same),
    Row("ResizeDuringBuildY", Kind::Same),
    Row("bChromaKeyTexture", Kind::Same),
    Row("ChromaKeyThreshold", Kind::Same),
    Row("ChromaKeyColor", Kind::Same),
    // 29–33
    Row("MipGenSettings", Kind::Enum),
    Row("CompositeTexture", Kind::Reference),
    Row("CompositeTextureMode", Kind::Enum),
    Row("CompositePower", Kind::Same),
    StructRow("LayerFormatSettings", Kind::StructArray, LayerFormatMembers, LayerFormatMemberCount),
    // 34–45
    Row("LODBias", Kind::Same),
    Row("CompressionSettings", Kind::Enum),
    Row("Filter", Kind::Enum),
    Row("MipLoadOptions", Kind::Enum),
    Row("VirtualTextureStreamingPriority", Kind::Enum),
    Row("VirtualTexturePrefetchMips", Kind::Same),
    Row("CookPlatformTilingSettings", Kind::Enum),
    Row("bOodlePreserveExtremes", Kind::Same),
    Row("LODGroup", Kind::Enum),
    Row("Downscale", Kind::PerPlatformFloat),
    Row("DownscaleOptions", Kind::Enum),
    Row("Availability", Kind::Enum),
    // 46–49
    Row("SRGB", Kind::Same),
    Row("bNormalizeNormals", Kind::Same),
    Row("bUseLegacyGamma", Kind::Same),
    StructRow("SourceColorSettings", Kind::Struct, SourceColorMembers, SourceColorMemberCount),
    // 50–53
    Row("bNoTiling", Kind::Same),
    Row("VirtualTextureStreaming", Kind::Same),
    Row("bUseVirtualTextureStreamingPriority", Kind::Same),
    Row("CompressionYCoCg", Kind::Same),
    // 54–56: UStreamableRenderAsset.
    Row("NumCinematicMipLevels", Kind::Same),
    Row("NeverStream", Kind::Same),
    Row("bGlobalForceMipLevelsToBeResident", Kind::Same),
    // 57–64: 차원 클래스.
    Row("AddressX", Kind::Enum, Texture2DBit | Texture2DArrayBit),
    Row("AddressY", Kind::Enum, Texture2DBit | Texture2DArrayBit),
    Row("AddressZ", Kind::Enum, Texture2DArrayBit),
    Row("AddressMode", Kind::Enum, VolumeTextureBit),
    Row("SourceTextures", Kind::ReferenceArray, Texture2DArrayBit | TextureCubeArrayBit),
    Row("Source2DTexture", Kind::Reference, VolumeTextureBit),
    Row("Source2DTileSizeX", Kind::Same, VolumeTextureBit),
    Row("Source2DTileSizeY", Kind::Same, VolumeTextureBit),
    // 65–67: Rocks 필드가 없는 UE 설정(기본값이 아니면 UnsupportedSetting).
    UnsupportedRow("AssetUserData", "[]"),
    UnsupportedRow("NoRefStreamingLODBias", R"({"Default": -1, "PerQuality": []})"),
    UnsupportedRow("CPUCopyTexture", "null"),
    // 68–72: 설정이 아닌 멤버.
    Row("LightingGuid", Kind::Ignore),
    Row("AssetImportData", Kind::Ignore),
    Row("ImportedSize", Kind::Ignore),
    Row("FirstResourceMemMip", Kind::Ignore),
    Row("bHasBeenPaintedInEditor", Kind::Ignore),
};

static_assert(sizeof(TextureRows) / sizeof(TextureRows[0]) == ExchangeTextureSettingRowCount,
              "texture setting rows must match the documented table");

} // namespace exchange_texture_settings

Span<const ExchangeSettingRow> ExchangeTextureSettingRows() noexcept {
    using namespace exchange_texture_settings;
    return Span<const ExchangeSettingRow>(TextureRows);
}

} // namespace rocks
