#pragma once

#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <Mesh/MeshBuildSettings.h>
#include <Mesh/SkeletalMesh/SkeletalMeshReductionSettings.h>
#include <Mesh/SkeletalMesh/SkeletalMeshSourceLod.h>
#include <MeshSource/MeshSource.h>
#include <MeshTools/StaticMeshImport.h>

// 스킨 메시 임포트 명세의 원본 LOD 하나다(Docs/Design/MeshAnimationAssets 6.7.3절 5 — UE
// FSkeletalMeshLODInfo + FSkeletalMeshSourceModel). 기본값은 SkeletalMeshSourceLod와 같다.
namespace rocks {

// source·bakedSource는 null이면 없음이고 둘 중 하나는 있어야 한다. 포인터와 span은
// ImportSkeletalMesh가 끝날 때까지 산다. 원본의 정점 가중치 boneIndex는 명세 bones 번호다.
struct SkeletalMeshImportLod {
    const MeshSource* source = nullptr;
    const MeshSource* bakedSource = nullptr;
    SkinnedMeshBuildSettings build;
    SkeletalMeshReductionSettings reduction;
    float screenSize = SkeletalMeshSourceLod::DefaultScreenSize;
    Span<const StaticMeshImportFloat> screenSizePerPlatform{};
    float lodHysteresis = 0.0f;
    // 섹션 번호 → 슬롯 덮어쓰기(SkeletalMeshNoMaterialRemap = 그대로 — −1 이상이어야 한다).
    Span<const int32> lodMaterialMap{};
    // 본 이름이다(축소 보존 값 — 메시 본에 없어도 된다, UE와 같다).
    Span<const StringView> bonesToRemove{};
    Span<const StringView> bonesToPrioritize{};
    float weightOfPrioritization = SkeletalMeshSourceLod::DefaultWeightOfPrioritization;
};

} // namespace rocks
