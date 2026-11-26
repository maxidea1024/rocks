#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>

#include "../Common/MeshRenderLodChecks.h"

// 스킨 메시 로드 검증과 확장 bounds다(Docs/Design/MeshAnimationAssets 6.7.1절 7). 공통 LOD 검사는
// MeshRenderLodChecks.h다.
namespace rocks {
namespace {

using graphics::VertexFormat;

// 영향 원소 하나(4칸)의 칸 수다.
constexpr uint32 InfluencesPerElement = MeshCookedInfluencesLow;

bool IsValidInfluenceCount(uint32 count) noexcept {
    return count == MeshCookedInfluencesLow || count == MeshCookedInfluencesHigh;
}

// 진단에 target·프로필 번호를 담는다(attribute — 기본 영향은 MeshNoAttribute).
Result<void> CorruptAt(MeshDiagnostic* failure, MeshIssue issue, uint32 lod, uint64 count,
                       uint32 attribute) noexcept {
    auto failed = mesh_render_checks::Corrupt(failure, issue, lod, count);
    if (failure != nullptr) {
        failure->attribute = attribute;
    }

    return failed;
}

// bone map 크기로 정해지는 영향 번호 형식이다(6.3절).
VertexFormat ExpectedIndexFormat(usize boneMapSize) noexcept {
    return boneMapSize <= MeshBoneIndex8Limit ? VertexFormat::UByte4 : VertexFormat::UShort4;
}

bool IsWeightFormat(uint32 number) noexcept {
    VertexFormat format = VertexFormat::Float1;
    return mesh_render_checks::DecodeFormat(number, format) &&
           (format == VertexFormat::UByte4Unorm || format == VertexFormat::UShort4Unorm);
}

// 스킨 stream hook이다(공통 형식·크기·구간 검사 사이에 끼운다 — MeshRenderLodChecks.h).
class SkinnedStreams {
public:
    SkinnedStreams(const SkinnedRenderLod& lod, uint32 index, MeshDiagnostic* failure) noexcept
        : lod_(lod), index_(index), failure_(failure) {
    }

    Result<void> Formats() const noexcept {
        if (!IsValidInfluenceCount(lod_.influencesPerVertex)) {
            return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderInfluenceLayout, index_);
        }

        const uint32 indexFormat =
            graphics::VertexFormatNumber(ExpectedIndexFormat(lod_.boneMap.Size()));
        if (lod_.boneIndices.vertexFormat != indexFormat ||
            !IsWeightFormat(lod_.boneWeights.vertexFormat)) {
            return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderInfluenceLayout, index_);
        }

        for (usize profile = 0; profile < lod_.skinWeightProfiles.Size(); ++profile) {
            const SkinWeightProfileRenderLod& value = lod_.skinWeightProfiles[profile];
            if (value.boneIndices.vertexFormat != indexFormat ||
                value.boneWeights.vertexFormat != lod_.boneWeights.vertexFormat) {
                return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderSkinWeightProfile,
                                                   index_, profile);
            }
        }

