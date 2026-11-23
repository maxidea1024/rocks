#pragma once

#include <AssetRuntime/AssetTypeHooks.h>
#include <Object/ObjectRegistry.h>

// 에셋 bundle 태그 수집이다(Docs/Design/AssetSystem 10.2절 결정 9, "A4e 구현 세부" — A4e2b, UE
// UAssetManager::InitializeAssetBundlesFromMetadata 대응). 대표 에셋의 값을 리플렉션 공용 순회로
// 돌며, 실효 bundle이 있는 null 아닌 소프트 참조마다 태그 `Bundle.<이름>` = 대상 AssetId 표준
// 문자열을 sink에 더한다. 실효 bundle은 대표 에셋에서 그 값까지 경로에 있는 필드 지정자
// AssetBundles 목록들의 교집합이다(목록이 없는 단계는 제한하지 않고, 하나도 없으면 어느 bundle에도
// 들지 않는다 — 교집합이 비면 들지 않는다). 경로의 어느 단계든 EditorOnly인 필드(쿠킹 패키지에서
// 빠진다)와 Transient 필드(저장되지 않는다)는 건너뛴다. InstancedStruct 내용도 따라간다. 하위
// 객체(다른 객체를 가리키는 참조)는 따라가지 않는다.
//
// 사용 예(에디터 패키지 저장 — 타입 hook Tags 뒤):
//   AssetTagSink tags(allocator);
//   if (auto collected = CollectAssetBundleTags(registry, access, representative, allocator, tags);
//       !collected) { ... }
namespace rocks {

// object의 bundle 태그를 sink에 더한다. allocator는 순회의 작업 메모리다. 오류: 순회의 오류
// (순회할 수 없는 필드 Unsupported·경로 연산 오류 — reflection_text::VisitValues), 태그 추가 오류,
// 할당 실패.
ROCKS_ASSETDATABASE_API Result<void>
CollectAssetBundleTags(ObjectRegistry& registry, const ObjectAccessContext& access,
                       ObjectHandle object, Allocator& allocator, AssetTagSink& sink) noexcept;

} // namespace rocks
