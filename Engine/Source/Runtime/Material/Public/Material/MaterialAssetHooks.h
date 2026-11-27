#pragma once

#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>

// 머티리얼 에셋 타입 hook 등록이다(Docs/Design/MaterialSystem 4.4·4.6.1절, AssetSystem 16.2절
// DependencyDeclarer). 에디터 저장과 쿠커가 부르는 Build 의존 hook을 등록한다.
//
// - MaterialInstance: 부모(Hard 참조이자 번역 입력), 레이어 스택의 레이어·혼합 함수(EditorOnly).
// - MaterialFunctionInstance: 부모 함수(EditorOnly).
// - Material: 그래프 안 함수 호출·다른 머티리얼 참조는 그 노드 속성이 생기는 단계(MS2e)에서 더한다.
//
// 사용 예(에셋 서비스 초기화 — 등록 handle을 서비스 수명 동안 쥔다):
//   Array<AssetTypeHookRegistration> registrations(allocator);
//   if (auto registered = RegisterMaterialAssetHooks(table, registrations); !registered) { ... }
//
// 스레드: 등록은 표 소유 스레드에서 한다.
namespace rocks {

// table에 머티리얼 hook을 등록하고 handle을 out 끝에 더한다. 오류: 표의 등록 오류(이미 등록
// AlreadyExists 등)·할당 실패. 실패하면 이번에 더한 handle은 놓는다.
ROCKS_MATERIAL_API Result<void>
RegisterMaterialAssetHooks(AssetTypeHookTable& table,
                           Array<AssetTypeHookRegistration>& out) noexcept;

} // namespace rocks
