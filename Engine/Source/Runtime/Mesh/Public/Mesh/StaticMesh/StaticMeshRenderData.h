#pragma once

#include <Core/Containers/Array.h>
#include <Core/Math/Shapes.h>
#include <Core/Memory/Allocator.h>
#include <Mesh/StaticMesh/StaticMeshRenderLod.h>

// 정적 메시 쿠킹 렌더 데이터다(Docs/Design/MeshAnimationAssets 5.3절, UE FStaticMeshRenderData).
// 에디터 패키지에서는 비어 있고 쿠킹 처리기가 채운다(MA2b2). 장치 없는 프로파일(전용 서버)의 쿠킹
// 패키지에서도 비어 있다(5.5.2절 3).
namespace rocks {

// 쿠킹 LOD 목록과 bounds다. bounds는 쿠킹 LOD 0 빌드 결과(경계 확장 없음 — UE와 같다, 확장 bounds는
// StaticMeshExtendedBounds)이고 구 중심은 상자 중심이다(UE FBoxSphereBounds). 길이는 cm다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct StaticMeshRenderData {
    ROCKS_GENERATED_BODY()

    explicit StaticMeshRenderData(Allocator& allocator) noexcept : lods(allocator) {
    }

    ROCKS_PROPERTY() Array<StaticMeshRenderLod> lods;
    ROCKS_PROPERTY() Box<double> box {};
    ROCKS_PROPERTY() double sphereRadius = 0.0;
};

} // namespace rocks
