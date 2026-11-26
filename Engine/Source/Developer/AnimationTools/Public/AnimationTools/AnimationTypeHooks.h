#pragma once

#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Containers/Array.h>

// 애니메이션 에셋 타입 hook이다(Docs/Design/MeshAnimationAssets 7.6.1절 2). AnimationAsset의 Build
// 의존(skeleton)을 선언한다: 쿠킹 트랙 → 본 번호가 스켈레톤에 기대므로 스켈레톤 콘텐츠가 바뀌면
// 쿠커가 클립을 다시 쿠킹한다(AssetSystem 10.2절 결정 5 — UE도 애니메이션 파생 데이터 키에 스켈레톤
// Guid를 넣는다). additive 기준 클립은 Hard로 둔다(7.1절).
//
// 사용 예(세션 hook 등록 — AssetTypeHookSetup 모양):
//   context.typeHooks = {&RegisterAnimationTypeHooks, nullptr};
namespace rocks {

// AnimationAsset의 buildDependencies hook을 table에 등록하고 토큰을 tokens에 둔다(state는 쓰지
// 않는다). 오류: 등록 오류(이미 등록 AlreadyExists 등), 할당 실패.
ROCKS_ANIMATIONTOOLS_API Result<void>
RegisterAnimationTypeHooks(void* state, AssetTypeHookTable& table,
                           Array<AssetTypeHookRegistration>& tokens) noexcept;

} // namespace rocks
