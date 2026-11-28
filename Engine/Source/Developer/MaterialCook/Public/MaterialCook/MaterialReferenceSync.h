#pragma once

#include <Core/Result.h>
#include <MaterialCook/MaterialCookGatherContext.h>
#include <MaterialCook/MaterialCookSources.h>

// 참조 동기화다(Docs/Design/MaterialSystem 12.8절 MS3c "참조 동기화" — UE
// FMaterialCachedExpressionData::ReferencedTextures). Material의 Hard 필드
// referencedTextures(그래프와 함수·레이어 닫힘의 텍스처·커브 atlas)와 parameterCollections(컬렉션
// 노드)를 그래프에서 다시 채운다. 쿠킹은 같은 모으기 결과가 필드 안에 드는지 대조한다(빠졌으면
// "다시 저장" — 쿠킹 대상 확장과 cookKey가 이 Hard 간선에 기댄다).
//
// 사용 예(저장 명령·에디터 컴파일):
//   bool changed = false;
//   if (auto synced = SyncMaterialReferences(gather, materialHandle, changed); !synced) { ... }
//   if (changed) { packages.MarkDirty(id); }
namespace rocks {

// material의 두 필드를 모은 참조(AssetId 순, 중복 없음 — 열지 못한 참조는 뺀다)로 바꾼다. changed는
// 필드가 바뀌었는가다. 오류: material이 Material이 아니면 TypeMismatch, 모으기 오류, Registry 참조
// 변경 오류, 할당 실패. 에디터 데이터 없는 빌드에서는 Unsupported다(그래프가 없다).
ROCKS_MATERIALCOOK_API Result<void> SyncMaterialReferences(const MaterialCookGatherContext& context,
                                                           ObjectHandle material,
                                                           bool& changed) noexcept;

// sources(같은 material 그래프에서 모은 결과)의 텍스처·atlas·컬렉션 참조가 material 필드 안에
// 모두 있는가다. 빠진 첫 참조의 AssetId를 missing에 쓴다. 오류: Registry 조회 오류.
ROCKS_MATERIALCOOK_API Result<bool>
MaterialReferencesInSync(const MaterialCookGatherContext& context, ObjectHandle material,
                         const MaterialCookSources& sources, AssetId& missing) noexcept;

} // namespace rocks
