#pragma once

#include <Core/Commandlet/Commandlet.h>

// 에셋 편집 명령이다(Docs/Design/AssetSystem.ko.md 13절, 7.1절 15 A2c3b). 서비스 API
// AssetService(AssetDatabase)를 부르고 인수·출력만 다룬다. context는 AssetCommandContext*다
// (commandlet_host::Run이 건다). 사용법 오류 2, 실행 실패 1(진단은 표준 오류에
// `<명령>: <코드>: <대상>: <설명>` 한 줄씩), 성공 0(경고만 있어도 0)이다. 공통 옵션
// --project=<dir>·--wait=<초>.
//   asset-new <Type> <path> [<경로>=<값>…] [--unset=<경로 목록>] [--erase=<경로 목록>]
//       새 패키지를 만들고 대입한 뒤 저장한다(쓰기 잠금). 표준 출력에 `<논리 경로> <AssetId>`.
//   asset-set <path|AssetId> <대입>… [--force]
//       대입을 모두 검증한 뒤 게시하고 저장한다(쓰기 잠금). --force는 미해결 import를 null로
//       저장한다.
//   asset-info <path|AssetId> [--json] [--all]
//       대표 에셋의 값을 `<경로>=<값>` 줄(asset-set에 그대로 넣을 수 있다) 또는 JSON으로 쓴다.
//       --all은 보고용 전체 출력이다. 잠금을 잡지 않는다.
namespace rocks::commandlet_host {

Result<int32> RunAssetNewCommand(void* context, Span<const StringView> args,
                                 CommandletOutput& output) noexcept;
Result<int32> RunAssetSetCommand(void* context, Span<const StringView> args,
                                 CommandletOutput& output) noexcept;
Result<int32> RunAssetInfoCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
