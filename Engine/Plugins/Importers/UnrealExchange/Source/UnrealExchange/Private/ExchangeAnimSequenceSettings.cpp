#include <UnrealExchange/ExchangeAnimSequenceSettings.h>

#include "ExchangeMeshSettingTables.h"

// 애니메이션 클립 설정 번역 표다(Docs/Design/UnrealExchange 7.2절 4번).

namespace rocks {

namespace exchange_anim_sequence_settings {

using namespace exchange_detail;
using Kind = ExchangeSettingKind;

// UE EAdditiveAnimationType(AnimTypes.h) → AdditiveType.
constexpr ExchangeSettingRow AdditiveValues[] = {
    EnumValue("AAT_None", "None"),
    EnumValue("AAT_LocalSpaceBase", "LocalSpace"),
    EnumValue("AAT_RotationOffsetMeshSpace", "MeshSpaceRotation"),
};

// UE EAdditiveBasePoseType(AnimEnums.h) → AdditiveBasePose.
constexpr ExchangeSettingRow BasePoseValues[] = {
    EnumValue("ABPT_None", "None"),
    EnumValue("ABPT_RefPose", "RefPose"),
    EnumValue("ABPT_AnimScaled", "Scaled"),
    EnumValue("ABPT_AnimFrame", "Frame"),
    EnumValue("ABPT_LocalAnimFrame", "LocalFrame"),
};

// UE ERootMotionRootLock(AnimEnums.h) → RootMotionRootLock.
constexpr ExchangeSettingRow RootLockValues[] = {
    EnumValue("RefPose", "RefPose"),
    EnumValue("AnimFirstFrame", "FirstFrame"),
    EnumValue("Zero", "Zero"),
};

// 7.2절 4번 표 순서다(행 번호 = 배열 번호 — ExchangeAnimSequenceSettingRow).
constexpr ExchangeSettingRow AnimSequenceRows[] = {
    // 0–14: 명세로 옮김(값은 코드가 번역 문서에서 읽는다).
    Row("Skeleton", Kind::Reference),
    StructRow("AdditiveAnimType", Kind::EnumMap, AdditiveValues),
    StructRow("RefPoseType", Kind::EnumMap, BasePoseValues),
    Row("RefFrameIndex"),
    Row("RefPoseSeq", Kind::Reference),
    Row("RetargetSource"),
    Row("Interpolation", Kind::Enum),
    Row("bEnableRootMotion"),
    StructRow("RootMotionRootLock", Kind::EnumMap, RootLockValues),
    Row("bForceRootLock"),
    Row("bUseNormalizedRootMotionScale"),
    Row("RateScale"),
    Row("bLoop"),
    Row("PlatformTargetFrameRate"),
    Row("AttributeCurves"),
    // 15–39: 설정이 아님(data가 정본·파생·UE 압축·편집기).
    Row("Notifies", Kind::Ignore),
    Row("AnimNotifyTracks", Kind::Ignore),
    Row("AuthoredSyncMarkers", Kind::Ignore),
    Row("RawCurveData", Kind::Ignore),
    Row("ImportFileFramerate", Kind::Ignore),
    Row("ImportResampleFramerate", Kind::Ignore),
    Row("NumFrames", Kind::Ignore),
    Row("NumberOfKeys", Kind::Ignore),
    Row("SamplingFrameRate", Kind::Ignore),
    Row("TargetFrameRate", Kind::Ignore),
    Row("SequenceLength", Kind::Ignore),
    Row("RawDataGuid", Kind::Ignore),
    Row("AnimationTrackNames", Kind::Ignore),
    Row("RetargetSourceAssetReferencePose", Kind::Ignore),
    Row("BoneCompressionSettings", Kind::Ignore),
    Row("CurveCompressionSettings", Kind::Ignore),
    Row("VariableFrameStrippingSettings", Kind::Ignore),
    Row("bDoNotOverrideCompression", Kind::Ignore),
    Row("CompressCommandletVersion", Kind::Ignore),
    Row("bRootMotionSettingsCopiedFromMontage", Kind::Ignore),
    Row("AssetImportData", Kind::Ignore),
    Row("AssetUserData", Kind::Ignore),
    Row("ThumbnailInfo", Kind::Ignore),
    Row("PreviewPoseAsset", Kind::Ignore),
    Row("PreviewSkeletalMesh", Kind::Ignore),
    // 40–47: Rocks 필드가 없다(UE 기본값 — UAnimSequence 생성자).
    UnsupportedRow("RetargetSourceAsset", "null"),
    UnsupportedRow("bAllowFrameStripping", "true"),
    UnsupportedRow("CompressionErrorThresholdScale", "1"),
    UnsupportedRow("StripAnimDataOnDedicatedServer", R"("UseProjectSetting")"),
    UnsupportedRow("PerBoneCustomAttributeData", "[]"),
    UnsupportedRow("MetaData", "[]"),
    UnsupportedRow("ParentAsset", "null"),
    UnsupportedRow("AssetMappingTable", "null"),
};

static_assert(sizeof(AnimSequenceRows) / sizeof(AnimSequenceRows[0]) ==
                  ExchangeAnimSequenceSettingRowCount,
              "anim sequence setting rows must match ExchangeAnimSequenceSettingRowCount");

} // namespace exchange_anim_sequence_settings

Span<const ExchangeSettingRow> ExchangeAnimSequenceSettingRows() noexcept {
    using namespace exchange_anim_sequence_settings;
    return Span<const ExchangeSettingRow>(AnimSequenceRows);
}

} // namespace rocks
