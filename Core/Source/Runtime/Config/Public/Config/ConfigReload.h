#pragma once

#include <Config/ConfigContext.h>
#include <Core/Memory/UniquePtr.h>
#include <Platform/DirectoryWatch.h>
#include <Platform/File.h>

// 설정 파일 hot reload다(Docs/Design/ConfigSystem.ko.md 8절 "실행 중 변경" — 설정 CF3). 에디터·개발
// 빌드에서 엔진·프로젝트·사용자 층의 `Config/` 폴더를 감시하고, 바뀐 branch 파일만 다시 읽어 그 층
// 입력을 바꾼다. 감시 이벤트는 바로 값을 바꾸지 않는다: 이벤트는 "다시 읽을 파일" 표시만 하고,
// 다음 sync 지점(ConfigContext::Sync 앞 hook)에서 파일을 읽어 큐에 넣은 뒤 그 Sync가 적용한다. 출시
// 빌드(출시 규칙)는 감시하지 않는다.
//
// 다시 읽기 규칙(파일 하나):
// - 있으면 ConfigContext::ReplaceLayerText(사용자 층은 AddUserFileText(replace))로 그 (층, 태그)
//   입력을 바꾼다. 내용이 거부되면 옛 입력을 그대로 두고 진단을 Error 로그로 남긴다.
// - 없으면(지워졌으면) Unset(층, 태그)로 그 파일의 값을 뺀다.
// 층과 태그는 LoadConfigFiles와 같다: `<Branch>.json` → Engine·Project·User,
// `<플랫폼>/<Branch>.json` → EnginePlatform·ProjectPlatform(플랫폼은 문맥의 플랫폼과 부모 사슬).
// 그 밖의 경로(임시 파일·모르는 branch·모르는 폴더)는 무시한다. 감시 이벤트를 잃었으면(Overflow)
// 그 루트의 모든 branch 파일을 다시 읽는다.
//
// 결정적 시험: 감시기 없이 Start(false)로 hook만 걸고 NotifyChanged로 이벤트를 넣는다(이벤트가 gate
// 다 — 값은 다음 Sync에서만 바뀐다).
//
// 사용 예(개발 빌드 호스트):
//   ConfigFileReloader reloader(context, platform::PhysicalFileSystem());
//   if (auto started = reloader.Start(true); !started) { ... }    // 출시 규칙이면 Unsupported
//   // 이후 프레임마다 엔진의 Sync가 바뀐 파일을 적용한다.
//
// 스레드: 문맥의 소유 스레드에서만 쓴다. 수명: 문맥보다 먼저 파괴한다(소멸자가 hook을 뺀다).
namespace rocks::config {

// 감시 루트 종류다(층 폴더).
enum class ConfigFileRoot : uint8 {
    // <engineDir>/Config(Engine·EnginePlatform 층).
    Engine,
    // <projectDir>/Config(Project·ProjectPlatform 층).
    Project,
    // <userDataDir>/<projectName>/Config(User 층 — Standard 모드만).
    User
};

class ROCKS_CONFIG_API ConfigFileReloader {
public:
    // context·files는 이 객체보다 오래 살아야 한다.
    ConfigFileReloader(ConfigContext& context, platform::FileSystem& files) noexcept;
    ~ConfigFileReloader() noexcept;

    ConfigFileReloader(const ConfigFileReloader&) = delete;
    ConfigFileReloader& operator=(const ConfigFileReloader&) = delete;

    // sync hook을 건다. watchDirectories면 있는 루트 폴더(엔진·프로젝트, Standard 모드의 사용자 —
    // 사용자 폴더는 없으면 만든다)마다 DirectoryWatcher를 연다(하나도 못 열면 그 오류). 오류: 이미
    // 시작함 AlreadyExists, 출시 규칙 Unsupported, 문맥 초기화 전 InvalidArgument, 다른 스레드
    // WrongThread, 감시기 열기 실패(FileErrorCode), 할당 실패.
    Result<void> Start(bool watchDirectories) noexcept;

    // hook을 빼고 감시기를 닫는다. 여러 번 불러도 된다. sync hook 안에서는 부르지 않는다.
    void Stop() noexcept;

    // root 폴더의 relative(Config 폴더 기준, '/' 구분 — `Render.json`·`Android/Render.json`)가
    // 바뀌었다고 표시한다(감시기 visitor와 시험이 부른다). 빈 relative는 그 루트 전체다(Overflow).
    // 같은 표시는 한 번만 남는다. 값은 다음 Sync에서 바뀐다. 오류: root 범위 InvalidArgument, 할당
    // 실패.
    Result<void> NotifyChanged(ConfigFileRoot root, StringView relative) noexcept;

    // root의 감시기에 이벤트가 생기거나 timeoutMilliseconds가 지날 때까지 기다린다(이벤트가 있으면
    // true — 다음 Sync가 가져간다). 감시기가 없으면 바로 false다. 이벤트를 기다리는
    // 도구·시험용이다.
    bool WaitForChanges(ConfigFileRoot root, uint32 timeoutMilliseconds) noexcept;

    // 마지막 sync 지점에서 다시 읽어 큐에 넣은 파일 수(지운 파일의 Unset 포함)다.
    uint32 LastReloadCount() const noexcept {
        return lastReloadCount_;
    }

private:
    struct State;

    static void OnSync(void* self, ConfigContext& context) noexcept;
    void PollWatchers() noexcept;
    Result<uint32> ReloadMarked() noexcept;

    ConfigContext& context_;
    platform::FileSystem& files_;
    UniquePtr<State> state_;
    uint32 lastReloadCount_ = 0;
};

} // namespace rocks::config