        for (usize target = 0; target < lod_.morphTargets.Size(); ++target) {
            if (lod_.morphTargets[target].deltas.vertexFormat != MeshRenderStreamNone) {
                return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderMorphTarget, index_,
                                                   target);
            }
        }

        return Result<void>::Success();
    }

    Result<void> Sizes() const noexcept {
        const uint64 elements = lod_.influencesPerVertex / InfluencesPerElement;
        const uint64 indexBytes = graphics::VertexFormatSize(
            graphics::VertexFormatFromNumber(lod_.boneIndices.vertexFormat).Value());
        const uint64 weightBytes = graphics::VertexFormatSize(
            graphics::VertexFormatFromNumber(lod_.boneWeights.vertexFormat).Value());
        const uint64 vertices = lod_.vertexCount;
        if (!mesh_render_checks::SizeMatches(lod_.boneIndices, vertices, indexBytes, elements) ||
            !mesh_render_checks::SizeMatches(lod_.boneWeights, vertices, weightBytes, elements)) {
            return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderStreamSize, index_);
        }

        for (usize profile = 0; profile < lod_.skinWeightProfiles.Size(); ++profile) {
            const SkinWeightProfileRenderLod& value = lod_.skinWeightProfiles[profile];
            if (value.boneIndices.byteSize != lod_.boneIndices.byteSize ||
                value.boneWeights.byteSize != lod_.boneWeights.byteSize) {
                return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderSkinWeightProfile,
                                                   index_, profile);
            }
        }

        for (usize target = 0; target < lod_.morphTargets.Size(); ++target) {
            const SkinnedMorphTarget& value = lod_.morphTargets[target];
            if (!mesh_render_checks::SizeMatches(value.deltas, value.deltaCount,
                                                 SkinnedMorphDeltaBytes, 1)) {
                return mesh_render_checks::Corrupt(failure_, MeshIssue::RenderMorphTarget, index_,
                                                   target);
            }
        }

        return Result<void>::Success();
    }

    bool Walk(mesh_render_checks::StreamWalk& walk) const noexcept {
        if (!walk.Next(lod_.boneIndices) || !walk.Next(lod_.boneWeights)) {
            return false;
        }

        for (const SkinWeightProfileRenderLod& value : lod_.skinWeightProfiles) {
            if (!walk.Next(value.boneIndices) || !walk.Next(value.boneWeights)) {
                return false;
            }
        }

        for (const SkinnedMorphTarget& value : lod_.morphTargets) {
            if (!walk.Next(value.deltas)) {
                return false;
            }
        }

        return true;
    }

private:
    const SkinnedRenderLod& lod_;
    uint32 index_;
    MeshDiagnostic* failure_;
};

// data가 빈 LOD는 스킨 stream도 모두 비고 influencesPerVertex가 0이다.
Result<void> ValidateStreamlessSkin(const SkinnedRenderLod& lod, uint32 index,
                                    MeshDiagnostic* failure) noexcept {
    if (lod.influencesPerVertex != 0 || !mesh_render_checks::IsEmptyStream(lod.boneIndices) ||
        !mesh_render_checks::IsEmptyStream(lod.boneWeights)) {
        return mesh_render_checks::Corrupt(failure, MeshIssue::RenderInfluenceLayout, index);
    }

    for (usize profile = 0; profile < lod.skinWeightProfiles.Size(); ++profile) {
        const SkinWeightProfileRenderLod& value = lod.skinWeightProfiles[profile];
        if (!mesh_render_checks::IsEmptyStream(value.boneIndices) ||
            !mesh_render_checks::IsEmptyStream(value.boneWeights)) {
            return mesh_render_checks::Corrupt(failure, MeshIssue::RenderSkinWeightProfile, index,
                                               profile);
        }
    }

    for (usize target = 0; target < lod.morphTargets.Size(); ++target) {
        if (lod.morphTargets[target].deltaCount != 0 ||
            !mesh_render_checks::IsEmptyStream(lod.morphTargets[target].deltas)) {
            return mesh_render_checks::Corrupt(failure, MeshIssue::RenderMorphTarget, index,
                                               target);
        }
    }

    return Result<void>::Success();
}

// list가 엄격 증가하고 모두 < boneCount인지다.
Result<void> ValidateBoneList(const Array<uint16>& list, usize boneCount, uint32 index,
                              MeshDiagnostic* failure) noexcept {
    for (usize position = 0; position < list.Size(); ++position) {
        if (list[position] >= boneCount || (position > 0 && list[position] <= list[position - 1])) {
            return mesh_render_checks::Corrupt(failure, MeshIssue::RenderBoneMap, index, position);
        }
    }

    return Result<void>::Success();
}

