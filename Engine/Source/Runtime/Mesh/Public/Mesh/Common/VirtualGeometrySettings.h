#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Meta/Annotations.h>

// 가상 기하(VirtualGeometry — UE Nanite 대응) 빌드 설정이다(Docs/Design/MeshAnimationAssets
// 5.5.1절 5, UE FMeshNaniteSettings — EngineTypes.h). VirtualGeometry 단계까지 모든 필드가
// 보존이다. UE의 DisplacementMaps(텍스처 참조 배열)와 NaniteAssemblyData는 그 단계에서 더한다.
namespace rocks {

// 먼 거리에서 형상을 지키는 방식이다(UE ENaniteShapePreservation — 값 순서가 같다).
ROCKS_ENUM()
enum class VirtualGeometryShapePreservation : uint8 {
    None,
    PreserveArea,
    Voxelize
};

// 대체 메시를 만드는지다(UE ENaniteGenerateFallback — 값 순서가 같다).
ROCKS_ENUM()
enum class VirtualGeometryFallbackMode : uint8 {
    // 플랫폼 설정을 따른다.
    PlatformDefault,
    // 플랫폼 설정과 무관하게 만든다.
    Enabled
};

// 대체 메시 축소 목표다(UE ENaniteFallbackTarget — 값 순서가 같다).
ROCKS_ENUM()
enum class VirtualGeometryFallbackTarget : uint8 {
    Auto,
    PercentTriangles,
    RelativeError
};

// 메시 하나의 가상 기하 설정이다. 필드는 UE FMeshNaniteSettings의 값 필드를 같은 순서·기본값으로
// 둔다(UE 생성자 — bLerpUVs·bVoxelNDF 참, 그 밖 bool 거짓).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct VirtualGeometrySettings {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() bool enabled = false;
    ROCKS_PROPERTY() bool explicitTangents = false;
    ROCKS_PROPERTY() bool lerpUvs = true;
    ROCKS_PROPERTY() bool separable = false;
    ROCKS_PROPERTY() bool voxelNdf = true;
    ROCKS_PROPERTY() bool voxelOpacity = false;
    ROCKS_PROPERTY()
    VirtualGeometryShapePreservation shapePreservation = VirtualGeometryShapePreservation::None;
    // 위치 정밀도다(UE MIN_int32 = 자동).
    ROCKS_PROPERTY() int32 positionPrecision = NumericLimits<int32>::Min();
    // 법선·tangent·본 가중치 정밀도다(−1 = 자동).
    ROCKS_PROPERTY() int32 normalPrecision = -1;
    ROCKS_PROPERTY() int32 tangentPrecision = -1;
    ROCKS_PROPERTY() int32 boneWeightPrecision = -1;
    ROCKS_PROPERTY() uint32 targetMinimumResidencyInKb = 0;
    ROCKS_PROPERTY() float keepPercentTriangles = 1.0f;
    ROCKS_PROPERTY() float trimRelativeError = 0.0f;
    ROCKS_PROPERTY()
    VirtualGeometryFallbackMode generateFallback = VirtualGeometryFallbackMode::PlatformDefault;
    ROCKS_PROPERTY()
    VirtualGeometryFallbackTarget fallbackTarget = VirtualGeometryFallbackTarget::Auto;
    ROCKS_PROPERTY() float fallbackPercentTriangles = 1.0f;
    ROCKS_PROPERTY() float fallbackRelativeError = 1.0f;
    ROCKS_PROPERTY() float maxEdgeLengthFactor = 0.0f;
    ROCKS_PROPERTY() int32 numRays = 64;
    ROCKS_PROPERTY() int32 voxelLevel = 0;
    ROCKS_PROPERTY() float rayBackUp = 0.0f;
    ROCKS_PROPERTY() int32 displacementUvChannel = 0;
};

} // namespace rocks
