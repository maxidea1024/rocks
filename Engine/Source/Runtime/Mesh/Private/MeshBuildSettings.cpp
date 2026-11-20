#include <Mesh/MeshBuildSettings.h>
#include <Platform/Memory.h>

// 빌드 설정 필드 번호의 이름·상태·기본값 비교다(Docs/Design/MeshAnimationAssets.ko.md 4.1절).
namespace rocks {

namespace {

// 두 값의 비트가 같은지다(-0.0과 +0.0도 가른다 — "기본값이 아님"을 저장 byte로 판정한다).
template <class T>
bool SameBits(const T& left, const T& right) noexcept {
    return platform::MemoryEquals(&left, &right, sizeof(T));
}

} // namespace

const char* MeshBuildSettingFieldName(MeshBuildSettingField field) noexcept {
    switch (field) {
    case MeshBuildSettingField::RecomputeNormals:
        return "recomputeNormals";
    case MeshBuildSettingField::RecomputeTangents:
        return "recomputeTangents";
    case MeshBuildSettingField::UseMikkTSpace:
        return "useMikkTSpace";
    case MeshBuildSettingField::ComputeWeightedNormals:
        return "computeWeightedNormals";
    case MeshBuildSettingField::RemoveDegenerates:
        return "removeDegenerates";
    case MeshBuildSettingField::BuildReversedIndexBuffer:
        return "buildReversedIndexBuffer";
    case MeshBuildSettingField::UseHighPrecisionTangentBasis:
        return "useHighPrecisionTangentBasis";
    case MeshBuildSettingField::UseFullPrecisionUVs:
        return "useFullPrecisionUVs";
    case MeshBuildSettingField::GenerateLightmapUVs:
        return "generateLightmapUVs";
    case MeshBuildSettingField::MinLightmapResolution:
        return "minLightmapResolution";
    case MeshBuildSettingField::SrcLightmapIndex:
        return "srcLightmapIndex";
    case MeshBuildSettingField::DstLightmapIndex:
        return "dstLightmapIndex";
    case MeshBuildSettingField::BuildScale:
        return "buildScale";
    case MeshBuildSettingField::GenerateDistanceFieldAsIfTwoSided:
        return "generateDistanceFieldAsIfTwoSided";
    case MeshBuildSettingField::DistanceFieldResolutionScale:
        return "distanceFieldResolutionScale";
    case MeshBuildSettingField::MaxLumenMeshCards:
        return "maxLumenMeshCards";
    case MeshBuildSettingField::SupportFaceRemap:
        return "supportFaceRemap";
    }

    return "Unknown";
}

bool IsPreservedMeshBuildSetting(MeshBuildSettingField field) noexcept {
    switch (field) {
    case MeshBuildSettingField::BuildReversedIndexBuffer:
    case MeshBuildSettingField::GenerateLightmapUVs:
    case MeshBuildSettingField::MinLightmapResolution:
    case MeshBuildSettingField::SrcLightmapIndex:
    case MeshBuildSettingField::DstLightmapIndex:
    case MeshBuildSettingField::GenerateDistanceFieldAsIfTwoSided:
    case MeshBuildSettingField::DistanceFieldResolutionScale:
    case MeshBuildSettingField::MaxLumenMeshCards:
    case MeshBuildSettingField::SupportFaceRemap:
        return true;
    case MeshBuildSettingField::RecomputeNormals:
    case MeshBuildSettingField::RecomputeTangents:
    case MeshBuildSettingField::UseMikkTSpace:
    case MeshBuildSettingField::ComputeWeightedNormals:
    case MeshBuildSettingField::RemoveDegenerates:
    case MeshBuildSettingField::UseHighPrecisionTangentBasis:
    case MeshBuildSettingField::UseFullPrecisionUVs:
    case MeshBuildSettingField::BuildScale:
        return false;
    }

    return false;
}

bool IsMeshBuildSettingChanged(const MeshBuildSettings& settings,
                               MeshBuildSettingField field) noexcept {
    const MeshBuildSettings defaults;
    switch (field) {
    case MeshBuildSettingField::RecomputeNormals:
        return settings.recomputeNormals != defaults.recomputeNormals;
    case MeshBuildSettingField::RecomputeTangents:
        return settings.recomputeTangents != defaults.recomputeTangents;
    case MeshBuildSettingField::UseMikkTSpace:
        return settings.useMikkTSpace != defaults.useMikkTSpace;
    case MeshBuildSettingField::ComputeWeightedNormals:
        return settings.computeWeightedNormals != defaults.computeWeightedNormals;
    case MeshBuildSettingField::RemoveDegenerates:
        return settings.removeDegenerates != defaults.removeDegenerates;
    case MeshBuildSettingField::BuildReversedIndexBuffer:
        return settings.buildReversedIndexBuffer != defaults.buildReversedIndexBuffer;
    case MeshBuildSettingField::UseHighPrecisionTangentBasis:
        return settings.useHighPrecisionTangentBasis != defaults.useHighPrecisionTangentBasis;
    case MeshBuildSettingField::UseFullPrecisionUVs:
        return settings.useFullPrecisionUVs != defaults.useFullPrecisionUVs;
    case MeshBuildSettingField::GenerateLightmapUVs:
        return settings.generateLightmapUVs != defaults.generateLightmapUVs;
    case MeshBuildSettingField::MinLightmapResolution:
        return settings.minLightmapResolution != defaults.minLightmapResolution;
    case MeshBuildSettingField::SrcLightmapIndex:
        return settings.srcLightmapIndex != defaults.srcLightmapIndex;
    case MeshBuildSettingField::DstLightmapIndex:
        return settings.dstLightmapIndex != defaults.dstLightmapIndex;
    case MeshBuildSettingField::BuildScale:
        return !SameBits(settings.buildScale.x, defaults.buildScale.x) ||
               !SameBits(settings.buildScale.y, defaults.buildScale.y) ||
               !SameBits(settings.buildScale.z, defaults.buildScale.z);
    case MeshBuildSettingField::GenerateDistanceFieldAsIfTwoSided:
        return settings.generateDistanceFieldAsIfTwoSided !=
               defaults.generateDistanceFieldAsIfTwoSided;
    case MeshBuildSettingField::DistanceFieldResolutionScale:
        return !SameBits(settings.distanceFieldResolutionScale,
                         defaults.distanceFieldResolutionScale);
    case MeshBuildSettingField::MaxLumenMeshCards:
        return settings.maxLumenMeshCards != defaults.maxLumenMeshCards;
    case MeshBuildSettingField::SupportFaceRemap:
        return settings.supportFaceRemap != defaults.supportFaceRemap;
    }

    return false;
}

} // namespace rocks