// 영향 stream의 번호 < bone map 크기, 정점마다 가중치 합이 형식의 합인지다.
Result<void> ValidateInfluenceValues(const SkinnedRenderLod& lod, const MeshRenderStream& indices,
                                     const MeshRenderStream& weights, Span<const uint8> data,
                                     uint32 lodIndex, uint32 attribute,
                                     MeshDiagnostic* failure) noexcept {
    const bool wideIndex =
        indices.vertexFormat == graphics::VertexFormatNumber(VertexFormat::UShort4);
    const bool wideWeight =
        weights.vertexFormat == graphics::VertexFormatNumber(VertexFormat::UShort4Unorm);
    const uint64 indexWidth = wideIndex ? sizeof(uint16) : sizeof(uint8);
    const uint64 weightWidth = wideWeight ? sizeof(uint16) : sizeof(uint8);
    const uint32 total = wideWeight ? uint32{BoneWeightTotal} : uint32{SkinWeightTotal8};
    const uint64 slots = lod.influencesPerVertex;
    const uint8* indexBytes = data.Data() + indices.offset;
    const uint8* weightBytes = data.Data() + weights.offset;
    for (uint64 vertex = 0; vertex < lod.vertexCount; ++vertex) {
        uint32 sum = 0;
        for (uint64 slot = 0; slot < slots; ++slot) {
            const uint64 at = vertex * slots + slot;
            const uint32 bone =
                wideIndex ? LoadLittleEndian<uint16>(indexBytes + at * indexWidth) : indexBytes[at];
            const uint32 weight = wideWeight
                                      ? LoadLittleEndian<uint16>(weightBytes + at * weightWidth)
                                      : weightBytes[at];
            // 빈 칸(가중치 0)은 번호 0이다 — 그 밖의 번호는 bone map 안이어야 한다.
            if (bone >= lod.boneMap.Size() && !(bone == 0 && weight == 0)) {
                return CorruptAt(failure, MeshIssue::RenderBoneIndexValue, lodIndex, at, attribute);
            }

            sum += weight;
        }

        if (sum != total) {
            return CorruptAt(failure, MeshIssue::RenderSkinWeightSum, lodIndex, vertex, attribute);
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> ValidateSkeletalMeshRenderData(const SkeletalMeshRenderData& renderData,
                                            usize boneCount, MeshDiagnostic* failure) noexcept {
    if (renderData.lods.Size() > SkeletalMeshMaxLods) {
        return mesh_render_checks::Corrupt(failure, MeshIssue::RenderLodCount, MeshNoIndex);
    }

    if (auto bounds =
            mesh_render_checks::ValidateBounds(renderData.box, renderData.sphereRadius, failure);
        !bounds) {
        return bounds;
    }

    for (usize lodIndex = 0; lodIndex < renderData.lods.Size(); ++lodIndex) {
        const SkinnedRenderLod& lod = renderData.lods[lodIndex];
        const auto index = static_cast<uint32>(lodIndex);
        const bool streams = !lod.data.IsEmpty();
        Result<void> streamed = Result<void>::Success();
        if (streams) {
            streamed = mesh_render_checks::ValidateStreams(lod, index, failure,
                                                           SkinnedStreams(lod, index, failure));
        } else {
            streamed = mesh_render_checks::ValidateStreamlessLod(lod, index, failure);
            if (streamed) {
                streamed = ValidateStreamlessSkin(lod, index, failure);
            }
        }

        if (!streamed) {
            return streamed;
        }

        if (auto sections = mesh_render_checks::ValidateSections(lod, index, streams, failure);
            !sections) {
            return sections;
        }

        if (auto map = ValidateBoneList(lod.boneMap, boneCount, index, failure); !map) {
            return map;
        }

        if (auto required = ValidateBoneList(lod.requiredBones, boneCount, index, failure);
            !required) {
            return required;
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateSkinnedRenderLodBulk(const SkinnedRenderLod& lod, uint32 lodIndex,
                                          Span<const uint8> data,
                                          MeshDiagnostic* failure) noexcept {
    // data가 빈 LOD(장치 없는 프로파일)는 읽을 byte가 없다.
    if (lod.data.IsEmpty()) {
        if (!data.IsEmpty()) {
            return mesh_render_checks::Corrupt(failure, MeshIssue::RenderStreamRange, lodIndex);
        }

        return Result<void>::Success();
    }

    if (auto indices = mesh_render_checks::ValidateIndexValues(lod, lodIndex, data, failure);
        !indices) {
        return indices;
    }

    if (auto base = ValidateInfluenceValues(lod, lod.boneIndices, lod.boneWeights, data, lodIndex,
                                            MeshNoAttribute, failure);
        !base) {
        return base;
    }

    for (usize index = 0; index < lod.skinWeightProfiles.Size(); ++index) {
        const SkinWeightProfileRenderLod& profile = lod.skinWeightProfiles[index];
        if (auto checked =
                ValidateInfluenceValues(lod, profile.boneIndices, profile.boneWeights, data,
                                        lodIndex, static_cast<uint32>(index), failure);
            !checked) {
            return checked;
        }
    }

    for (usize index = 0; index < lod.morphTargets.Size(); ++index) {
        const SkinnedMorphTarget& target = lod.morphTargets[index];
        const uint8* bytes = data.Data() + target.deltas.offset;
        for (uint32 delta = 0; delta < target.deltaCount; ++delta) {
            const uint32 vertex = LoadLittleEndian<uint32>(bytes + delta * SkinnedMorphDeltaBytes);
            const uint32 previous =
                delta == 0 ? 0
                           : LoadLittleEndian<uint32>(bytes + (delta - 1) * SkinnedMorphDeltaBytes);
            if (vertex >= lod.vertexCount || (delta > 0 && vertex <= previous)) {
                return CorruptAt(failure, MeshIssue::RenderMorphVertex, lodIndex, delta,
                                 static_cast<uint32>(index));
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateSkeletalMesh(const SkeletalMesh& mesh, Allocator& scratch,
                                  MeshDiagnostic* failure) noexcept {
    SkeletonDiagnostic bones;
    if (auto valid = ValidateBoneHierarchy(mesh.bones, scratch, &bones); !valid) {
        if (bones.code == AssetDiagnosticCode::None) {
            return valid;
        }

        const uint32 bone = bones.index == SkeletonNoIndex ? MeshNoIndex : bones.index;
        return mesh_render_checks::Corrupt(failure, MeshIssue::InvalidBoneHierarchy, bone);
    }

    // 쿠킹 LOD가 있으면 inverse-bind는 본마다 행렬 하나, 없으면(에디터 패키지) 빈다.
    const uint64 boneCount = mesh.bones.bones.Size();
    const uint64 expected =
        mesh.renderData.lods.IsEmpty() ? 0 : boneCount * SkinnedInverseBindMatrixBytes;
    const BulkDataRef& binds = mesh.renderData.inverseBindMatrices;
    if (binds.rawBytes != expected || binds.IsEmpty() != (expected == 0)) {
        return mesh_render_checks::Corrupt(failure, MeshIssue::RenderInverseBind, MeshNoIndex);
    }

    return ValidateSkeletalMeshRenderData(mesh.renderData, mesh.bones.bones.Size(), failure);
}

MeshBounds SkeletalMeshExtendedBounds(const SkeletalMesh& mesh) noexcept {
    // UE USkeletalMesh::CalculateExtendedBounds(SkeletalMesh.cpp:548)는 확장이 0이어도 늘 상자로
    // 다시 만든다(FBoxSphereBounds(FBox) — 반지름 = 반 대각선 길이). 정적 메시와 다르다.
    MeshBounds bounds;
    const Vec3d& positive = mesh.positiveBoundsExtension;
    const Vec3d& negative = mesh.negativeBoundsExtension;
    const Box<double>& box = mesh.renderData.box;
    bounds.box.min = Vec3d(box.min.x - negative.x, box.min.y - negative.y, box.min.z - negative.z);
    bounds.box.max = Vec3d(box.max.x + positive.x, box.max.y + positive.y, box.max.z + positive.z);
    const double halfX = (bounds.box.max.x - bounds.box.min.x) * 0.5;
    const double halfY = (bounds.box.max.y - bounds.box.min.y) * 0.5;
    const double halfZ = (bounds.box.max.z - bounds.box.min.z) * 0.5;
    bounds.sphereRadius = math::Sqrt(halfX * halfX + halfY * halfY + halfZ * halfZ);
    return bounds;
}

} // namespace rocks
