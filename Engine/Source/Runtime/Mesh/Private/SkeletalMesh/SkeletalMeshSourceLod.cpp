#include <Mesh/SkeletalMesh/SkeletalMeshSourceLod.h>

// 스킨 메시 원본 LOD 판정이다(Docs/Design/MeshAnimationAssets 6.7.1절 4).
namespace rocks {

bool IsSkeletalMeshLodGenerated(const SkeletalMeshSourceLod& lod, int32 lodIndex) noexcept {
    if (!lod.hasSource) {
        return true;
    }

    const bool inlineReduction = lod.reduction.baseLod == lodIndex;
    const uint32 vertices = inlineReduction ? lod.source.vertexCount : NumericLimits<uint32>::Max();
    const uint32 triangles =
        inlineReduction ? lod.source.triangleCount : NumericLimits<uint32>::Max();
    return IsSkeletalMeshReductionActive(lod.reduction, vertices, triangles);
}

} // namespace rocks
