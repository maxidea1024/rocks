#pragma once

#include <AssetRuntime/PerPlatformFloat.h>
#include <Mesh/Common/MeshReductionSettings.h>
#include <Mesh/MeshBuildSettings.h>
#include <MeshSource/MeshSourceData.h>

// 정적 메시 원본 LOD 하나다(Docs/Design/MeshAnimationAssets 5.1절·5.5.1절 6, UE
// FStaticMeshSourceModel — StaticMeshSourceData.h). 에디터 데이터라 StaticMesh가 EditorOnly 배열로
// 갖는다.
namespace rocks {

// 원본 메시·빌드 설정·축소 설정·화면 크기다. 원본이 없는 LOD(UE에서 축소로만 만드는 LOD)는
// hasSource가 거짓이고, 다른 엔진이 축소로 만든 결과를 임포터가 함께 주면 hasBakedSource가 참이다
// (교환 경로 — 빈 MeshSourceData도 정상 메시라 "없음"은 플래그로 나타낸다, 3.4.2절).
//
// 사용 예:
//   StaticMeshSourceLod lod(allocator);
//   lod.hasSource = true;
//   auto stored = mesh.Store(bulk, names, lod.source);   // MeshSource → 영속 형태
//   lod.screenSize.defaultValue = 0.5f;
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct StaticMeshSourceLod {
    ROCKS_GENERATED_BODY()

    explicit StaticMeshSourceLod(Allocator& allocator) noexcept
        : source(allocator), bakedSource(allocator), screenSize(allocator) {
    }

    ROCKS_PROPERTY() bool hasSource = false;
    ROCKS_PROPERTY() MeshSourceData source;
    ROCKS_PROPERTY() bool hasBakedSource = false;
    ROCKS_PROPERTY() MeshSourceData bakedSource;
    ROCKS_PROPERTY() MeshBuildSettings build;
    ROCKS_PROPERTY() MeshReductionSettings reduction;
    // LOD 화면 크기다(UE ScreenSize — 기본값 0, autoComputeLodScreenSize면 쓰지 않는다).
    ROCKS_PROPERTY() PerPlatformFloat screenSize;
};

// lod가 생성 LOD인지다(UE !IsMeshDescriptionValid(i) || IsReductionActive(i) — StaticMesh.cpp):
// 원본이 없거나 축소 설정이 원본(vertexCount·triangleCount)을 줄인다. UE의 정점 수는 속성까지
// 용접한 수라 다를 수 있다(5.5.1절 6).
//
// 사용 예:
//   if (IsStaticMeshLodGenerated(lod) && lod.hasBakedSource) { /* baked 원본으로 빌드 */ }
ROCKS_MESH_API bool IsStaticMeshLodGenerated(const StaticMeshSourceLod& lod) noexcept;

} // namespace rocks
