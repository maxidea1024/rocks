#pragma once

#include <Mesh/Common/MeshSectionInfo.h>

// 메시 섹션 정보 표의 항목이다(Docs/Design/MeshAnimationAssets 4.4절·5.5.1절 4, UE
// FMeshSectionInfoMap — StaticMesh.h).
namespace rocks {

// 섹션 정보 표의 항목이다(UE FMeshSectionInfoMap 키 (LOD, 섹션)). 표(메시의 sectionInfo 배열)는
// (lod, section) 엄격 증가 순이다. 항목이 없는 섹션은 같은 섹션 번호의 LOD 0 항목, 그것도 없으면
// 슬롯 대응을 쓴다(UE FMeshSectionInfoMap::Get의 LOD 0 대체 — 5.5.2절 6).
//
// 스레드: 값 타입이다(동기화 없음).
ROCKS_STRUCT()
struct MeshSectionInfoEntry {
    ROCKS_GENERATED_BODY()

    ROCKS_PROPERTY() int32 lod = 0;
    ROCKS_PROPERTY() int32 section = 0;
    ROCKS_PROPERTY() MeshSectionInfo info;
};

} // namespace rocks
