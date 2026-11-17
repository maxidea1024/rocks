#pragma once

#include <Config/ConfigContext.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>

// 호스트의 설정 부트다(Docs/Design/ConfigSystem.ko.md 5.3절). 한 프로세스(게임·에디터·Sandbox·
// RocksCommandlet)의 설정 문맥 하나를 만들고, 그 문맥이 쓰는 타입 등록·모듈 등록·객체 Registry를
// 소유한다. 엔진은 이 문맥을 빌려 쓴다(EngineConfig::settings).
//
// 부트 순서(호스트가 이 순서로 부른다):
//   1) Initialize          모듈 타입(TraceControl + options.registerModuleTypes)과 설정 등록
//                          (log.*, trace.*·TracePresets, options.registerSettings). 파일보다
//                          먼저다.
//   2) LoadFiles           엔진·프로젝트(·플랫폼·사용자) 층 파일(LoadConfigFiles)
//   3) AddFile             호스트가 받은 추가 파일(예: Sandbox --config, Project 층)
//   4) ApplyCommandLine    --set=<target>=<value>(Session 층)
//   5) CompleteStartup     Sync 뒤 시작 완료(StartupOnly 값 — log.mode·log.file.* 등 — 고정)
//   그다음 LogHost 설치 → trace 세션 → 엔진 루프(프레임마다 Sync).
// 실패는 이 객체가 진단과 함께 한 번 기록한다(LogHost 설치 전이면 비상 경로 stderr). 호출자는 다시
// 기록하지 않는다.
//
// 수명: LogHost·TraceController·엔진보다 오래 산다(LogHost::Stop이 구독을 푼다). 스레드: 만든
// 스레드(게임 스레드)에서만 쓴다.
namespace rocks {

struct HostSettingsOptions {
    // 설정 문맥 옵션의 디렉터리·모드다(ConfigContextOptions와 같은 뜻). view는 Initialize가
    // 복사한다.
    config::ConfigMode mode = config::ConfigMode::Sealed;
    StringView engineDir{};
    StringView projectDir{};
    StringView projectName{};
    StringView userDataDir{};
    StringView savedDir{};
    // 호스트·게임 모듈의 리플렉션 타입 등록(선택)이다. TraceControl 다음에 부른다.
    Result<void> (*registerModuleTypes)(ModuleRegistration&) noexcept = nullptr;
    // 호스트·게임 모듈의 설정 등록(선택)이다. log.*·trace.* 다음, 파일보다 먼저 부른다.
    Result<void> (*registerSettings)(config::ConfigContext&) noexcept = nullptr;
};

// 실행 파일 위쪽 폴더에서 `Engine/Config` 폴더를 가진 첫 폴더를 찾아 그 `Engine` 경로를 out에 NUL로
// 끝내 쓴다(빌드 출력이 저장소·설치 폴더 안에 있다). 반환: 경로 byte 수. 오류: 찾지 못하면
// NotFound, 실행 파일 경로를 얻지 못하면 그 파일 상태의 오류, out이 작으면 CapacityExceeded.
Result<usize> FindEngineDirectory(Span<char> out) noexcept;

class HostSettings {
public:
    explicit HostSettings(Allocator& allocator) noexcept;
    ~HostSettings() noexcept;

    HostSettings(const HostSettings&) = delete;
    HostSettings& operator=(const HostSettings&) = delete;

    // 1) 문맥을 만들고 타입·설정을 등록한다. 오류: 등록·초기화의 오류(기록한다).
    Result<void> Initialize(const HostSettingsOptions& options) noexcept;

    // 2) 층 파일을 읽는다. 오류: LoadConfigFiles의 오류(진단을 기록한다).
    Result<void> LoadFiles() noexcept;

    // 3) Engine branch 파일 하나를 layer로 넣는다. 오류: 파일 읽기는 그 파일 상태의 오류,
    // AddLayerText의 오류(진단을 기록한다).
    Result<void> AddFile(config::ConfigLayer layer, StringView branch, const char* path) noexcept;

    // 4) 명령줄 --set을 넣는다. 오류: AddCommandLineAssignments의 오류(진단을 기록한다).
    Result<void> ApplyCommandLine(Span<const StringView> arguments) noexcept;

    // 5) 큐를 적용하고 시작을 끝낸다. 오류: Sync·CompleteStartup의 오류(진단을 기록한다).
    Result<void> CompleteStartup() noexcept;

    // 설정 문맥이다. 전제: Initialize 성공 — 아니면 Fatal.
    config::ConfigContext& Context() noexcept;

    ModuleRegistration& Modules() noexcept {
        return modules_;
    }

private:
    static RegistryOptions MakeOptions(NamePool& names) noexcept;
    // 거부를 진단과 함께 기록한다(진단은 비운다).
    void ReportRejected(StringView what, ErrorCode error) noexcept;

    Allocator& allocator_;
    TypeRegistry types_;
    ModuleRegistration modules_;
    NamePool names_;
    ObjectRegistry objects_;
    UniquePtr<config::ConfigContext> context_;
    bool namesReady_ = false;
};

} // namespace rocks
