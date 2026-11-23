#pragma once

#include <Core/Commandlet/Commandlet.h>

// 쿠킹 명령이다(Docs/Design/AssetSystem.ko.md 13절, 10.2절 결정 12, "A4e 구현 세부" — A4e1).
// context는 AssetCommandContext*다(commandlet_host::Run이 건다).
//   asset-cook --profile=<id> [<path>…] [--verify-nuggets] [--verify-nuggets-source=local|remote]
//              [--json] [--project=<dir>] [--wait=<초>]
//       쓰기 잠금을 잡고 색인을 만든 뒤 대상 프로파일(엔진 `Engine/Config/TargetProfiles/<id>.json`
//       위에 프로젝트 파일)과 `Config/CookSettings.json`으로 쿠킹해 `Saved/Cooked/<id>/`에
//       게시한다(CookProject). 경로가 없으면 전체 쿠킹, 있으면 부분 쿠킹(에셋·디렉터리)이다.
//       진단은 표준 오류에 `asset-cook: <코드>: <대상>: <error|warning>[ <원인 오류>]
//       [ (related <대상>)]` 한 줄씩, 요약은 표준 출력에 `asset-cook: <id> <published|not
//       published> targets <n> cooked <n> reused <n> bytes <n>[ partial]` 한 줄이다(--json이면
//       요약 대신 CookReport.json 내용). --verify-nuggets는 재사용 없이 모든 대상을 쿠킹하며
//       처리기가 너겟 적중을 다시 만들어 대조한다(다르면 NuggetMismatch). source remote는 Rockpile
//       backend가 생기기 전까지 실행 실패 Unsupported다.
//       종료 코드: 게시 0(경고만 있어도), 오류 진단으로 게시하지 않음·실행 실패 1, 사용법 오류 2.
namespace rocks::commandlet_host {

ROCKS_ASSETCOMMANDLETS_API Result<int32>
RunAssetCookCommand(void* context, Span<const StringView> args, CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
