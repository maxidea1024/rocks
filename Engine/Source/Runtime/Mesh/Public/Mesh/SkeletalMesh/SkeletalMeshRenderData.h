#pragma once

#include <AssetRuntime/BulkData.h>
#include <Core/Containers/Array.h>
#include <Core/Math/Shapes.h>
#include <Core/Memory/Allocator.h>
#include <Mesh/SkeletalMesh/SkinnedRenderLod.h>

// 스킨 메시 쿠킹 렌더 데이터다(Docs/Design/MeshAnimationAssets 6.7.1절 6, UE
// FSkeletalMeshRenderData). 에디터 패키지에서는 비어 있고 쿠킹 처리기가 채운다(MA5b2).
namespace rocks {

// inverse-bind 행렬 하나의 byte 수다(float 16개 — 행 벡터 규약, 행 우선).
inline constexpr uint64 SkinnedInverseBindMatrixBytes = 16 * sizeof(float);

// 쿠킹 LOD 목록, bounds(쿠킹 LOD 0 빌드 결과 — 확장 없음, 정적과 같다), 메시 본마다의 inverse-bind
// 행렬(UE RefBasesInvMatrix와 같은 값 — 쿠킹이 double로 계산해 float로 둔다)이다. 길이는 cm다.
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct SkeletalMeshRenderData {
    ROCKS_GENERATED_BODY()

    explicit SkeletalMeshRenderData(Allocator& allocator) noexcept : lods(allocator) {
    }

    ROCKS_PROPERTY() Array<SkinnedRenderLod> lods;
    ROCKS_PROPERTY() Box<double> box {};
    ROCKS_PROPERTY() double sphereRadius = 0.0;
    // 메시 본 수 × SkinnedInverseBindMatrixBytes byte다(쿠킹 패키지 bulk). 에디터 패키지에서는
    // 빈다.
    ROCKS_PROPERTY() BulkDataRef inverseBindMatrices;
};

} // namespace rocks
