#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Core/Math/MathUtility.h>
#include <Meta/Annotations.h>

// 메시 축소 설정이다(Docs/Design/MeshAnimationAssets 4.2절·5.5.1절 5, UE FMeshReductionSettings —
// MeshReductionSettings.h). 축소기가 생길 때까지 모든 필드가 보존이다(저장·임포트만 한다).
namespace rocks {

// 자동 LOD 생성에서 메시 특징의 중요도다(UE EMeshFeatureImportance — 값 순서가 같다).
ROCKS_ENUM()
enum class MeshFeatureImportance : uint8 {
    Off,
    Lowest,
    Low,
    Normal,
    High,
    Highest
};

// 축소 종료 기준이다(UE EStaticMeshReductionTerimationCriterion — 값 순서가 같다).
ROCKS_ENUM()
enum class MeshReductionTermination : uint8 {
    // 삼각형 비율·수.
    Triangles,
    // 정점 비율·수.
    Vertices,
    // 둘 중 먼저 맞는 것.
    Any
};

// LOD 하나의 축소 설정이다. 기본값은 UE 생성자다(MeshReductionSettings.cpp).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshReductionSettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() float percentTriangles = 1.0f;
    ROCKS_PROPERTY() uint32 maxTriangles = NumericLimits<uint32>::Max();
    ROCKS_PROPERTY() float percentVertices = 1.0f;
    ROCKS_PROPERTY() uint32 maxVertices = NumericLimits<uint32>::Max();
    ROCKS_PROPERTY() float maxDeviation = 0.0f;
    ROCKS_PROPERTY() float pixelError = 8.0f;
    ROCKS_PROPERTY() float weldingThreshold = 0.0f;
    ROCKS_PROPERTY() float hardAngleThreshold = 80.0f;
    ROCKS_PROPERTY() int32 baseLod = 0;
    ROCKS_PROPERTY()
    MeshFeatureImportance silhouetteImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() MeshFeatureImportance textureImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() MeshFeatureImportance shadingImportance = MeshFeatureImportance::Normal;
    ROCKS_PROPERTY() bool recalculateNormals = false;
    ROCKS_PROPERTY() bool generateUniqueLightmapUVs = false;
    ROCKS_PROPERTY() bool keepSymmetry = false;
    ROCKS_PROPERTY() bool visibilityAided = false;
    ROCKS_PROPERTY() bool cullOccluded = false;
    ROCKS_PROPERTY()
    MeshReductionTermination terminationCriterion = MeshReductionTermination::Triangles;
    ROCKS_PROPERTY()
    MeshFeatureImportance visibilityAggressiveness = MeshFeatureImportance::Lowest;
    ROCKS_PROPERTY()
    MeshFeatureImportance vertexColorImportance = MeshFeatureImportance::Off;
};

// 축소 비율이 "줄이지 않음"으로 보는 하한이다(UE 기본 축소기의 Threshold_One = 1 −
// KINDA_SMALL_NUMBER — QuadricMeshReduction.cpp).
inline constexpr float MeshReductionNoReductionThreshold = 1.0f - math::KindaSmallNumber<float>;

// settings가 vertexCount 정점·triangleCount 삼각형 메시를 줄이는지다(UE 기본 축소기
// IsReductionActive와 같은 식 — 종료 기준별로 비율 < MeshReductionNoReductionThreshold이거나 최대
// 수 < 원본 수). UE UStaticMesh::IsReductionActive의 LOD 그룹 덮어쓰기는 메시 LOD 그룹 표가 생길 때
// 더한다.
//
// 사용 예:
//   const bool generated = IsMeshReductionActive(lod.reduction, source.vertexCount,
//   source.triangleCount);
ROCKS_MESH_API bool IsMeshReductionActive(const MeshReductionSettings& settings, uint32 vertexCount,
                                          uint32 triangleCount) noexcept;

} // namespace rocks
