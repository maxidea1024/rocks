#include <MeshTools/SkinnedMeshPacking.h>

#include "MeshLodPacking.h"

// 스킨 메시 쿠킹 LOD 패킹이다(Docs/Design/MeshAnimationAssets 6.7.2절 1·7). 정적과 같은 단계는
// MeshLodPacking.h다.
namespace rocks {
namespace {

using graphics::VertexFormat;

// 영향 원소 하나(4칸)의 칸 수다.
constexpr uint32 InfluencesPerElement = MeshCookedInfluencesLow;

// 영향 번호·가중치 stream 둘을 쓴다(정점마다 칸 순서 — 6.7.1절 6).
// 영향 칸이 bone map 안이고 가중치가 합 이하인지다(좁은 형식으로 자르기 전에 본다).
bool InfluencesFit(Span<const BoneInfluence> influences, usize boneMapSize, uint16 total) noexcept {
    for (const BoneInfluence& influence : influences) {
        if (influence.boneIndex >= boneMapSize || influence.weight > total) {
            return false;
        }
    }

    return true;
}

Result<void> PutInfluences(mesh_packing::PackWriter& writer, const SkinnedMeshBuildResult& built,
                           Span<const BoneInfluence> influences, uint32 vertexCount, bool wideIndex,
                           bool wideWeight, MeshRenderStream& indexStream,
                           MeshRenderStream& weightStream) noexcept {
    const uint32 slots = built.influencesPerVertex;
    const uint64 cells = uint64{vertexCount} * slots;
    if (influences.Size() != cells ||
        !InfluencesFit(influences, built.boneMap.Size(), built.weightTotal)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const VertexFormat indexFormat = wideIndex ? VertexFormat::UShort4 : VertexFormat::UByte4;
    const VertexFormat weightFormat =
        wideWeight ? VertexFormat::UShort4Unorm : VertexFormat::UByte4Unorm;
    const uint64 elements = uint64{vertexCount} * (slots / InfluencesPerElement);
    uint64 size = 0;
    if (!mesh_packing::Multiply(elements, graphics::VertexFormatSize(indexFormat), size)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto put = mesh_packing::PutStream(writer, indexStream, indexFormat, size); !put) {
        return put;
    }

    for (const BoneInfluence& influence : influences) {
        if (wideIndex) {
            writer.Put<uint16>(influence.boneIndex);
        } else {
            writer.PutByte(static_cast<uint8>(influence.boneIndex));
        }
    }

    if (!mesh_packing::Multiply(elements, graphics::VertexFormatSize(weightFormat), size)) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    if (auto put = mesh_packing::PutStream(writer, weightStream, weightFormat, size); !put) {
        return put;
    }

    for (const BoneInfluence& influence : influences) {
        if (wideWeight) {
            writer.Put<uint16>(influence.weight);
        } else {
            writer.PutByte(static_cast<uint8>(influence.weight));
        }
    }

    return Result<void>::Success();
}

void PutHalf4(mesh_packing::PackWriter& writer, const Vec3& value) noexcept {
    writer.Put<uint16>(Float16::FromFloat(value.x).bits);
    writer.Put<uint16>(Float16::FromFloat(value.y).bits);
    writer.Put<uint16>(Float16::FromFloat(value.z).bits);
    writer.Put<uint16>(Float16::FromFloat(0.0f).bits);
}

} // namespace

Result<void> PackSkinnedMeshLod(const SkinnedMeshBuildResult& built,
                                const MeshBuildSettings& settings,
                                Span<const MeshSectionInfo> sections,
                                Span<const int32> lodMaterialMap, uint32 slotCount,
                                MeshColorFormat colorFormat, Span<const Name> profileNames,
                                Span<const Name> morphNames, Array<uint8>& bytes,
                                SkinnedRenderLod& lod) noexcept {
    const StaticMeshBuildResult& mesh = built.mesh;
    if (profileNames.Size() != built.profiles.Size() ||
        morphNames.Size() != built.morphTargets.Size() ||
        (built.influencesPerVertex != MeshCookedInfluencesLow &&
         built.influencesPerVertex != MeshCookedInfluencesHigh) ||
        (built.weightTotal != SkinWeightTotal8 && built.weightTotal != BoneWeightTotal)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (mesh.indices.Size() > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    bytes.Clear();
    mesh_packing::PackWriter writer(bytes);
    lod.sections.Clear();
    lod.boneMap.Clear();
    lod.requiredBones.Clear();
    lod.morphTargets.Clear();
    lod.skinWeightProfiles.Clear();
    if (auto streams = mesh_packing::PackVertexStreams(mesh, settings, colorFormat, writer, lod);
        !streams) {
        return streams;
    }

    // 영향(기본·프로필).
    const bool wideIndex = built.boneMap.Size() > MeshBoneIndex8Limit;
    const bool wideWeight = built.weightTotal != SkinWeightTotal8;
    lod.influencesPerVertex = built.influencesPerVertex;
    if (auto put = PutInfluences(writer, built, built.influences.View(), lod.vertexCount, wideIndex,
                                 wideWeight, lod.boneIndices, lod.boneWeights);
        !put) {
        return put;
    }

    if (auto reserved = lod.skinWeightProfiles.TryReserve(built.profiles.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < built.profiles.Size(); ++index) {
        SkinWeightProfileRenderLod profile;
        profile.name = profileNames[index];
        if (auto put = PutInfluences(writer, built, built.profiles[index].influences.View(),
                                     lod.vertexCount, wideIndex, wideWeight, profile.boneIndices,
                                     profile.boneWeights);
            !put) {
            return put;
        }

        (void)lod.skinWeightProfiles.TryPushBack(profile);
    }

    // morph target(정점 uint32, Δp Half4, Δn Half4 — w = 0).
    if (auto reserved = lod.morphTargets.TryReserve(built.morphTargets.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < built.morphTargets.Size(); ++index) {
        const MorphTargetBuild& source = built.morphTargets[index];
        if (source.deltas.Size() > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        SkinnedMorphTarget target;
        target.name = morphNames[index];
        target.deltaCount = static_cast<uint32>(source.deltas.Size());
        uint64 size = 0;
        if (!mesh_packing::Multiply(source.deltas.Size(), SkinnedMorphDeltaBytes, size)) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        auto start = writer.Begin(size);
        if (!start) {
            return Result<void>::Failure(start.Error());
        }

        target.deltas = MeshRenderStream{MeshRenderStreamNone, start.Value(), size};
        for (const MorphTargetDelta& delta : source.deltas) {
            writer.Put<uint32>(delta.vertex);
            PutHalf4(writer, delta.positionDelta);
            PutHalf4(writer, delta.normalDelta);
        }

        (void)lod.morphTargets.TryPushBack(target);
    }

    if (auto packed = mesh_packing::PackIndicesAndSections(mesh, sections, writer, lod); !packed) {
        return packed;
    }

    // 섹션 materialIndex에 LOD 머티리얼 덮어쓰기를 굽는다(UE LODMaterialMap — 섹션 번호 기준, 슬롯
    // 범위 안 값만 — IsValidMaterialIndex).
    for (usize index = 0; index < lod.sections.Size() && index < lodMaterialMap.Size(); ++index) {
        if (lodMaterialMap[index] >= 0 && static_cast<uint32>(lodMaterialMap[index]) < slotCount) {
            lod.sections[index].materialIndex = static_cast<uint32>(lodMaterialMap[index]);
        }
    }

    if (auto appended = lod.boneMap.TryAppend(built.boneMap.View()); !appended) {
        return appended;
    }

    if (auto appended = lod.requiredBones.TryAppend(built.requiredBones.View()); !appended) {
        return appended;
    }

    return Result<void>::Success();
}

} // namespace rocks
