#pragma once

#include <AnimationTools/SkeletonImport.h>
#include <Object/AssetId.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Vector3.h>
#include <Mesh/Common/MeshSectionInfoEntry.h>
#include <Mesh/Common/VirtualGeometrySettings.h>
#include <MeshTools/SkeletalMeshImportLod.h>
#include <MeshTools/SkeletalMeshImportProfile.h>
#include <MeshTools/StaticMeshImport.h>

// 스킨 메시 임포트 명세다(Docs/Design/MeshAnimationAssets 6.7.3절 1·5). 기본값은 SkeletalMesh
// 기본값과 같다. span은 ImportSkeletalMesh가 끝날 때까지 산다.
//
// 사용 예(임포터 Import 안 — 새 스켈레톤을 보조 출력으로 만든다):
//   const SkeletonImportBone bones[] = {{"root", NoParentBone}, {"spine", 0}};
//   SkeletalMeshImportLod lod;
//   lod.source = &source;                   // 정점 SkinWeights의 boneIndex = bones 번호
//   SkeletalMeshImportSpec spec;
//   spec.lods = Span<const SkeletalMeshImportLod>(&lod, 1);
//   spec.bones = Span<const SkeletonImportBone>(bones);
//   auto output = context.Output({}, TypeIdOf<SkeletalMesh>());
//   if (!output) { return Result<void>::Failure(output.Error()); }
//   return ImportSkeletalMesh(context, output.Value(), spec);
namespace rocks {

struct SkeletalMeshImportSpec {
    Span<const SkeletalMeshImportLod> lods{};
    // 메시 본(본 번호 순 — 6.6절과 같은 타입)이다.
    Span<const SkeletonImportBone> bones{};
    Span<const StaticMeshImportSlot> slots{};
    // (lod, section) 엄격 증가다.
    Span<const MeshSectionInfoEntry> sectionInfo{};
    // 메시 소켓이다(본 이름은 메시 본이어야 한다).
    Span<const SkeletonImportSocket> sockets{};
    Span<const SkeletalMeshImportProfile> skinWeightProfiles{};
    Vec3d positiveBoundsExtension{};
    Vec3d negativeBoundsExtension{};
    int32 minLod = 0;
    Span<const StaticMeshImportInt> minLodPerPlatform{};
    VirtualGeometrySettings virtualGeometry;
    // 참조할 스켈레톤이다. null이면 서비스가 보조 출력 "Skeleton"을 메시 본으로 만든다(UE
    // 스켈레톤을 만들며 임포트). 아니면 그 스켈레톤에 메시 본을 맞춘다(모자란 본은 EditAsset으로
    // 더한다 — 묶음 구성원 스켈레톤이면 실패, 6.7.3절 3).
    AssetId skeleton{};
    // 재임포트에서도 설정(LOD 설정·가중치 프로필 설정·소켓·bounds 확장·minLod·virtualGeometry)을
    // 명세 값으로 바꾼다. 거짓이면 재임포트가 옛 설정을 보존한다(UE와 같다).
    bool replaceSettingsOnReimport = false;
};

} // namespace rocks
