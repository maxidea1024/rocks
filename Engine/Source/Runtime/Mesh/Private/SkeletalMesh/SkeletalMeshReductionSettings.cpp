#include <Mesh/SkeletalMesh/SkeletalMeshReductionSettings.h>

// 스킨 메시 축소 활성 판정이다(Docs/Design/MeshAnimationAssets 6.7.1절 4 — UE 스킨 축소기
// IsReductionActive, SkeletalMeshReductionPlugin.cpp:69).
namespace rocks {

bool IsSkeletalMeshReductionActive(const SkeletalMeshReductionSettings& settings,
                                   uint32 vertexCount, uint32 triangleCount) noexcept {
    switch (settings.terminationCriterion) {
    case SkeletalMeshReductionTermination::Triangles:
        return settings.percentTriangles < MeshReductionNoReductionThreshold ||
               settings.maxTrianglesFromPercent < triangleCount;
    case SkeletalMeshReductionTermination::Vertices:
        return settings.percentVertices < MeshReductionNoReductionThreshold ||
               settings.maxVerticesFromPercent < vertexCount;
    case SkeletalMeshReductionTermination::TrianglesOrVertices:
        return settings.percentTriangles < MeshReductionNoReductionThreshold ||
               settings.percentVertices < MeshReductionNoReductionThreshold;
    // 절대 수 기준은 수가 상한보다 크면 줄인다.
    case SkeletalMeshReductionTermination::AbsoluteTriangles:
        return settings.maxTriangles < triangleCount;
    case SkeletalMeshReductionTermination::AbsoluteVertices:
        return settings.maxVertices < vertexCount;
    case SkeletalMeshReductionTermination::AbsoluteTrianglesOrVertices:
        return settings.maxVertices < vertexCount || settings.maxTriangles < triangleCount;
    }

    return false;
}

} // namespace rocks
