#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 쿠킹 렌더 LOD의 섹션이다(Docs/Design/MeshAnimationAssets 5.3절, UE FStaticMeshSection —
// StaticMeshResources.h). 정적·스킨 메시의 쿠킹 LOD가 함께 쓴다.
namespace rocks {

// 같은 머티리얼 슬롯으로 그리는 삼각형 구간이다. 섹션 정보 여섯 값을 담는다(UE ResolveSectionInfo가
// 섹션에 옮기는 값 — materialIndex·castShadow는 렌더러가, 나머지 넷은 물리·레이 트레이싱·GI 단계가
// 쓴다).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshRenderSection {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() uint32 materialIndex = 0;
    // index 구간 안 시작 index와 삼각형 수다(빈 섹션은 0, 0).
    ROCKS_PROPERTY() uint32 firstIndex = 0;
    ROCKS_PROPERTY() uint32 triangleCount = 0;
    // 섹션 index가 가리키는 정점 번호의 최소·최대다(빈 섹션은 0, 0).
    ROCKS_PROPERTY() uint32 minVertex = 0;
    ROCKS_PROPERTY() uint32 maxVertex = 0;
    ROCKS_PROPERTY() bool enableCollision = true;
    ROCKS_PROPERTY() bool castShadow = true;
    ROCKS_PROPERTY() bool visibleInRayTracing = true;
    ROCKS_PROPERTY() bool affectDistanceFieldLighting = true;
    ROCKS_PROPERTY() bool forceOpaque = false;
};

} // namespace rocks
