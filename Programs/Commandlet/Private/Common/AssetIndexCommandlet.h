#pragma once

#include <Core/Commandlet/Commandlet.h>

// 에디터 패키지 색인 명령이다(Docs/Design/AssetSystem.ko.md 13절, 7.1절 13 — A2b). 둘 다 쓰기 잠금
// 없이 Content를 검색해 읽기만 한다. 공통 옵션 --project=<dir>(없으면 현재 디렉터리에서 위로
// `*.rocksproject`를 찾는다). 사용법 오류 2, 실행 실패 1, 성공 0이다. context는 asset-list가
// AssetCommandContext*, asset-index-verify가 작업 메모리 Allocator*다(commandlet_host::Run이 건다).
//   asset-list [--type=<Type>] [--under=<path>] [--project=<dir>]
//       색인 항목을 경로 키 순으로 한 줄씩(`<논리 경로> <AssetId> <대표 타입 id>`) 표준 출력에
//       쓴다. --type은 에셋 타입을 등록해 그 클래스와 파생만 남긴다(A2c3b — AssetService::List).
//   asset-index-verify [--project=<dir>]
//       검색 진단과 의존 대조(Verify) 진단을 표준 오류에 `<진단 코드>: <path>: <설명>` 한 줄씩 쓰고
//       요약 한 줄을 표준 출력에 쓴다. 진단이 있으면 1이다.
namespace rocks::commandlet_host {

Result<int32> RunAssetListCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept;

Result<int32> RunAssetOrphansListCommand(void* context, Span<const StringView> args,
                                         CommandletOutput& output) noexcept;
Result<int32> RunAssetIndexVerifyCommand(void* context, Span<const StringView> args,
                                         CommandletOutput& output) noexcept;
// asset-index-rebuild [--project=<dir>] [--wait=<seconds>] — 쓰기 잠금을 잡고 캐시 없이 다시
// 검색하고 고아 임시 파일을 지운 뒤 색인 캐시를 쓴다(7.1절 16 A2d1). 진단이 있어도 0이다.
Result<int32> RunAssetIndexRebuildCommand(void* context, Span<const StringView> args,
                                          CommandletOutput& output) noexcept;

} // namespace rocks::commandlet_host
