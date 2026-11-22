#pragma once

#include <Base/Types.h>
#include <Core/Math/Vector3.h>
#include <Meta/Annotations.h>
#include <Object/ModuleRegistration.h>

// 메시 빌드 설정이다(Docs/Design/MeshAnimationAssets.ko.md 4.1절, UE FMeshBuildSettings —
// EngineTypes.h).
namespace rocks {

// LOD 하나의 빌드 설정이다. 정적 메시와 스킨 메시가 같은 구조를 쓰고(스킨 메시만의 필드는
// SkinnedMeshBuildSettings가 더한다 — 6.3절), 기본값은 UE FMeshBuildSettings 생성자와 같다.
//
// 필드 상태(4.1절 표): "구현" 필드는 MeshTools 빌드(BuildStaticMeshLod)가 쓰고, "보존" 필드는
// 저장·임포트만 하며 값이 기본값이 아니면 빌드가 UnsupportedSetting 경고를 낸다(경고의
// MeshDiagnostic::index가 MeshBuildSettingField 번호다). UE의 DistanceFieldReplacementMesh(객체
// 참조)는 StaticMesh 필드로 두고(4.1절), bUseBackwardsCompatibleF16TruncUVs(옛 콘텐츠 호환)는 두지
// 않는다.
//
// 사용 예:
//   MeshBuildSettings settings;
//   settings.removeDegenerates = false;          // 위치가 정확히 같은 꼭짓점이 있는 삼각형만 뺀다
//   settings.buildScale = Vec3d(1.0, 1.0, -1.0);  // Z 거울(winding은 그대로 — NegativeBuildScale
//   경고)
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshBuildSettings {
    ROCKS_GENERATED_BODY()

    // 구현: 법선을 늘 다시 계산한다(꺼져 있어도 0 벡터 코너는 계산한다).
    ROCKS_PROPERTY() bool recomputeNormals = true;
    // 구현: tangent를 늘 다시 계산한다(꺼져 있어도 0 벡터 코너는 계산한다).
    ROCKS_PROPERTY() bool recomputeTangents = true;
    // 구현: 다시 계산하는 tangent를 MikkTSpace로 만든다(UV 채널이 없으면 평균 방식).
    ROCKS_PROPERTY() bool useMikkTSpace = true;
    // 구현: 코너 법선을 면적 × 코너 각으로 가중 평균한다.
    ROCKS_PROPERTY() bool computeWeightedNormals = false;
    // 구현: 두 꼭짓점 위치가 MeshPointsSameThreshold 안에서 같은 삼각형을 빼고 용접 위치 임계값도
    // 그 값을 쓴다. 끄면 임계값이 0이다(정확히 같은 위치만 — UE와 같다).
    ROCKS_PROPERTY() bool removeDegenerates = true;
    // 보존: 뒤집힌 index buffer(UE bBuildReversedIndexBuffer).
    ROCKS_PROPERTY() bool buildReversedIndexBuffer = true;
    // 구현(쿠킹 정점 형식 선택 — MA2b 패킹이 쓴다): tangent 기저 16비트, UV float.
    ROCKS_PROPERTY() bool useHighPrecisionTangentBasis = false;
    ROCKS_PROPERTY() bool useFullPrecisionUVs = false;
    // 보존: lightmap UV 생성(라이트맵 단계, 계획 14.10절).
    ROCKS_PROPERTY() bool generateLightmapUVs = true;
    ROCKS_PROPERTY() int32 minLightmapResolution = 64;
    ROCKS_PROPERTY() int32 srcLightmapIndex = 0;
    ROCKS_PROPERTY() int32 dstLightmapIndex = 1;
    // 구현: 위치·tangent·법선에 곱하는 축별 크기(5.2절 8단계 — 음수 성분도 winding을 뒤집지
    // 않는다). 성분은 float로 바꿔 쓰며 0이 아닌 유한값이고 역수도 유한해야 한다.
    ROCKS_PROPERTY() Vec3d buildScale = Vec3d(1.0, 1.0, 1.0);
    // 보존: distance field·Lumen(GI 단계).
    ROCKS_PROPERTY() bool generateDistanceFieldAsIfTwoSided = false;
    ROCKS_PROPERTY() float distanceFieldResolutionScale = 1.0f;
    ROCKS_PROPERTY() int32 maxLumenMeshCards = 12;
    // 보존: 면 번호 재매핑(물리 단계).
    ROCKS_PROPERTY() bool supportFaceRemap = false;
};

// 스킨 메시 LOD 하나의 빌드 설정이다(6.3·6.5절, UE FSkeletalMeshBuildSettings — EngineTypes.h).
// MeshBuildSettings를 물려받아 스킨 빌드만 쓰는 여섯 필드를 더한다(모두 "구현"). 기본값은 UE
// 생성자와 같다. 스킨 빌드는 buildScale을 쓰지 않는다(보존 — 크기는 임포터가 메시와 본에 함께
// 적용한다, 6.5절 3).
//
// 사용 예:
//   SkinnedMeshBuildSettings settings;
//   settings.boneInfluenceLimit = 4;              // 정점마다 영향 4개까지(쿠킹 칸 4)
//   settings.useHighPrecisionSkinWeights = true;  // 가중치 16비트(합 65535)
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkinnedMeshBuildSettings : public MeshBuildSettings {
    ROCKS_GENERATED_BODY()

    // 쿠킹 가중치를 16비트(합 65535)로 둔다. 거짓이면 8비트(합 255)다.
    ROCKS_PROPERTY() bool useHighPrecisionSkinWeights = false;
    // 렌더 정점 용접에서 두 위치가 같다고 보는 축별 거리다(removeDegenerates면 퇴화 판정도 이 값).
    // 유한한 0 이상이어야 한다.
    ROCKS_PROPERTY() float thresholdPosition = 0.00002f;
    // 렌더 정점 용접에서 두 법선·tangent·쌍법선이 같다고 보는 축별 거리다.
    ROCKS_PROPERTY() float thresholdTangentNormal = 0.00002f;
    // 렌더 정점 용접에서 두 UV가 같다고 보는 축별 거리다(UE 0.0009765625 = 1/1024).
    ROCKS_PROPERTY() float thresholdUV = 0.0009765625f;
    // morph 위치 변화량 길이가 이 값 이하인 렌더 정점은 그 morph target에서 뺀다.
    ROCKS_PROPERTY() float morphThresholdPosition = 0.015f;
    // 정점마다 남길 최대 영향 수다. 0이면 기본 상한(8)이고, 8을 넘는 값도 8이다. 0 이상이어야 한다.
    ROCKS_PROPERTY() int32 boneInfluenceLimit = 0;
};

// 빌드 설정 필드 하나를 가리키는 번호다(UnsupportedSetting 진단의 위치 — MeshDiagnostic::index).
// 값은 저장 형식이 아니다(순서를 바꿔도 된다).
enum class MeshBuildSettingField : uint32 {
    RecomputeNormals,
    RecomputeTangents,
    UseMikkTSpace,
    ComputeWeightedNormals,
    RemoveDegenerates,
    BuildReversedIndexBuffer,
    UseHighPrecisionTangentBasis,
    UseFullPrecisionUVs,
    GenerateLightmapUVs,
    MinLightmapResolution,
    SrcLightmapIndex,
    DstLightmapIndex,
    BuildScale,
    GenerateDistanceFieldAsIfTwoSided,
    DistanceFieldResolutionScale,
    MaxLumenMeshCards,
    SupportFaceRemap,
    // SkinnedMeshBuildSettings가 더한 필드(스킨 빌드만 쓴다).
    UseHighPrecisionSkinWeights,
    ThresholdPosition,
    ThresholdTangentNormal,
    ThresholdUV,
    MorphThresholdPosition,
    BoneInfluenceLimit
};

// MeshBuildSettings 필드 수다(MeshBuildSettingField의 앞 부분 —
// RecomputeNormals..SupportFaceRemap).
inline constexpr uint32 MeshBuildSettingFieldCount = 17;

// SkinnedMeshBuildSettings 필드 수다(MeshBuildSettingField 전체).
inline constexpr uint32 SkinnedMeshBuildSettingFieldCount = 23;

// 필드 이름이다(리플렉션 필드 이름과 같다 — 예: GenerateLightmapUVs → "generateLightmapUVs").
// 모르는 값은 "Unknown"이다. 반환 문자열은 정적 수명이다(진단 줄·로그가 쓴다).
ROCKS_MESH_API const char* MeshBuildSettingFieldName(MeshBuildSettingField field) noexcept;

// 정적 메시 빌드에서 4.1절 "보존" 필드인지다(빌드가 소비하지 않고 기본값이 아니면
// UnsupportedSetting). 스킨 필드와 모르는 값은 false다.
ROCKS_MESH_API bool IsPreservedMeshBuildSetting(MeshBuildSettingField field) noexcept;

// 스킨 메시 빌드에서 보존 필드인지다: 정적 메시의 보존 필드에 buildScale을 더한다(6.5절 3). 스킨
// 필드와 모르는 값은 false다.
ROCKS_MESH_API bool IsPreservedSkinnedMeshBuildSetting(MeshBuildSettingField field) noexcept;

// settings의 field가 기본값(MeshBuildSettings{}의 값)과 다른지다. 실수 필드는 비트 단위로 비교한다
// (-0.0도 기본값 +0.0과 다르다 — 저장 byte가 다르다). 스킨 필드와 모르는 값은 false다.
ROCKS_MESH_API bool IsMeshBuildSettingChanged(const MeshBuildSettings& settings,
                                              MeshBuildSettingField field) noexcept;

// settings의 field가 기본값(SkinnedMeshBuildSettings{}의 값)과 다른지다(공통 필드는 위 함수와
// 같고 스킨 필드도 본다). 모르는 값은 false다.
ROCKS_MESH_API bool IsMeshBuildSettingChanged(const SkinnedMeshBuildSettings& settings,
                                              MeshBuildSettingField field) noexcept;

// Mesh 리플렉션 타입(MeshBuildSettings·SkinnedMeshBuildSettings)을 등록한다(생성 집계 함수
// generated::Mesh::RegisterModuleTypes를 부른다 — 의존 모듈 타입도 함께). 도구·시험은 직접, 엔진은
// 메시 에셋 모듈이 부른다.
ROCKS_MESH_API Result<void> RegisterMeshTypes(ModuleRegistration& registration) noexcept;

} // namespace rocks
