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

} // namespace rocks::config
