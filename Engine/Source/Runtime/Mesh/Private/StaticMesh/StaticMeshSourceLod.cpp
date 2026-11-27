#include <Mesh/StaticMesh/StaticMeshSourceLod.h>

// 정적 메시 원본 LOD 판정이다(Docs/Design/MeshAnimationAssets 5.5.1절 6).
namespace rocks {

bool IsStaticMeshLodGenerated(const StaticMeshSourceLod& lod) noexcept {
    return !lod.hasSource ||
           IsMeshReductionActive(lod.reduction, lod.source.vertexCount, lod.source.triangleCount);
}

} // namespace rocks
