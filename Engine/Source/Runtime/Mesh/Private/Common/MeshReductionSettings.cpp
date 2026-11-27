#include <Mesh/Common/MeshReductionSettings.h>

// 메시 축소 활성 판정이다(Docs/Design/MeshAnimationAssets 5.5.1절 5 — UE 기본 축소기
// IsReductionActive, QuadricMeshReduction.cpp).
namespace rocks {

bool IsMeshReductionActive(const MeshReductionSettings& settings, uint32 vertexCount,
                           uint32 triangleCount) noexcept {
    const bool triangles = settings.percentTriangles < MeshReductionNoReductionThreshold ||
                           settings.maxTriangles < triangleCount;
    const bool vertices = settings.percentVertices < MeshReductionNoReductionThreshold ||
                          settings.maxVertices < vertexCount;
    switch (settings.terminationCriterion) {
    case MeshReductionTermination::Triangles:
        return triangles;
    case MeshReductionTermination::Vertices:
        return vertices;
    case MeshReductionTermination::Any:
        return triangles || vertices;
    }

    return false;
}

} // namespace rocks
