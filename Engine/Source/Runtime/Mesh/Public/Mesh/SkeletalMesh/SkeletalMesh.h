#pragma once

#include <Animation/BoneHierarchy.h>
#include <Animation/Skeleton.h>
#include <AssetRuntime/PerPlatformInt32.h>
#include <Base/Compiler.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Vector3.h>
#include <Mesh/Common/MeshMaterialSlot.h>
#include <Mesh/Common/MeshSectionInfoEntry.h>
#include <Mesh/Common/VirtualGeometrySettings.h>
#include <Mesh/SkeletalMesh/SkeletalMeshRenderData.h>
#include <Mesh/SkeletalMesh/SkeletalMeshSourceLod.h>
#include <Mesh/SkeletalMesh/SkinWeightProfileInfo.h>
#include <Mesh/SkeletalMesh/SkinnedAsset.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 스킨 메시 에셋이다(Docs/Design/MeshAnimationAssets 6.3절·6.7.1절 3, UE USkeletalMesh).
namespace rocks {

// 스킨 메시 원본 LOD 수 상한이다(UE MAX_SKELETAL_MESH_LODS — 정적 메시 StaticMeshMaxLods와 같은 8).
inline constexpr uint32 SkeletalMeshMaxLods = 8;

// 머티리얼 슬롯 수 상한이다(섹션 materialIndex가 16비트 — UE FSkelMeshRenderSection::MaterialIndex
// uint16, 정적 메시와 같다).
inline constexpr uint32 SkeletalMeshMaxMaterialSlots = 65535;

// 스킨 메시다. 에디터 패키지는 원본 LOD·설정을 갖고 renderData가 비어 있다. 쿠킹 패키지는
// EditorOnly 필드가 빠지고 renderData를 쿠킹 처리기가 채운다(MA5b2). bones는 메시 자체 본
// 계층이다(스켈레톤의 부분집합, 순서가 달라도 된다 — 메시 본 ↔ 스켈레톤 본 대응은 로드 때 이름으로
// 만든다, 6.3절). 쿠킹 데이터는 소비자가 쓰기 전에 ValidateSkeletalMesh로
// 검사한다(Mesh/SkeletalMesh/SkeletalMeshValidation.h).
//
// 사용 예:
//   const SkeletalMesh& mesh = *borrowed.Value().Get().Value();
//   for (const SkinnedRenderLod& lod : mesh.renderData.lods) { ... }   // 쿠킹 LOD
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MESH_API SkeletalMesh final : public SkinnedAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit SkeletalMesh(Allocator& allocator) noexcept
        : bones(allocator),
#if ROCKS_WITH_EDITOR_DATA
          sourceLods(allocator), sectionInfo(allocator),
#endif
          materialSlots(allocator), sockets(allocator), skinWeightProfiles(allocator),
          minLod(allocator), renderData(allocator) {
    }

    // 스켈레톤이다(Hard — 쿠킹 폐포에 들어간다. 쿠킹 결과는 메시 본만 쓰므로 Build 의존은 없다 —
    // 6.7.2절 3).
    ROCKS_PROPERTY() ObjectRef<Skeleton> skeleton;
    // 메시 본 계층이다(UE RefSkeleton).
    ROCKS_PROPERTY() BoneHierarchy bones;

    // ---- 에디터 데이터 ----
#if ROCKS_WITH_EDITOR_DATA
    // 원본 LOD다(UE SourceModels·LODInfo는 쿠킹에 남지만 Rocks는 런타임 값을 쿠킹 LOD로 굽는다
    // — 6.7.1절 3).
    ROCKS_PROPERTY(EditorOnly) Array<SkeletalMeshSourceLod> sourceLods;
    // 섹션 정보 표다((lod, section) 엄격 증가 — UE FSkeletalMeshLODModel::UserSectionsData, 정적
    // 메시와 같은 대체 규칙).
    ROCKS_PROPERTY(EditorOnly) Array<MeshSectionInfoEntry> sectionInfo;
    // 가상 기하 설정이다(UE NaniteSettings — 보존).
    ROCKS_PROPERTY(EditorOnly) VirtualGeometrySettings virtualGeometry;
#endif

    // ---- 저장(쿠킹에도 남음) ----
    // 머티리얼 슬롯이다(UE FSkeletalMaterial과 같은 필드).
    ROCKS_PROPERTY() StructRefArray<MeshMaterialSlot> materialSlots;
    // 메시 전용 소켓이다(스켈레톤 소켓과 합쳐 쓴다 — UE USkeletalMesh::Sockets).
    ROCKS_PROPERTY() Array<BoneSocket> sockets;
    ROCKS_PROPERTY() Array<SkinWeightProfileInfo> skinWeightProfiles;
    // bounds 확장이다(cm — 확장 bounds는 SkeletalMeshExtendedBounds가 계산한다).
    ROCKS_PROPERTY() Vec3d positiveBoundsExtension {};
    ROCKS_PROPERTY() Vec3d negativeBoundsExtension {};
    // 최소 LOD다(쿠킹은 대상 프로파일 값 — UE MinLod).
    ROCKS_PROPERTY() PerPlatformInt32 minLod;

    // ---- 쿠킹 데이터(쿠킹 처리기가 채운다 — MA5b2) ----
    ROCKS_PROPERTY() SkeletalMeshRenderData renderData;
};

} // namespace rocks
