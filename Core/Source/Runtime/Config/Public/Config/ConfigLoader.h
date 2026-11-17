#pragma once

#include <Config/ConfigContext.h>
#include <Platform/File.h>

// 설정 파일 층 로더다(Docs/Design/ConfigSystem.ko.md 3.1·3.3절).
namespace rocks::config {

// 등록된 branch마다(등록 순서) 다음 파일을 이 순서로 읽어 AddLayerText로 넣는다(없는 파일은
// 건너뛴다). 플랫폼 층은 부모 사슬의 폴더를 약한 것부터 읽고 마지막에 platform 폴더를 읽는다.
//   Engine:          <engineDir>/Config/<Branch>.json
//   EnginePlatform:  <engineDir>/Config/<부모 또는 platform>/<Branch>.json
//   Project:         <projectDir>/Config/<Branch>.json
//   ProjectPlatform: <projectDir>/Config/<부모 또는 platform>/<Branch>.json
//   User:            <userDataDir>/<projectName>/Config/<Branch>.json(Standard 모드만)
// 디렉터리가 비어 있으면 그 층을 읽지 않는다. 부모 사슬(약한 것부터)은 옵션 platformParents다.
// 태그는 Config 폴더 기준 상대 이름(`Render.json`·`Android/Render.json`), 출처 이름은 파일 경로다.
// 값은 다음 Sync에 바뀐다(설정 struct는 먼저 등록해 둔다 — 모르는 섹션은 오류다).
//
// 한 파일이 진단으로 실패해도 나머지 파일을 끝까지 읽어 진단을 모은다.
// 오류: 진단을 남긴 파일이 있으면 InvalidArgument(그 파일은 넣지 않았다), 초기화 전
// InvalidArgument, 다른 스레드 WrongThread, 파일 읽기 실패는 그 FileStatus의 ErrorCode, 할당 실패.
//
// 사용 예:
//   if (auto loaded = LoadConfigFiles(context, platform::PhysicalFileSystem()); !loaded) {
//       // context.Diagnostics()의 파일·줄·열 보고
//   }
//   context.Sync();
Result<void> LoadConfigFiles(ConfigContext& context, platform::FileSystem& files) noexcept;

// 명령줄 대입 인자의 접두사다(`--set=<target>=<value>`).
inline constexpr StringView SetArgumentPrefix = "--set=";

// 명령줄 인자 가운데 `--set=<target>=<value>`를 나온 순서대로 세션 층 대입(AddAssignment —
// ConfigLayer::Session, 태그·출처 `--set`)으로 넣는다. target은 첫 `=` 앞까지이고 value는 그 뒤
// 전부다(`--set=Render.RenderGraph.passes=a,b`·`--set=log.level=debug`). 다른 인자는 무시한다. 값은
// 다음 Sync에 바뀐다. 거부한 인자가 있어도 나머지를 끝까지 넣어 진단을 모은다. 반환: 넣은 대입 수.
// 오류: 첫 실패 — target이 비었거나 `=`가 없음 InvalidArgument, AddAssignment의 오류(진단 참조).
//
// 사용 예(엔진 부트):
//   LoadConfigFiles(context, files);                       // 엔진·프로젝트 층
//   AddCommandLineAssignments(context, arguments);         // 세션 층
//   context.Sync();
//   context.CompleteStartup();
Result<uint32> AddCommandLineAssignments(ConfigContext& context,
                                         Span<const StringView> arguments) noexcept;

} // namespace rocks::config
