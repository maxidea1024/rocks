#include "MeshBuildInternal.h"

#include <Base/NumericLimits.h>
#include <Core/Text/Name.h>
#include <MeshTools/SkinnedMeshBuilder.h>
#include <Platform/FloatingPointEnvironment.h>
#include <Platform/Memory.h>

// 스킨 메시 LOD 빌드다(Docs/Design/MeshAnimationAssets 6.3·6.5절, UE
// GenerateSkeletalRenderMesh·BuildSkeletalMeshChunks·CalculateInvRefMatrices·ComputeMorphDeltas).
// 렌더 정점·섹션·bounds는 정적 빌드와 같은 단계 함수(MeshLodBuild.cpp)를 쓴다.
namespace rocks {

namespace {

// 스킨 빌드는 크기를 적용하지 않는다(6.5절 3).
constexpr float UnitScale = 1.0f;

// 원본 정점마다의 영향 칸 수다(줄인 영향은 많아야 MeshCookedInfluencesHigh개).
constexpr uint32 ReducedSlots = MeshCookedInfluencesHigh;

// 8비트 가중치 반올림의 더하는 값이다((w × 255 + 32767) / 65535 — BoneWeightTotal의 절반 내림).
constexpr uint32 QuantizeRounding = BoneWeightTotal / 2;

// 영향 a가 b보다 앞인지다(가중치 내림차순, 같으면 본 번호 오름차순).
bool InfluenceBefore(const BoneInfluence& a, const BoneInfluence& b) noexcept {
    return a.weight > b.weight || (a.weight == b.weight && a.boneIndex < b.boneIndex);
}

// 짧은 영향 배열의 삽입 정렬이다(많아야 MeshMaxSourceInfluences개).
void SortInfluences(Span<BoneInfluence> influences) noexcept {
    for (usize index = 1; index < influences.Size(); ++index) {
        const BoneInfluence value = influences[index];
        usize at = index;
        while (at > 0 && InfluenceBefore(value, influences[at - 1])) {
            influences[at] = influences[at - 1];
            --at;
        }

        influences[at] = value;
    }
}

// failure는 BuildSkinnedMeshLod의 지역 진단이라 늘 있다.
Result<void> FailSkin(MeshDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                      MeshIssue issue, uint32 index) noexcept {
    MeshDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.error = error;
    diagnostic.issue = issue;
    diagnostic.index = index;
    *failure = diagnostic;
    return Result<void>::Failure(error);
}

// 6.5절 4: 네 임계값은 유한한 0 이상, boneInfluenceLimit은 0 이상. 성공하면 영향 상한을 돌려준다.
Result<void> CheckSettings(const SkinnedMeshBuildSettings& settings, uint32& limit,
                           MeshDiagnostic* failure) noexcept {
    struct Threshold {
        MeshBuildSettingField field;
        float value;
    };

    const Threshold thresholds[] = {
        {MeshBuildSettingField::ThresholdPosition, settings.thresholdPosition},
        {MeshBuildSettingField::ThresholdTangentNormal, settings.thresholdTangentNormal},
        {MeshBuildSettingField::ThresholdUV, settings.thresholdUV},
        {MeshBuildSettingField::MorphThresholdPosition, settings.morphThresholdPosition},
    };
    for (const Threshold& threshold : thresholds) {
        if (!math::IsFinite(threshold.value) || threshold.value < 0.0f) {
            return FailSkin(failure, AssetDiagnosticCode::InvalidSetting,
                            ErrorCode::InvalidArgument, MeshIssue::InvalidSettingValue,
                            static_cast<uint32>(threshold.field));
        }
    }

    if (settings.boneInfluenceLimit < 0) {
        return FailSkin(failure, AssetDiagnosticCode::InvalidSetting, ErrorCode::InvalidArgument,
                        MeshIssue::InvalidSettingValue,
                        static_cast<uint32>(MeshBuildSettingField::BoneInfluenceLimit));
    }

    const auto requested = static_cast<uint32>(settings.boneInfluenceLimit);
    limit = requested == 0 || requested > MeshCookedInfluencesHigh ? MeshCookedInfluencesHigh
                                                                   : requested;
    return Result<void>::Success();
}

// 6.5절 5: 본이 있어야 하고 ValidateBoneHierarchy를 통과해야 한다.
Result<void> CheckBones(const BoneHierarchy& bones, Allocator& scratch,
                        MeshDiagnostic* failure) noexcept {
    if (bones.bones.IsEmpty()) {
        return FailSkin(failure, AssetDiagnosticCode::InvalidSkeletonData,
                        ErrorCode::InvalidArgument, MeshIssue::EmptyBoneHierarchy, MeshNoIndex);
    }

    SkeletonDiagnostic skeleton;
    if (auto valid = ValidateBoneHierarchy(bones, scratch, &skeleton); !valid) {
        if (skeleton.code == AssetDiagnosticCode::None) {
            return valid;
        }

        return FailSkin(failure, skeleton.code, valid.Error(), MeshIssue::InvalidBoneHierarchy,
                        skeleton.index);
    }

    return Result<void>::Success();
}

// 6.5절 3: 보존 필드(buildScale 포함)가 기본값이 아니면 UnsupportedSetting.
Result<void> AppendSkinSettingWarnings(const SkinnedMeshBuildSettings& settings,
                                       Array<MeshDiagnostic>& out) noexcept {
    for (uint32 field = 0; field < SkinnedMeshBuildSettingFieldCount; ++field) {
        const auto value = static_cast<MeshBuildSettingField>(field);
        if (IsPreservedSkinnedMeshBuildSetting(value) &&
            IsMeshBuildSettingChanged(settings, value)) {
            auto pushed = out.TryPushBack(mesh_build::MakeWarning(
                AssetDiagnosticCode::UnsupportedSetting, MeshIssue::UnsupportedSetting,
                MeshElement::Invalid, MeshNoAttribute, field, 0));
            if (!pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

// 가중치 속성 하나의 원본 정점별 줄인 영향이다(정점마다 ReducedSlots칸, 앞 counts[v]칸이 값).
struct ReducedWeights {
    explicit ReducedWeights(Allocator& allocator) noexcept : slots(allocator), counts(allocator) {
    }

    // 원본 속성 번호다(기본 가중치가 없으면 MeshNoAttribute — 모든 정점이 루트 본).
    uint32 sourceAttribute = MeshNoAttribute;
    Array<BoneInfluence> slots;
    Array<uint8> counts;

    Span<const BoneInfluence> Of(uint32 vertex) const noexcept {
        return Span<const BoneInfluence>(slots.Data() + static_cast<usize>(vertex) * ReducedSlots,
                                         counts[vertex]);
    }
};

// 원본 정점 전체의 영향을 줄이고 MissingInfluences·InfluencesTrimmed 경고를 붙인다(6.5절 7).
Result<void> ReduceAttribute(const MeshSource& work, uint32 workAttribute, uint32 limit,
                             ReducedWeights& reduced, Array<MeshDiagnostic>& warnings) noexcept {
    const uint32 vertices = work.Count(MeshElement::Vertex);
    if (auto resized = reduced.slots.TryResize(static_cast<usize>(vertices) * ReducedSlots);
        !resized) {
        return resized;
    }

    if (auto resized = reduced.counts.TryResize(vertices); !resized) {
        return resized;
    }

    uint32 missingCount = 0;
    uint32 missingFirst = MeshNoIndex;
    uint32 trimmedCount = 0;
    uint32 trimmedFirst = MeshNoIndex;
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        const Span<const BoneInfluence> source = workAttribute == MeshNoAttribute
                                                     ? Span<const BoneInfluence>()
                                                     : work.Influences(workAttribute, vertex);
        bool trimmed = false;
        bool missing = false;
        const Span<BoneInfluence> out(
            reduced.slots.Data() + static_cast<usize>(vertex) * ReducedSlots, ReducedSlots);
        reduced.counts[vertex] =
            static_cast<uint8>(ReduceBoneInfluences(source, limit, out, trimmed, missing));
        missingFirst = missing && missingCount == 0 ? vertex : missingFirst;
        missingCount += missing ? 1 : 0;
        trimmedFirst = trimmed && trimmedCount == 0 ? vertex : trimmedFirst;
        trimmedCount += trimmed ? 1 : 0;
    }

    if (missingCount != 0) {
        auto pushed = warnings.TryPushBack(mesh_build::MakeWarning(
            AssetDiagnosticCode::MissingInfluences, MeshIssue::MissingInfluences,
            MeshElement::Vertex, reduced.sourceAttribute, missingFirst, missingCount));
        if (!pushed) {
            return pushed;
        }
    }

    if (trimmedCount != 0) {
        return warnings.TryPushBack(mesh_build::MakeWarning(
            AssetDiagnosticCode::InfluencesTrimmed, MeshIssue::InfluencesTrimmed,
            MeshElement::Vertex, reduced.sourceAttribute, trimmedFirst, trimmedCount));
    }

    return Result<void>::Success();
}

// 모든 가중치 속성(기본 먼저, 프로필 원본 순서)의 영향을 줄인다. 기본 SkinWeights가 없어도
// sets[0]은 기본 가중치(모든 정점이 루트 본)다.
Result<void> ReduceAllWeights(const MeshSource& source, const MeshSource& work,
                              const mesh_build::WorkAttributes& attributes, uint32 limit,
                              Allocator& scratch, Array<ReducedWeights>& sets,
                              Array<MeshDiagnostic>& warnings) noexcept {
    const uint32 base = source.FindAttribute(MeshElement::Vertex, MeshAttributeName::SkinWeights);
    const bool hasBase = base != MeshNoAttribute;
    if (!hasBase) {
        ReducedWeights reduced(scratch);
        if (auto done = ReduceAttribute(work, MeshNoAttribute, limit, reduced, warnings); !done) {
            return done;
        }

        if (auto pushed = sets.TryPushBack(Move(reduced)); !pushed) {
            return pushed;
        }
    }

    for (usize index = 0; index < attributes.weightWork.Size(); ++index) {
        ReducedWeights reduced(scratch);
        reduced.sourceAttribute = attributes.weightSource[index];
        if (auto done =
                ReduceAttribute(work, attributes.weightWork[index], limit, reduced, warnings);
            !done) {
            return done;
        }

        if (auto pushed = sets.TryPushBack(Move(reduced)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// morph target 하나의 원본 속성 번호다(6.5절 13).
struct MorphSource {
    uint32 position = MeshNoAttribute;
    uint32 normal = MeshNoAttribute;
};

// 접두 뒤 이름이다(접두 판정을 통과한 이름에만 부른다).
StringView Suffix(StringView name, StringView prefix) noexcept {
    return StringView(name.Data() + prefix.ByteCount(), name.ByteCount() - prefix.ByteCount());
}

// morph 항목 값이다(속성이 없거나 항목이 없으면 +0, 비유한 항목은 +0 — 6.5절 13).
Vec3 MorphValue(const MeshSource& source, uint32 attribute, uint32 index) noexcept {
    if (attribute == MeshNoAttribute) {
        return Vec3(0.0f, 0.0f, 0.0f);
    }

    return mesh_build::FiniteOrZero(source.SparseValue(attribute, index));
}

// 비유한 항목이 있으면 NonFiniteAttribute(NonFiniteMorph) 경고를 붙인다.
Result<void> AppendNonFiniteMorph(const MeshSource& source, uint32 attribute, MeshElement element,
                                  Array<MeshDiagnostic>& warnings) noexcept {
    uint32 count = 0;
    uint32 first = MeshNoIndex;
    for (const MeshSparseVec3& entry : source.SparseValues(attribute)) {
        const bool finite = mesh_build::IsFiniteVector(entry.value);
        first = !finite && count == 0 ? entry.index : first;
        count += finite ? 0 : 1;
    }

    if (count == 0) {
        return Result<void>::Success();
    }

    return warnings.TryPushBack(mesh_build::MakeWarning(AssetDiagnosticCode::NonFiniteAttribute,
                                                        MeshIssue::NonFiniteMorph, element,
                                                        attribute, first, count));
}

// morph target을 찾고(Morph- 원본 순서 + 같은 이름의 MorphNormal-, 그다음 짝 없는 MorphNormal-)
// 비유한 항목 경고를 붙인다.
Result<void> FindMorphTargets(const MeshSource& source, Array<MorphSource>& morphs,
                              Array<MeshDiagnostic>& warnings) noexcept {
    const uint32 count = source.AttributeCount();
    for (uint32 attribute = 0; attribute < count; ++attribute) {
        const StringView name = source.AttributeInfo(attribute).name;
        if (!MatchesMeshAttributePrefix(name, MeshAttributeName::MorphPrefix)) {
            continue;
        }

        MorphSource morph;
        morph.position = attribute;
        const StringView suffix = Suffix(name, MeshAttributeName::MorphPrefix);
        for (uint32 other = 0; other < count; ++other) {
            const StringView otherName = source.AttributeInfo(other).name;
            if (MatchesMeshAttributePrefix(otherName, MeshAttributeName::MorphNormalPrefix) &&
                NameTextEquals(Suffix(otherName, MeshAttributeName::MorphNormalPrefix), suffix)) {
                morph.normal = other;
                break;
            }
        }

        if (auto pushed = morphs.TryPushBack(morph); !pushed) {
            return pushed;
        }
    }

    for (uint32 attribute = 0; attribute < count; ++attribute) {
        const StringView name = source.AttributeInfo(attribute).name;
        if (!MatchesMeshAttributePrefix(name, MeshAttributeName::MorphNormalPrefix)) {
            continue;
        }

        bool paired = false;
        for (const MorphSource& morph : morphs) {
            paired = paired || morph.normal == attribute;
        }

        if (!paired) {
            MorphSource morph;
            morph.normal = attribute;
            if (auto pushed = morphs.TryPushBack(morph); !pushed) {
                return pushed;
            }
        }
    }

    for (const MorphSource& morph : morphs) {
        if (morph.position != MeshNoAttribute) {
            if (auto appended =
                    AppendNonFiniteMorph(source, morph.position, MeshElement::Vertex, warnings);
                !appended) {
                return appended;
            }
        }

        if (morph.normal != MeshNoAttribute) {
            if (auto appended =
                    AppendNonFiniteMorph(source, morph.normal, MeshElement::Corner, warnings);
                !appended) {
                return appended;
            }
        }
    }

    return Result<void>::Success();
}

bool SameBits(const Vec3& a, const Vec3& b) noexcept {
    return platform::MemoryEquals(&a, &b, sizeof(Vec3));
}

bool SameInfluences(Span<const BoneInfluence> a, Span<const BoneInfluence> b) noexcept {
    if (a.Size() != b.Size()) {
        return false;
    }

    for (usize index = 0; index < a.Size(); ++index) {
        if (!(a[index] == b[index])) {
            return false;
        }
    }

    return true;
}

// 용접 비교의 스킨 항목(6.5절 8)이다.
struct SkinWeld {
    const MeshSource* source = nullptr;
    Span<const ReducedWeights> weights;
    Span<const MorphSource> morphs;
};

bool SameSkin(const void* context, uint32 a, uint32 b) noexcept {
    const auto& weld = *static_cast<const SkinWeld*>(context);
    const MeshSource& source = *weld.source;
    const uint32 vertexA = source.CornerVertices()[a];
    const uint32 vertexB = source.CornerVertices()[b];
    for (const MorphSource& morph : weld.morphs) {
        if (!SameBits(MorphValue(source, morph.normal, a), MorphValue(source, morph.normal, b))) {
            return false;
        }
    }

    if (vertexA == vertexB) {
        return true;
    }

    for (const ReducedWeights& set : weld.weights) {
        if (!SameInfluences(set.Of(vertexA), set.Of(vertexB))) {
            return false;
        }
    }

    for (const MorphSource& morph : weld.morphs) {
        if (!SameBits(MorphValue(source, morph.position, vertexA),
                      MorphValue(source, morph.position, vertexB))) {
            return false;
        }
    }

    return true;
}

// 렌더 정점마다의 쿠킹 영향을 만든다(6.5절 10·11): 칸 수, bone map, 본 번호를 map 위치로, 8비트
// 부호화. 프로필도 같다.
Result<void> BuildInfluences(const SkinnedMeshBuildInput& input, const MeshSource& work,
                             Span<const ReducedWeights> sets,
                             const mesh_build::VertexBuffers& buffers, Allocator& scratch,
                             SkinnedMeshBuildResult& result) noexcept {
    const uint32 vertices = result.mesh.VertexCount();
    const uint32 bones = static_cast<uint32>(input.bones->bones.Size());
    const Span<const uint32> cornerVertices = work.CornerVertices();
    Array<uint16> remap(scratch);
    if (auto resized = remap.TryResize(bones); !resized) {
        return resized;
    }

    // 쓰는 본에 1을 적고 칸 수를 정한다.
    uint32 widest = 0;
    for (uint32 vertex = 0; vertex < vertices; ++vertex) {
        const uint32 source = cornerVertices[buffers.vertexCorners[vertex]];
        for (const ReducedWeights& set : sets) {
            const Span<const BoneInfluence> influences = set.Of(source);
            const auto used = static_cast<uint32>(influences.Size());
            widest = used > widest ? used : widest;
            for (const BoneInfluence& influence : influences) {
                remap[influence.boneIndex] = 1;
            }
        }
    }

    for (uint32 bone = 0; bone < bones; ++bone) {
        if (remap[bone] != 0) {
            remap[bone] = static_cast<uint16>(result.boneMap.Size());
            if (auto pushed = result.boneMap.TryPushBack(static_cast<uint16>(bone)); !pushed) {
                return pushed;
            }
        }
    }

    const bool high = input.settings.useHighPrecisionSkinWeights;
    result.influencesPerVertex =
        widest <= MeshCookedInfluencesLow ? MeshCookedInfluencesLow : MeshCookedInfluencesHigh;
    result.weightTotal = high ? BoneWeightTotal : SkinWeightTotal8;
    const usize slots = static_cast<usize>(vertices) * result.influencesPerVertex;
    for (usize set = 0; set < sets.Size(); ++set) {
        Array<BoneInfluence>* target = &result.influences;
        if (set != 0) {
            SkinWeightProfileBuild profile(result.influences.GetAllocator());
            profile.sourceAttribute = sets[set].sourceAttribute;
            if (auto pushed = result.profiles.TryPushBack(Move(profile)); !pushed) {
                return pushed;
            }

            target = &result.profiles[result.profiles.Size() - 1].influences;
        }

        if (auto resized = target->TryResize(slots); !resized) {
            return resized;
        }

        for (uint32 vertex = 0; vertex < vertices; ++vertex) {
            const uint32 source = cornerVertices[buffers.vertexCorners[vertex]];
            const Span<const BoneInfluence> influences = sets[set].Of(source);
            const Span<BoneInfluence> out(target->Data() + static_cast<usize>(vertex) *
                                                               result.influencesPerVertex,
                                          influences.Size());
            for (usize at = 0; at < influences.Size(); ++at) {
                out[at].boneIndex = remap[influences[at].boneIndex];
                out[at].weight = influences[at].weight;
            }

            if (!high) {
                QuantizeSkinWeights8(out);
            }
        }
    }

    return Result<void>::Success();
}

// 필요 본(메시 본 전체)과 inverse-bind(6.5절 11·12)다.
Result<void> BuildBindPose(const BoneHierarchy& bones, Allocator& scratch,
                           SkinnedMeshBuildResult& result, MeshDiagnostic* failure) noexcept {
    const uint32 count = static_cast<uint32>(bones.bones.Size());
    Array<Mat4d> component(scratch);
    if (auto resized = component.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = result.requiredBones.TryResize(count); !resized) {
        return resized;
    }

    if (auto resized = result.inverseBindMatrices.TryResize(count); !resized) {
        return resized;
    }

    for (uint32 bone = 0; bone < count; ++bone) {
        result.requiredBones[bone] = static_cast<uint16>(bone);
        const Mat4d local = bones.referencePose[bone].ToMatrix();
        component[bone] =
            bone == 0 ? local
                      : local * component[static_cast<uint32>(bones.bones[bone].parentIndex)];
        auto inverse = component[bone].TryInverse();
        if (!inverse) {
            return FailSkin(failure, AssetDiagnosticCode::InvalidSkeletonData,
                            ErrorCode::InvalidArgument, MeshIssue::SingularBindPose, bone);
        }

        result.inverseBindMatrices[bone] = inverse.Value();
    }

    return Result<void>::Success();
}

// morph 변화량 빌드에 필요한 기본 빌드 상태다.
struct MorphBuild {
    const SkinnedMeshBuildInput* input = nullptr;
    const MeshSource* source = nullptr;
    MeshSource* work = nullptr;
    const mesh_build::LodBases* bases = nullptr;
    const mesh_build::VertexBuffers* buffers = nullptr;
    float degenerateThreshold = 0.0f;
};

// 법선 재계산의 작업 배열이다(target마다 다시 쓴다).
struct MorphScratch {
    explicit MorphScratch(Allocator& allocator) noexcept
        : basePositions(allocator), normals(allocator), positionDeltas(allocator),
          triangles(allocator) {
    }

    Array<Vec3> basePositions;
    // 코너 법선(성분 3개씩)이다.
    Array<float> normals;
    // 정점마다 위치 변화량(희소 항목만 채우고 끝나면 되돌린다)이다.
    Array<Vec3> positionDeltas;
    mesh_build::TriangleBases triangles;
};

// 6.5절 13: Δp가 0이 아닌 정점을 옮긴 위치로 그 정점을 쓰는 삼각형의 세 정점에 붙은 코너 법선만
// 0으로 두고 법선을 다시 계산한다. 결과는 scratch.normals다.
Result<void> RecomputeMorphNormals(const MorphBuild& build, uint32 attribute, Allocator& allocator,
                                   MorphScratch& scratch) noexcept {
    MeshSource& work = *build.work;
    const mesh_build::LodBases& bases = *build.bases;
    const Span<float> normals = scratch.normals.View();
    platform::MemoryCopy(normals.Data(), bases.basis.normals.Data(),
                         bases.basis.normals.ByteCount());
    const Span<Vec3> positions = work.Positions();
    const Span<const uint32> cornerVertices = work.CornerVertices();
    for (const MeshSparseVec3& entry : build.source->SparseValues(attribute)) {
        const Vec3 delta = mesh_build::FiniteOrZero(entry.value);
        if (delta.x == 0.0f && delta.y == 0.0f && delta.z == 0.0f) {
            continue;
        }

        positions[entry.index] = scratch.basePositions[entry.index] + delta;
        for (uint32 corner : bases.topology.VertexCorners(entry.index)) {
            for (uint32 triangle : bases.topology.CornerTriangles(corner)) {
                for (uint32 neighbor : work.TriangleCorners()[triangle].corners) {
                    for (uint32 affected : bases.topology.VertexCorners(cornerVertices[neighbor])) {
                        normals[3 * static_cast<usize>(affected)] = 0.0f;
                        normals[3 * static_cast<usize>(affected) + 1] = 0.0f;
                        normals[3 * static_cast<usize>(affected) + 2] = 0.0f;
                    }
                }
            }
        }
    }

    mesh_build::CornerBasis basis = bases.basis;
    basis.normals = normals;
    mesh_build::CornerBasisOptions options;
    options.mikkTSpace = true;
    options.weighted = bases.options.weighted;
    Result<void> status =
        mesh_build::ComputeTriangleBases(work, basis, build.degenerateThreshold, scratch.triangles);
    if (status) {
        status = mesh_build::ComputeCornerBases(work, bases.topology, bases.hardness,
                                                scratch.triangles, options, allocator, basis);
    }

    // 위치를 되돌린다(다음 target과 결과가 기본 위치를 본다).
    for (const MeshSparseVec3& entry : build.source->SparseValues(attribute)) {
        positions[entry.index] = scratch.basePositions[entry.index];
    }

    return status;
}

// morph target 하나의 렌더 정점별 변화량을 만든다(6.5절 13).
Result<void> BuildMorphTarget(const MorphBuild& build, const MorphSource& morph,
                              Allocator& allocator, MorphScratch& scratch,
                              const StaticMeshBuildResult& mesh, MorphTargetBuild& out) noexcept {
    const MeshSource& source = *build.source;
    const SkinnedMeshBuildSettings& settings = build.input->settings;
    // MorphNormal이 없는 target은 늘 Morph- 속성이 있다(FindMorphTargets).
    const bool recompute = morph.normal == MeshNoAttribute;
    if (recompute) {
        if (auto computed = RecomputeMorphNormals(build, morph.position, allocator, scratch);
            !computed) {
            return computed;
        }
    }

    if (morph.position != MeshNoAttribute) {
        for (const MeshSparseVec3& entry : source.SparseValues(morph.position)) {
            scratch.positionDeltas[entry.index] = mesh_build::FiniteOrZero(entry.value);
        }
    }

    const bool tracked = morph.normal != MeshNoAttribute || settings.recomputeNormals;
    const float threshold = settings.morphThresholdPosition * settings.morphThresholdPosition;
    const Span<const uint32> cornerVertices = source.CornerVertices();
    const Vec3 unit(UnitScale, UnitScale, UnitScale);
    Result<void> status = Result<void>::Success();
    for (uint32 vertex = 0; vertex < mesh.VertexCount() && status; ++vertex) {
        const uint32 corner = build.buffers->vertexCorners[vertex];
        MorphTargetDelta delta;
        delta.vertex = vertex;
        delta.positionDelta = scratch.positionDeltas[cornerVertices[corner]];
        if (recompute) {
            const Vec3 normal(scratch.normals[3 * static_cast<usize>(corner)],
                              scratch.normals[3 * static_cast<usize>(corner) + 1],
                              scratch.normals[3 * static_cast<usize>(corner) + 2]);
            delta.normalDelta = mesh_build::RenderNormal(normal, unit) - mesh.normals[vertex];
        } else {
            delta.normalDelta = MorphValue(source, morph.normal, corner);
        }

        if (mesh_build::Dot3(delta.positionDelta, delta.positionDelta) > threshold ||
            (tracked && mesh_build::Dot3(delta.normalDelta, delta.normalDelta) >
                            MorphNormalDeltaThresholdSquared)) {
            status = out.deltas.TryPushBack(delta);
        }
    }

    if (morph.position != MeshNoAttribute) {
        for (const MeshSparseVec3& entry : source.SparseValues(morph.position)) {
            scratch.positionDeltas[entry.index] = Vec3(0.0f, 0.0f, 0.0f);
        }
    }

    return status;
}

Result<void> BuildMorphTargets(const MorphBuild& build, Span<const MorphSource> morphs,
                               Allocator& scratch, SkinnedMeshBuildResult& result) noexcept {
    if (morphs.IsEmpty()) {
        return Result<void>::Success();
    }

    MorphScratch work(scratch);
    const Span<const Vec3> positions = build.work->Positions();
    if (auto appended = work.basePositions.TryAppend(positions); !appended) {
        return appended;
    }

    if (auto resized = work.positionDeltas.TryResize(positions.Size()); !resized) {
        return resized;
    }

    if (auto resized = work.normals.TryResize(build.bases->basis.normals.Size()); !resized) {
        return resized;
    }

    for (const MorphSource& morph : morphs) {
        MorphTargetBuild target(result.morphTargets.GetAllocator());
        target.positionAttribute = morph.position;
        target.normalAttribute = morph.normal;
        if (auto built = BuildMorphTarget(build, morph, scratch, work, result.mesh, target);
            !built) {
            return built;
        }

        if (auto pushed = result.morphTargets.TryPushBack(Move(target)); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 6.5절의 단계를 차례로 한다(결정적 부동소수점 환경 안).
Result<void> BuildSkinInEnvironment(const SkinnedMeshBuildInput& input, Allocator& scratch,
                                    SkinnedMeshBuildResult& result,
                                    MeshDiagnostic* failure) noexcept {
    const MeshSource& source = *input.source;
    const SkinnedMeshBuildSettings& settings = input.settings;
    uint32 limit = 0;
    if (auto checked = CheckSettings(settings, limit, failure); !checked) {
        return checked;
    }

    if (auto checked = CheckBones(*input.bones, scratch, failure); !checked) {
        return checked;
    }

    MeshValidateOptions options;
    options.scratch = &scratch;
    options.boneCount = static_cast<uint32>(input.bones->bones.Size());
    if (auto valid = ValidateMeshSource(source, options, failure); !valid) {
        return valid;
    }

    const uint32 triangles = source.Count(MeshElement::Triangle);
    if (static_cast<uint64>(triangles) * 3 > NumericLimits<uint32>::Max() ||
        triangles > MeshMikkTSpaceMaxTriangles) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 원본 사본(가중치 포함)에 값 정리, 설정 경고.
    StaticMeshBuildResult& mesh = result.mesh;
    MeshSource work(scratch);
    mesh_build::WorkAttributes attributes(scratch);
    if (auto copied = mesh_build::CopyBuildMesh(source, true, work, attributes); !copied) {
        return copied;
    }

    if (auto sanitized =
            mesh_build::SanitizeWorkMesh(work, attributes, options, mesh.warnings, failure);
        !sanitized) {
        return sanitized;
    }

    if (auto appended = AppendSkinSettingWarnings(settings, mesh.warnings); !appended) {
        return appended;
    }

    // 영향 줄이기와 morph 속성.
    Array<ReducedWeights> sets(scratch);
    if (auto reduced =
            ReduceAllWeights(source, work, attributes, limit, scratch, sets, mesh.warnings);
        !reduced) {
        return reduced;
    }

    Array<MorphSource> morphs(scratch);
    if (auto found = FindMorphTargets(source, morphs, mesh.warnings); !found) {
        return found;
    }

    // 5.2절 3–5단계(스킨 임계값).
    const float degenerate = settings.removeDegenerates ? settings.thresholdPosition : 0.0f;
    mesh_build::LodBases bases(scratch);
    if (auto computed = mesh_build::ComputeLodBases(source, work, attributes, settings, degenerate,
                                                    scratch, bases);
        !computed) {
        return computed;
    }

    if (bases.hasUv) {
        mesh.texCoordChannels =
            work.AttributeInfo(attributes.work[mesh_build::TexCoordSlot]).channels;
    }

    mesh_build::OverlappingWedges overlaps(scratch);
    if (auto found =
            mesh_build::FindOverlappingWedges(work, settings.thresholdPosition, scratch, overlaps);
        !found) {
        return found;
    }

    // 렌더 정점(6.5절 6·8), 섹션, bounds.
    SkinWeld skin;
    skin.source = &source;
    skin.weights = sets.View();
    skin.morphs = morphs.View();
    mesh_build::WeldOptions weld;
    weld.degenerateThreshold = degenerate;
    weld.positionThreshold = settings.thresholdPosition;
    weld.normalThreshold = settings.thresholdTangentNormal;
    weld.uvThreshold = settings.thresholdUV;
    weld.dropCollapsedTriangles = true;
    weld.sameExtra = &SameSkin;
    weld.context = &skin;
    const Vec3 unit(UnitScale, UnitScale, UnitScale);
    mesh_build::VertexBuffers buffers(scratch);
    if (auto built = mesh_build::BuildRenderVertices(work, bases.basis, attributes, overlaps, unit,
                                                     weld, mesh, buffers);
        !built) {
        return built;
    }

    if (auto built = mesh_build::BuildSections(source, input.slotNames, input.sectionInfo, buffers,
                                               scratch, mesh);
        !built) {
        return built;
    }

    mesh.bounds = mesh_build::ComputeBounds(mesh.positions.View(), input.positiveBoundsExtension,
                                            input.negativeBoundsExtension);

    // 영향·bone map·필요 본·inverse-bind·morph.
    if (auto built = BuildInfluences(input, work, sets.View(), buffers, scratch, result); !built) {
        return built;
    }

    if (auto built = BuildBindPose(*input.bones, scratch, result, failure); !built) {
        return built;
    }

    MorphBuild morph;
    morph.input = &input;
    morph.source = &source;
    morph.work = &work;
    morph.bases = &bases;
    morph.buffers = &buffers;
    morph.degenerateThreshold = degenerate;
    return BuildMorphTargets(morph, morphs.View(), scratch, result);
}

struct SkinBuildJob {
    const SkinnedMeshBuildInput* input = nullptr;
    Allocator* scratch = nullptr;
    SkinnedMeshBuildResult* result = nullptr;
    MeshDiagnostic* failure = nullptr;
    Result<void> status = Result<void>::Success();
};

} // namespace

uint32 ReduceBoneInfluences(Span<const BoneInfluence> source, uint32 limit, Span<BoneInfluence> out,
                            bool& trimmed, bool& missing) noexcept {
    ROCKS_ASSERT(source.Size() <= MeshMaxSourceInfluences && limit >= 1 &&
                 limit <= MeshCookedInfluencesHigh && out.Size() >= MeshCookedInfluencesHigh);
    BoneInfluence sorted[MeshMaxSourceInfluences] = {};
    uint32 count = 0;
    for (const BoneInfluence& influence : source) {
        if (influence.weight != 0) {
            sorted[count++] = influence;
        }
    }

    SortInfluences(Span<BoneInfluence>(sorted, count));
    missing = count == 0;
    trimmed = count > limit;
    if (missing) {
        out[0] = BoneInfluence{0, BoneWeightTotal};
        return 1;
    }

    const uint32 kept = trimmed ? limit : count;
    for (uint32 index = 0; index < kept; ++index) {
        out[index] = sorted[index];
    }

    if (trimmed) {
        const Span<BoneInfluence> view(out.Data(), kept);
        NormalizeBoneWeights(view);
        SortInfluences(view);
    }

    return kept;
}

void QuantizeSkinWeights8(Span<BoneInfluence> influences) noexcept {
    if (influences.IsEmpty()) {
        return;
    }

    int32 sum = 0;
    for (BoneInfluence& influence : influences) {
        influence.weight = static_cast<uint16>(
            (static_cast<uint32>(influence.weight) * SkinWeightTotal8 + QuantizeRounding) /
            BoneWeightTotal);
        sum += influence.weight;
    }

    const int32 first = static_cast<int32>(influences[0].weight) + (SkinWeightTotal8 - sum);
    ROCKS_ASSERT(first >= 0 && first <= SkinWeightTotal8);
    influences[0].weight = static_cast<uint16>(first);
    SortInfluences(influences);
}

Result<void> BuildSkinnedMeshLod(const SkinnedMeshBuildInput& input, SkinnedMeshBuildResult& out,
                                 MeshDiagnostic* failure) noexcept {
    if (input.source == nullptr || input.bones == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = out.influences.GetAllocator();
    SkinnedMeshBuildResult result(allocator);
    MeshDiagnostic diagnostic;
    SkinBuildJob job;
    job.input = &input;
    job.scratch = input.scratch != nullptr ? input.scratch : &allocator;
    job.result = &result;
    job.failure = &diagnostic;
    platform::RunWithDeterministicFloatingPoint(
        [](void* context) noexcept {
            auto& build = *static_cast<SkinBuildJob*>(context);
            build.status =
                BuildSkinInEnvironment(*build.input, *build.scratch, *build.result, build.failure);
        },
        &job);
    if (!job.status) {
        if (failure != nullptr) {
            diagnostic.error = job.status.Error();
            *failure = diagnostic;
        }

        return Result<void>::Failure(job.status.Error());
    }

    out = Move(result);
    return Result<void>::Success();
}

} // namespace rocks
