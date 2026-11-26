#pragma once

#include <Base/Types.h>
#include <Meta/Annotations.h>

// 메시 섹션 정보다(Docs/Design/MeshAnimationAssets 4.4절·5.5.1절 4, UE FMeshSectionInfo·
// StaticMesh.h).
namespace rocks {

// (LOD, 섹션) 하나의 섹션 정보다. 기본값은 UE 생성자다. materialIndex·castShadow는 빌드가
// 쓰고(구현) 나머지 넷은 쿠킹 섹션에 옮겨 둔다(보존 — 소비자는 물리·레이 트레이싱·GI 단계).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshSectionInfo {
    ROCKS_GENERATED_BODY()

    // 섹션이 쓸 머티리얼 슬롯 번호다.
    ROCKS_PROPERTY() int32 materialIndex = 0;
    ROCKS_PROPERTY() bool enableCollision = true;
    ROCKS_PROPERTY() bool castShadow = true;
    ROCKS_PROPERTY() bool visibleInRayTracing = true;
    ROCKS_PROPERTY() bool affectDistanceFieldLighting = true;
    ROCKS_PROPERTY() bool forceOpaque = false;
};

} // namespace rocks
