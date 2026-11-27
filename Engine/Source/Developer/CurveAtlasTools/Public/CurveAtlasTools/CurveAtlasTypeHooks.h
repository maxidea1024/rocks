#pragma once

#include <AssetRuntime/AssetTypeHooks.h>
#include <Core/Containers/Array.h>
#include <Core/Result.h>

// 색 커브 atlas 타입 hook이다(Docs/Design/CurveAssets 4.2·4.3절, AssetSystem 12.12절 정규화·12.14절
// A5j2 원본 재구성·16.2절 Build 의존).
//
// - 원본 재구성(까닭 Edit·New·Open — UE PostEditChangeProperty·PostLoad의 UpdateTextures): curves를
//   풀어 BakeColorCurveAtlas로 굽고, 지금 원본(형식·크기·slice·mip·layer·bulk 내용)과 다를 때만 새
//   bulk(sink AddBulk)로 source를 바꾸고 MarkRebuilt한다. Propagate는 아무것도 하지 않는다.
//   assignedFields는 보지 않는다(늘 굽고 비교한다) — 다만 New에서 width를 대입하지 않았으면 먼저
//   NewColorCurveAtlasWidth(64 — UE 팩터리)로 둔다. 오류: 크기 상한 InvalidSetting /
//   CapacityExceeded (detail = 넘은 값), 풀리지 않는 커브 MissingDependency / 그 Resolve
//   오류(detail = 원소 번호), 객체 접근 오류, 할당 실패.
// - Build 의존: curves의 각 커브 패키지(null·패키지 밖·atlas와 같은 패키지 제외 — outermost 비교).
// - 정규화(Texture 정규화 뒤): width = max(width, 2), disableAdjustments면 텍스처 조정값 항등값·
//   크로마 키 끔(UE CacheAndResetColorAdjustments).
//
// 사용 예(제품 호스트 — 세션마다 표에 건다):
//   context.typeHooks = {&RegisterProductTypeHooks, nullptr};  // 그 안에서
//   (void)RegisterCurveAtlasTypeHooks(state, table, tokens);
namespace rocks {

// ColorCurveAtlas의 hook 셋을 table에 등록하고 토큰을 tokens에 둔다(AssetTypeHookSetup 모양 —
// state는 쓰지 않는다). 오류: 등록 오류(이미 등록 AlreadyExists, 타입 없음 NotFound), 할당 실패.
ROCKS_CURVEATLASTOOLS_API Result<void>
RegisterCurveAtlasTypeHooks(void* state, AssetTypeHookTable& table,
                            Array<AssetTypeHookRegistration>& tokens) noexcept;

} // namespace rocks
