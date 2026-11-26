#pragma once

#include <AssetRuntime/PerPlatformFloat.h>
#include <Core/Containers/Array.h>
#include <Core/Text/Name.h>
#include <Mesh/MeshBuildSettings.h>
#include <Mesh/SkeletalMesh/SkeletalMeshReductionSettings.h>
#include <MeshSource/MeshSourceData.h>

// 스킨 메시 원본 LOD 하나다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 4, UE
// FSkeletalMeshLODInfo + FSkeletalMeshSourceModel — SkinnedAssetCommon.h). 에디터 데이터라
// SkeletalMesh가 EditorOnly 배열로 갖는다.
namespace rocks {

// lodMaterialMap 원소의 "덮어쓰지 않음" 표지다(UE LODMaterialMap의 INDEX_NONE).
inline constexpr int32 SkeletalMeshNoMaterialRemap = -1;

// 원본 메시·스킨 빌드 설정·축소 설정·화면 크기·LOD 전환 여유·섹션 머티리얼 덮어쓰기·본 제거
// 설정이다. hasSource·hasBakedSource는 정적 메시와 같다(5.5.1절 6). 축소·본 제거는 보존
// 설정이다(6.3절 — 빌드가 아직 쓰지 않는다).
//
// 사용 예:
//   SkeletalMeshSourceLod lod(allocator);
//   lod.hasSource = true;
//   auto stored = mesh.Store(bulk, names, lod.source);   // MeshSource → 영속 형태
//   lod.screenSize.defaultValue = 0.3f;
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkeletalMeshSourceLod {
    ROCKS_GENERATED_BODY()

    // UE FSkeletalMeshLODInfo 기본값(SkinnedAssetCommon.h — ScreenSize{1.0}, WeightOfPrioritization
    // 1)이다.
    static constexpr float DefaultScreenSize = 1.0f;
    static constexpr float DefaultWeightOfPrioritization = 1.0f;

    explicit SkeletalMeshSourceLod(Allocator& allocator) noexcept
        : source(allocator), bakedSource(allocator), screenSize(allocator),
          lodMaterialMap(allocator), bonesToRemove(allocator), bonesToPrioritize(allocator) {
        screenSize.defaultValue = DefaultScreenSize;
    }

    ROCKS_PROPERTY() bool hasSource = false;
    ROCKS_PROPERTY() MeshSourceData source;
    ROCKS_PROPERTY() bool hasBakedSource = false;
    ROCKS_PROPERTY() MeshSourceData bakedSource;
    ROCKS_PROPERTY() SkinnedMeshBuildSettings build;
    ROCKS_PROPERTY() SkeletalMeshReductionSettings reduction;
    // LOD 화면 크기다(UE ScreenSize — 스킨 메시는 자동 계산이 없다).
    ROCKS_PROPERTY() PerPlatformFloat screenSize;
    // LOD를 바꿀 때의 화면 크기 여유다(UE LODHysteresis).
    ROCKS_PROPERTY() float lodHysteresis = 0.0f;
    // 섹션 번호 → 머티리얼 슬롯 덮어쓰기다(SkeletalMeshNoMaterialRemap = 그대로 — UE
    // LODMaterialMap). 쿠킹이 섹션 materialIndex에 굽는다(6.7.1절 6).
    ROCKS_PROPERTY() Array<int32> lodMaterialMap;
    // 보존(축소 단계): 뺄 본·우선할 본과 우선 가중치(UE BonesToRemove·BonesToPrioritize·
    // WeightOfPrioritization).
    ROCKS_PROPERTY() Array<Name> bonesToRemove;
    ROCKS_PROPERTY() Array<Name> bonesToPrioritize;
    ROCKS_PROPERTY() float weightOfPrioritization = DefaultWeightOfPrioritization;
};

// lodIndex번 원본 LOD가 생성 LOD인지다(UE USkeletalMesh::IsReductionActive — SkeletalMesh.cpp:623):
// 원본이 없거나 축소 설정이 원본을 줄인다. inline 축소(reduction.baseLod == lodIndex)면 원본의
// 정점·삼각형 수로, 아니면 수를 모르는 것(uint32 최댓값 — 비율 기준은 비율만, 절대 기준은 늘
// 참)으로 판정한다.
ROCKS_MESH_API bool IsSkeletalMeshLodGenerated(const SkeletalMeshSourceLod& lod,
                                               int32 lodIndex) noexcept;

} // namespace rocks
