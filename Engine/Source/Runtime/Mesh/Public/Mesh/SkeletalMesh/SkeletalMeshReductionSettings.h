#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Mesh/Common/MeshReductionSettings.h>
#include <Meta/Annotations.h>

// 스킨 메시 축소 설정이다(Docs/Design/MeshAnimationAssets 6.7.1절 4, UE
// FSkeletalMeshOptimizationSettings — E/Public/SkeletalMeshReductionSettings.h). 정적 메시 축소
// 설정과 필드·기본값이 다른 별도 struct다. 축소기가 생길 때까지 모든 필드가 보존이다(저장·임포트만
// 한다). 특징 중요도는 정적과 같은 MeshFeatureImportance다(UE SkeletalMeshOptimizationImportance와
// 값 순서가 같다 — Off·Lowest·Low·Normal·High·Highest).
namespace rocks {

// 축소 종료 기준이다(UE SkeletalMeshTerminationCriterion — 값 순서가 같다).
ROCKS_ENUM()
enum class SkeletalMeshReductionTermination : uint8 {
    // 삼각형 비율(maxTrianglesFromPercent 상한).
    Triangles,
    // 정점 비율(maxVerticesFromPercent 상한).
    Vertices,
    // 둘 중 먼저 맞는 비율.
    TrianglesOrVertices,
    // 삼각형 절대 수(maxTriangles).
    AbsoluteTriangles,
    // 정점 절대 수(maxVertices).
    AbsoluteVertices,
    // 둘 중 먼저 맞는 절대 수.
    AbsoluteTrianglesOrVertices
};

// 축소 방식이다(UE SkeletalMeshOptimizationType — 값 순서가 같다).
ROCKS_ENUM()
enum class SkeletalMeshReductionMethod : uint8 {
    Triangles,
    MaxDeviation,
    TrianglesOrDeviation
};

// LOD 하나의 스킨 축소 설정이다. 기본값은 UE 생성자다(SkeletalMeshReductionSettings.h:179).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkeletalMeshReductionSettings {
    ROCKS_GENERATED_BODY()

    // UE 생성자 기본값이다.
    static constexpr float DefaultPercent = 0.5f;
    static constexpr uint32 DefaultMaxTriangles = 4;
    static constexpr uint32 DefaultMaxVertices = 6;
    static constexpr float DefaultMaxDeviationPercent = 0.5f;
    static constexpr float DefaultWeldingThreshold = 0.1f;
    static constexpr float DefaultNormalsThreshold = 60.0f;
    static constexpr int32 DefaultMaxBonesPerVertex = 4;
    static constexpr float DefaultVolumeImportance = 1.0f;

    ROCKS_PROPERTY()
    SkeletalMeshReductionTermination terminationCriterion =
        SkeletalMeshReductionTermination::Triangles;
    ROCKS_PROPERTY() float percentTriangles = DefaultPercent;
    ROCKS_PROPERTY() float percentVertices = DefaultPercent;
    ROCKS_PROPERTY() uint32 maxTriangles = DefaultMaxTriangles;
    ROCKS_PROPERTY() uint32 maxVertices = DefaultMaxVertices;
    // 비율 기준의 절대 상한이다(UE MaxNumOfTrianglesPercentage·MaxNumOfVertsPercentage).
    ROCKS_PROPERTY() uint32 maxTrianglesFromPercent = NumericLimits<uint32>::Max();
    ROCKS_PROPERTY() uint32 maxVerticesFromPercent = NumericLimits<uint32>::Max();
    ROCKS_PROPERTY() float maxDeviationPercent = DefaultMaxDeviationPercent;
    ROCKS_PROPERTY()
    SkeletalMeshReductionMethod reductionMethod = SkeletalMeshReductionMethod::Triangles;
    ROCKS_PROPERTY() MeshFeatureImportance silhouetteImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() MeshFeatureImportance textureImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() MeshFeatureImportance shadingImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() MeshFeatureImportance skinningImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() bool remapMorphTargets = false;
    ROCKS_PROPERTY() bool recalculateNormals = true;
    ROCKS_PROPERTY() float weldingThreshold = DefaultWeldingThreshold;
    ROCKS_PROPERTY() float normalsThreshold = DefaultNormalsThreshold;
    ROCKS_PROPERTY() int32 maxBonesPerVertex = DefaultMaxBonesPerVertex;
    ROCKS_PROPERTY() bool enforceBoneBoundaries = false;
    ROCKS_PROPERTY() bool mergeCoincidentVertexBones = true;
    ROCKS_PROPERTY() float volumeImportance = DefaultVolumeImportance;
    ROCKS_PROPERTY() bool lockEdges = false;
    ROCKS_PROPERTY() bool lockColorBoundaries = false;
    ROCKS_PROPERTY() bool improveTrianglesForCloth = true;
    // 축소 원본 LOD 번호다(UE BaseLOD — 자기 번호면 inline 축소).
    ROCKS_PROPERTY() int32 baseLod = 0;
};

// 설정이 vertexCount·triangleCount 원본을 줄이는지다(UE 스킨 축소기 IsReductionActive —
// SkeletalMeshReductionPlugin.cpp:69, 비율 문턱은 정적과 같은 MeshReductionNoReductionThreshold).
//
// 사용 예:
//   if (IsSkeletalMeshReductionActive(lod.reduction, source.vertexCount, source.triangleCount)) {
//   ... }
ROCKS_MESH_API bool IsSkeletalMeshReductionActive(const SkeletalMeshReductionSettings& settings,
                                                  uint32 vertexCount,
                                                  uint32 triangleCount) noexcept;

} // namespace rocks
