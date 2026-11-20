#pragma once

#include "MeshToolsTestSupport.h"

#include <Animation/Tests/AnimationTestSupport.h>
#include <MeshSource/Tests/MeshSourceTestSupport.h>
#include <MeshTools/SkinnedMeshBuilder.h>

// 스킨 메시 빌드 시험 공용 도우미다(Docs/Design/MeshAnimationAssets.ko.md 12절 "스켈레톤·스킨"):
// Name pool, 본 계층 만들기, 가중치·morph 속성 쓰기, 스킨 빌드 결과의 정규 byte.
namespace rocks::skinned_mesh_test {

using namespace rocks::mesh_tools_test;

// 큰 계층·퍼징이 쓰는 pool entry 상한이다(기본 65536보다 넉넉하게).
constexpr uint64 SkinPoolEntries = 1u << 18;

// 할당기와 Name pool이다.
struct SkinFixture {
    SkinFixture() noexcept : names(allocator) {
        NamePoolConfig config;
        config.maxEntries = SkinPoolEntries;
        ready = names.Initialize(config).IsOk();
    }

    SystemAllocator allocator;
    NamePool names;
    bool ready = false;
};

// 본 하나를 더한다(이름은 prefix + 번호 문자, pose는 인자).
inline bool AddBone(NamePool& names, BoneHierarchy& bones, int32 parent,
                    const Transform& pose) noexcept {
    char text[16] = {};
    const StringView name =
        animation_test::LetterName('b', static_cast<uint32>(bones.bones.Size()), text);
    auto interned = names.TryIntern(name);
    if (!interned) {
        return false;
    }

    BoneInfo bone(bones.bones.GetAllocator());
    bone.name = interned.Value();
    bone.parentIndex = parent;
    return bone.exportName.TryAssign(name) && bones.bones.TryPushBack(Move(bone)) &&
           bones.referencePose.TryPushBack(pose);
}

// 본 count개의 사슬(본 i의 부모 = i − 1)이다. pose는 PoseFor(번호)다.
inline bool ChainBones(NamePool& names, uint32 count, BoneHierarchy& bones) noexcept {
    for (uint32 bone = 0; bone < count; ++bone) {
        const int32 parent = bone == 0 ? NoParentBone : static_cast<int32>(bone - 1);
        if (!AddBone(names, bones, parent, animation_test::PoseFor(bone))) {
            return false;
        }
    }

    return true;
}

inline SkinnedMeshBuildInput SkinInputFor(const MeshSource& mesh,
                                          const BoneHierarchy& bones) noexcept {
    SkinnedMeshBuildInput input;
    input.source = &mesh;
    input.bones = &bones;
    return input;
}

// 정점 가중치 속성을 더한다(실패하면 MeshNoAttribute).
inline uint32 AddWeights(MeshSource& mesh, StringView name) noexcept {
    auto added = mesh.AddAttribute(MeshElement::Vertex, name, MeshValueType::BoneInfluences, 1);
    return added ? added.Value() : MeshNoAttribute;
}

inline bool SetWeights(MeshSource& mesh, uint32 attribute, uint32 vertex,
                       Span<const BoneInfluence> influences) noexcept {
    return mesh.SetInfluences(attribute, vertex, influences).IsOk();
}

// 모든 정점을 본 bone 하나(가중치 전부)에 붙인다.
inline uint32 RigidWeights(MeshSource& mesh, uint16 bone) noexcept {
    const uint32 attribute = AddWeights(mesh, MeshAttributeName::SkinWeights);
    for (uint32 vertex = 0;
         attribute != MeshNoAttribute && vertex < mesh.Count(MeshElement::Vertex); ++vertex) {
        const BoneInfluence influence{bone, BoneWeightTotal};
        if (!SetWeights(mesh, attribute, vertex, Span<const BoneInfluence>(&influence, 1))) {
            return MeshNoAttribute;
        }
    }

    return attribute;
}

// 희소 Vec3 속성을 더한다(실패하면 MeshNoAttribute).
inline uint32 AddSparse(MeshSource& mesh, MeshElement element, StringView name) noexcept {
    auto added = mesh.AddAttribute(element, name, MeshValueType::SparseVec3, 1);
    return added ? added.Value() : MeshNoAttribute;
}

// count × count 정점 격자(간격 spacing, z = 0, 정점마다 코너 하나를 모든 칸이 공유)다. 칸마다
// 다각형 하나(0번 꼭짓점 부채꼴 두 삼각형), 묶음 하나.
inline bool BuildGrid(MeshSource& mesh, uint32 count, float spacing) noexcept {
    Array<Vec3> positions(mesh.GetAllocator());
    for (uint32 y = 0; y < count; ++y) {
        for (uint32 x = 0; x < count; ++x) {
            if (!positions.TryPushBack(Vec3(spacing * x, spacing * y, 0.0f))) {
                return false;
            }
        }
    }

    Array<uint32> quads(mesh.GetAllocator());
    Array<PolygonSpec> polygons(mesh.GetAllocator());
    if (!quads.TryResize(static_cast<usize>(count - 1) * (count - 1) * 4)) {
        return false;
    }

    for (uint32 y = 0; y + 1 < count; ++y) {
        for (uint32 x = 0; x + 1 < count; ++x) {
            uint32* quad = quads.Data() + (static_cast<usize>(y) * (count - 1) + x) * 4;
            quad[0] = y * count + x;
            quad[1] = y * count + x + 1;
            quad[2] = (y + 1) * count + x + 1;
            quad[3] = (y + 1) * count + x;
            if (!polygons.TryPushBack(PolygonSpec{Span<const uint32>(quad, 4), 0})) {
                return false;
            }
        }
    }

    return BuildPolygonMesh(mesh, positions.View(), polygons.View(), 1, true);
}

// BuildGrid 격자에서 원본 정점 source의 렌더 정점 번호다(위치로 찾는다 — 렌더 정점은 삼각형 순서로
// 생겨 원본 번호와 다르다). 없으면 MeshNoIndex다.
inline uint32 GridRenderVertex(const StaticMeshBuildResult& result, uint32 count, float spacing,
                               uint32 source) noexcept {
    const Vec3 position(spacing * (source % count), spacing * (source / count), 0.0f);
    for (uint32 vertex = 0; vertex < result.VertexCount(); ++vertex) {
        if (result.positions[vertex].x == position.x && result.positions[vertex].y == position.y) {
            return vertex;
        }
    }

    return MeshNoIndex;
}

// 스킨 빌드 결과의 정규 byte다(렌더 부분 + 스킨 배열 전부).
inline Array<uint8> SkinBytes(Allocator& allocator, const SkinnedMeshBuildResult& result) noexcept {
    Array<uint8> bytes = CanonicalBytes(allocator, result.mesh);
    bool ok = !bytes.IsEmpty() && AppendBytes(bytes, result.influencesPerVertex) &&
              AppendBytes(bytes, result.weightTotal) && AppendArray(bytes, result.influences) &&
              AppendArray(bytes, result.boneMap) && AppendArray(bytes, result.requiredBones) &&
              AppendArray(bytes, result.inverseBindMatrices);
    for (const SkinWeightProfileBuild& profile : result.profiles) {
        ok = ok && AppendBytes(bytes, profile.sourceAttribute) &&
             AppendArray(bytes, profile.influences);
    }

    for (const MorphTargetBuild& target : result.morphTargets) {
        ok = ok && AppendBytes(bytes, target.positionAttribute) &&
             AppendBytes(bytes, target.normalAttribute) && AppendArray(bytes, target.deltas);
    }

    if (!ok) {
        bytes.Clear();
    }

    return bytes;
}

// 경고에서 code인 것 하나를 찾는다(없으면 null).
inline const MeshDiagnostic* FindWarning(const SkinnedMeshBuildResult& result,
                                         AssetDiagnosticCode code,
                                         uint32 attribute = MeshNoAttribute) noexcept {
    for (const MeshDiagnostic& warning : result.mesh.warnings) {
        if (warning.code == code &&
            (attribute == MeshNoAttribute || warning.attribute == attribute)) {
            return &warning;
        }
    }

    return nullptr;
}

// 렌더 정점 vertex의 영향 칸이다.
inline Span<const BoneInfluence> VertexInfluences(const SkinnedMeshBuildResult& result,
                                                  const Array<BoneInfluence>& influences,
                                                  uint32 vertex) noexcept {
    return Span<const BoneInfluence>(influences.Data() +
                                         static_cast<usize>(vertex) * result.influencesPerVertex,
                                     result.influencesPerVertex);
}

// 칸의 가중치 합이 total이고 내림차순(같으면 번호 오름차순)이며 번호가 bone map 안인지 본다.
inline bool InfluencesWellFormed(const SkinnedMeshBuildResult& result,
                                 Span<const BoneInfluence> slots) noexcept {
    uint32 sum = 0;
    for (usize index = 0; index < slots.Size(); ++index) {
        sum += slots[index].weight;
        if (slots[index].boneIndex >= result.boneMap.Size()) {
            return false;
        }

        if (index != 0 && (slots[index].weight > slots[index - 1].weight)) {
            return false;
        }
    }

    return sum == result.weightTotal;
}

} // namespace rocks::skinned_mesh_test
