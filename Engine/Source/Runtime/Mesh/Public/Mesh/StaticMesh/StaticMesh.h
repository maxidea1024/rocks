#pragma once

#include <AssetRuntime/PerPlatformInt32.h>
#include <AssetRuntime/StreamableRenderAsset.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/Name.h>
#include <Mesh/Common/MeshMaterialSlot.h>
#include <Mesh/Common/MeshSectionInfoEntry.h>
#include <Mesh/Common/MeshSocket.h>
#include <Mesh/Common/VirtualGeometrySettings.h>
#include <Mesh/SimpleCollision.h>
#include <Mesh/StaticMesh/StaticMeshRenderData.h>
#include <Mesh/StaticMesh/StaticMeshSourceLod.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

// 정적 메시 에셋 클래스다(Docs/Design/MeshAnimationAssets 5.1절·5.5.1절 3, UE UStaticMesh). 필드
// 이름은 Rocks 규칙(camelCase, bool에 b 접두 없음)이고 기본값은 UE 생성자 값이다. EditorOnly 구분은
// UE WITH_EDITORONLY_DATA 구간과 같다.
namespace rocks {

// 한 메시의 최대 LOD 수다(UE MAX_STATIC_MESH_LODS).
inline constexpr uint32 StaticMeshMaxLods = 8;

// 머티리얼 슬롯 수 상한이다(섹션 슬롯 번호를 16비트 안에 — MeshMaxPolygonGroups와 같은 값).
inline constexpr uint32 StaticMeshMaxMaterialSlots = 65535;

// UE 생성자의 lightmap 해상도 기본값이다(SetLightMapResolution(4)).
inline constexpr int32 StaticMeshDefaultLightMapResolution = 4;

// numStreamedLods의 "LOD 그룹 기본값" 표지다(UE NumStreamedLODs.Default = -1).
inline constexpr int32 StaticMeshGroupStreamedLods = -1;

// 정적 메시다. 에디터 패키지는 원본 LOD·설정을 갖고 renderData가 비어 있다. 쿠킹 패키지는
// EditorOnly 필드가 빠지고 renderData를 쿠킹 처리기가 채운다(MA2b2). 쿠킹 데이터는 소비자가 쓰기
// 전에 ValidateStaticMesh로 검사한다(Mesh/StaticMesh/StaticMeshValidation.h).
//
// 사용 예:
//   const StaticMesh& mesh = *borrowed.Value().Get().Value();
//   for (const StaticMeshRenderLod& lod : mesh.renderData.lods) { ... }   // 쿠킹 LOD
//
// 스레드: 에셋 객체 Registry의 소유 스레드 전용이다.
ROCKS_CLASS()
class ROCKS_MESH_API StaticMesh final : public StreamableRenderAsset {
    ROCKS_GENERATED_BODY()

public:
    explicit StaticMesh(Allocator& allocator) noexcept
        : sourceLods(allocator), sectionInfo(allocator), materialSlots(allocator),
          sockets(allocator), simpleCollision(allocator), minLod(allocator),
          numStreamedLods(allocator), renderData(allocator) {
        numStreamedLods.defaultValue = StaticMeshGroupStreamedLods;
    }

    // ---- 에디터 데이터(UE WITH_EDITORONLY_DATA) ----
    ROCKS_PROPERTY(EditorOnly) Array<StaticMeshSourceLod> sourceLods;
    // 섹션 정보 표다((lod, section) 엄격 증가 — UE SectionInfoMap).
    ROCKS_PROPERTY(EditorOnly) Array<MeshSectionInfoEntry> sectionInfo;
    ROCKS_PROPERTY(EditorOnly) bool autoComputeLodScreenSize = true;
    // 메시 LOD 그룹 이름이다(None = 그룹 없음, 표는 TargetProfile 메시 LOD 그룹 — 보존).
    ROCKS_PROPERTY(EditorOnly) Name lodGroup;
    // 가상 기하 설정이다(UE NaniteSettings — 보존).
    ROCKS_PROPERTY(EditorOnly) VirtualGeometrySettings virtualGeometry;
    // 복잡 충돌에 쓸 다른 메시다(UE ComplexCollisionMesh — 보존, 물리 쿠킹 단계에서 Build 의존).
    ROCKS_PROPERTY(EditorOnly) ObjectRef<StaticMesh> complexCollisionMesh;
    // distance field 대체 메시다(UE DistanceFieldReplacementMesh — 보존, GI 단계에서 Build 의존).
    ROCKS_PROPERTY(EditorOnly) ObjectRef<StaticMesh> distanceFieldReplacementMesh;

    // ---- 저장(쿠킹에도 남음) ----
    ROCKS_PROPERTY() StructRefArray<MeshMaterialSlot> materialSlots;
    ROCKS_PROPERTY() Array<MeshSocket> sockets;
    ROCKS_PROPERTY() SimpleCollision simpleCollision;
    ROCKS_PROPERTY() CollisionComplexity complexity = CollisionComplexity::Default;
    // 충돌에 쓸 LOD 번호다(쓰는 쪽이 [0, LOD 수 − 1]로 자른다 — UE와 같다).
    ROCKS_PROPERTY() int32 lodForCollision = 0;
    // 충돌을 양면으로 다룬다(UE BodySetup bDoubleSidedGeometry).
    ROCKS_PROPERTY() bool doubleSidedGeometry = false;
    // lightmap 설정이다(보존 — 라이트맵 단계).
    ROCKS_PROPERTY() int32 lightMapResolution = StaticMeshDefaultLightMapResolution;
    ROCKS_PROPERTY() int32 lightMapCoordinateIndex = 0;
    // bounds 확장이다(cm — 확장 bounds는 StaticMeshExtendedBounds가 계산한다).
    ROCKS_PROPERTY() Vec3d positiveBoundsExtension {};
    ROCKS_PROPERTY() Vec3d negativeBoundsExtension {};
    // 최소 LOD·스트리밍 LOD 수다(쿠킹은 대상 프로파일 값 — 5.5.2절 9).
    ROCKS_PROPERTY() PerPlatformInt32 minLod;
    ROCKS_PROPERTY() PerPlatformInt32 numStreamedLods;

    // ---- 쿠킹 데이터(쿠킹 처리기가 채운다 — MA2b2) ----
    ROCKS_PROPERTY() StaticMeshRenderData renderData;
};

// Mesh 모듈 리플렉션 타입 등록은 RegisterMeshTypes(Mesh/MeshBuildSettings.h)다.

} // namespace rocks
