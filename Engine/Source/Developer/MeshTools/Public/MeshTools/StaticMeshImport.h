#pragma once

#include <AssetDatabase/AssetImporter.h>
#include <Core/Containers/Span.h>
#include <Core/Math/Rotator.h>
#include <Core/Math/Vector3.h>
#include <Core/Text/StringView.h>
#include <Mesh/StaticMesh/StaticMesh.h>
#include <MeshSource/MeshSource.h>

// 정적 메시 임포트 서비스다(Docs/Design/MeshAnimationAssets 5.5.3절·10절 — MA2b3). 파일 형식을 읽는
// 임포터(교환 형식 UX3b, 뒤의 glTF·FBX)가 AssetImporter::Import 안에서 메모리 MeshSource와 값으로
// StaticMeshImportSpec을 만들어 부르면, 서비스가 Output()으로 받은 StaticMesh 객체를 채운다(원본
// bulk는 AddBulk, 참조는 OpenAsset). 텍스처 TextureImportSpec과 같은 역할이다.
namespace rocks {

// 대상 프로파일별 값 하나다(PerPlatformFloat·PerPlatformInt32의 perPlatform 항목 — profile은
// TargetProfile id).
struct StaticMeshImportFloat {
    StringView profile{};
    float value = 0.0f;
};

struct StaticMeshImportInt {
    StringView profile{};
    int32 value = 0;
};

// 원본 LOD 하나다. source·bakedSource는 null이면 없음이고 둘 중 하나는 있어야 한다. 포인터와
// span은 ImportStaticMesh가 끝날 때까지 산다.
struct StaticMeshImportLod {
    const MeshSource* source = nullptr;
    const MeshSource* bakedSource = nullptr;
    MeshBuildSettings build;
    MeshReductionSettings reduction;
    float screenSize = 0.0f;
    Span<const StaticMeshImportFloat> screenSizePerPlatform{};
};

// 머티리얼 슬롯 하나다(material·overlayMaterial은 null이면 비움 — 그 밖은 MaterialBase 파생 에셋).
struct StaticMeshImportSlot {
    StringView slotName{};
    StringView importedSlotName{};
    AssetId material{};
    AssetId overlayMaterial{};
    MeshUvDensity uvDensity;
};

// 소켓 하나다(MeshSocket 필드 — createdAtImport 기본값은 참이다: 임포터가 만든 소켓이다. MeshSocket
// 기본값(거짓)은 편집기에서 만든 소켓의 값이다).
struct StaticMeshImportSocket {
    StringView name{};
    Vec3d relativeLocation{};
    Rotator<double> relativeRotation{};
    Vec3d relativeScale = Vec3d(1.0, 1.0, 1.0);
    StringView tag{};
    bool createdAtImport = true;
};

// 정적 메시 임포트 명세다(5.5.3절 2). 기본값은 StaticMesh 기본값과 같다.
//
// 사용 예(임포터 Import 안):
//   MeshSource source(allocator);           // 파일 형식에서 채운다
//   StaticMeshImportLod lod;
//   lod.source = &source;
//   StaticMeshImportSpec spec;
//   spec.lods = Span<const StaticMeshImportLod>(&lod, 1);
//   auto output = context.Output({}, TypeIdOf<StaticMesh>());
//   if (!output) { return Result<void>::Failure(output.Error()); }
//   return ImportStaticMesh(context, output.Value(), spec);
struct StaticMeshImportSpec {
    Span<const StaticMeshImportLod> lods{};
    Span<const StaticMeshImportSlot> slots{};
    // (lod, section) 엄격 증가다.
    Span<const MeshSectionInfoEntry> sectionInfo{};
    Span<const StaticMeshImportSocket> sockets{};
    // 단순 충돌 값(null이면 없음)과 볼록 형상마다의 점·index다(convexes[i] = collision->convexes[i]
    // — 그 struct의 vertices·indices ref는 서비스가 채운다).
    const SimpleCollision* collision = nullptr;
    Span<const ConvexGeometry> convexes{};
    CollisionComplexity complexity = CollisionComplexity::Default;
    int32 lodForCollision = 0;
    bool doubleSidedGeometry = false;
    AssetId complexCollisionMesh{};
    AssetId distanceFieldReplacementMesh{};
    int32 lightMapResolution = StaticMeshDefaultLightMapResolution;
    int32 lightMapCoordinateIndex = 0;
    Vec3d positiveBoundsExtension{};
    Vec3d negativeBoundsExtension{};
    bool autoComputeLodScreenSize = true;
    StringView lodGroup{};
    int32 minLod = 0;
    Span<const StaticMeshImportInt> minLodPerPlatform{};
    int32 numStreamedLods = StaticMeshGroupStreamedLods;
    Span<const StaticMeshImportInt> numStreamedLodsPerPlatform{};
    VirtualGeometrySettings virtualGeometry;
    // 재임포트에서도 설정(LOD 빌드·축소·화면 크기, 에셋 설정·참조 둘)을 명세 값으로 바꾼다(교환
    // 경로처럼 원본 쪽이 설정의 정본일 때). 거짓이면 재임포트가 옛 설정을 보존한다(UE와 같다).
    bool replaceSettingsOnReimport = false;
};

// spec으로 output(context.Output이 준 StaticMesh)을 채운다(5.5.3절). 먼저 모두 검증하고(LOD 수
// 1–StaticMeshMaxLods → LOD마다 원본이나 baked → 원본마다 ValidateMeshSource → 섹션 정보 순서·LOD
// 범위 → 슬롯 수 → 이름·프로파일 키 → 단순 충돌·볼록 형상 → 참조 타입) 그다음에 원본·볼록 형상
// bulk를 context.AddBulk로 넘기고 필드를 쓴다. 새 임포트는 임포트가 정하는 필드를 모두 쓴다.
// 재임포트(context.IsReimport())는 사용자 편집을 보존한다(5.5.3절 6 — importedSlotName이 같은 옛
// 슬롯의 slotName·머티리얼·UV 밀도, 편집기에서 만든 소켓, LOD·에셋 설정;
// replaceSettingsOnReimport면 설정은 명세 값). renderData는 비운다. 값 정리는 하지 않는다(빌드 몫
// — 3.3절).
//
// 오류(진단은 context.Fail): LOD 수·원본 없음·섹션 정보·슬롯 수·이름(intern의 원래 오류)·프로파일
// 키 (비었거나 겹침) InvalidSetting(detail = 위치), 원본 검증 실패는 ValidateMeshSource의
// 코드(detail = LOD — meshFailure가 null이 아니면 세부 진단도 채운다), 충돌·볼록 형상 수
// InvalidCollisionData(detail = 형상 번호), 참조 타입 TypeMismatch(detail = 슬롯 i의 material
// 2i·overlay 2i+1, 그 뒤 complexCollisionMesh·distanceFieldReplacementMesh), 참조 열기 실패는
// OpenAsset의 오류(진단은 문맥 — 자기 자신은 InvalidArgument), output이 StaticMesh가 아님
// TypeMismatch, 할당 실패. 실패하면 output은 부분적으로 바뀔 수 있다(서비스가 출력을 버린다 —
// AssetImporter 계약).
ROCKS_MESHTOOLS_API Result<void> ImportStaticMesh(AssetImportContext& context, ObjectHandle output,
                                                  const StaticMeshImportSpec& spec,
                                                  MeshDiagnostic* meshFailure = nullptr) noexcept;

} // namespace rocks
