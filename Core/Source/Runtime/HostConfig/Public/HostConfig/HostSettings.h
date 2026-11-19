#pragma once

#include <Config/ConfigContext.h>
#include <Config/ConfigReload.h>
#include <Config/DeviceProfiles.h>
#include <Config/DeviceTraits.h>
#include <Core/Text/String.h>
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
//   4) ApplyCommandLine    --set=<target>=<value>(Session 층), --device-profile=<이름> 기억
//   5) CompleteStartup     Sync → 기기 프로필 적용 → Sync → 시작 완료
//                          (StartupOnly 값 — log.mode·log.file.* 등 — 고정)
//   그다음 LogHost 설치 → trace 세션 → 엔진 루프(프레임마다 Sync).
// 실패는 이 객체가 진단과 함께 한 번 기록한다(LogHost 설치 전이면 비상 경로 stderr). 호출자는 다시
// 기록하지 않는다.
//
// 기기 특성(DeviceTraits)은 Initialize가 플랫폼 특성(플랫폼 이름·시스템 메모리)으로 채운다. 그래픽
// 장치를 만든 호스트는 DeviceTraits()에 GPU 특성(DeviceCaps)을 더하고 ApplyDeviceTraits를 부른다
// (기기 프로필·품질 묶음을 다시 적용 — 다음 Sync, 시작 뒤라 StartupOnly 값은 바뀌지 않는다).
//
// 프로젝트 폴더·사용자 층(게임·에디터 실행 파일 — Docs/Design/ConfigSystem.ko.md 5.4절): 프로젝트
// 폴더는 실행 파일 위쪽에서 프로젝트 기술자(`*.rocksproject`)를 가진 첫 폴더이고 프로젝트 이름은 그
// 기술자 파일 이름이다(FindProjectDirectory). 사용자 층은 Standard 모드에서
// `<UserData>/<프로젝트 이름>/Config`다(UserDataDirectory — Windows Roaming AppData, macOS Library/
// Application Support). Sandbox·RocksCommandlet처럼 프로젝트가 없는 도구는 봉인 모드다.
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
    // 플랫폼 이름(플랫폼 층 폴더·기기 프로필의 플랫폼)이다. 비면 HostPlatformName()이다.
    StringView platform{};
    // 호스트·게임 모듈의 리플렉션 타입 등록(선택)이다. TraceControl 다음에 부른다.
    Result<void> (*registerModuleTypes)(ModuleRegistration&) noexcept = nullptr;
    // 호스트·게임 모듈의 설정 등록(선택)이다. log.*·trace.* 다음, 파일보다 먼저 부른다.
    Result<void> (*registerSettings)(config::ConfigContext&) noexcept = nullptr;
    // 품질 조합 검증 규칙(선택 — ConfigContext::SetQualityValidation, 기기 특성은 HostSettings의
    // DeviceTraits)이다. 빌린다: HostSettings보다 오래 살아야 한다.
    Span<const config::QualityGroupRule> qualityRules{};
    Span<const config::QualityBudgetRule> qualityBudgets{};
};

// 이 실행 파일의 플랫폼 이름이다("Windows"·"Mac"·"Android" — 플랫폼 층 폴더 이름).
StringView HostPlatformName() noexcept;

// 플랫폼 기기 특성(DeviceTraitNames::Platform = platform, SystemMemoryMiB =
// platform::QueryPhysicalMemory)을 traits에 쓴다. 오류: 메모리 조회 실패 Unsupported(플랫폼 이름은
// 이미 썼다), 할당 실패.
Result<void> CollectPlatformDeviceTraits(StringView platform,
                                         config::DeviceTraits& traits) noexcept;

// start 폴더부터 위로 올라가며 프로젝트 기술자(`<이름>.rocksproject` 파일)를 가진 첫 폴더를 찾는다.
// directory에 그 폴더(구분자 '/'), name에 기술자 이름(확장자 제외)을 쓴다. 기술자가 여럿이면 이름
// byte 순서로 첫 것이다. 오류: 없으면 NotFound, 파일 시스템 오류(FileErrorCode), 할당 실패.
Result<void> FindProjectFrom(StringView start, String& directory, String& name) noexcept;

// 실행 파일 폴더부터 FindProjectFrom을 한다(게임·에디터 실행 파일). 오류: 실행 파일 경로를 얻지
// 못하면 그 오류, 그 밖은 FindProjectFrom과 같다.
Result<void> FindProjectDirectory(String& directory, String& name) noexcept;

// 사용자 층 뿌리 폴더(platform::StandardDirectory::UserData — 구분자 '/')를 out에 쓴다. 오류:
// 조회 실패(FileErrorCode), 할당 실패.
Result<void> UserDataDirectory(String& out) noexcept;

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

    // 4) 명령줄 --set을 넣고 --device-profile=<이름>을 기억한다(5단계가 쓴다). 오류:
    // AddCommandLineAssignments의 오류(진단을 기록한다).
    Result<void> ApplyCommandLine(Span<const StringView> arguments) noexcept;

    // 5) 큐를 적용하고, 기기 프로필(ApplyDeviceProfile — 명령줄·프로젝트·규칙·플랫폼 기본)을 적용해
    // 다시 Sync한 뒤 시작을 끝낸다. 프로필 정의 오류는 기록하고 시작은 계속한다. 품질
    // 묶음(CVarGroup) 펼침과 조합 검증(options.qualityRules — 기기 특성으로 펼칠 등급을 낮춘다)은
    // 이 Sync들이 한다. 오류: Sync·CompleteStartup·적용의 호출 오류(기록한다).
    Result<void> CompleteStartup() noexcept;

    // 기기 특성이다(Initialize가 플랫폼 특성을 채운다). 호스트는 GPU 특성을 더한 뒤
    // ApplyDeviceTraits를 부른다. 전제: Initialize 성공 — 아니면 Fatal.
    config::DeviceTraits& DeviceTraits() noexcept;

    // 지금 기기 특성으로 기기 프로필을 다시 적용하고 품질 조합 검증도 다시 하게 한다(다음 Sync에
    // 바뀐다). 오류: 적용의 호출 오류(기록한다).
    Result<void> ApplyDeviceTraits() noexcept;

    // 마지막 기기 프로필 선택이다.
    const config::DeviceProfileSelection& DeviceProfile() const noexcept {
        return profile_;
    }

    // 설정 파일 hot reload를 켠다(개발 빌드 — ConfigFileReloader, 엔진·프로젝트·사용자 Config 폴더
    // 감시). 이후 Sync(엔진 프레임 sync 지점)마다 바뀐 파일을 적용한다. 출시 규칙이면 아무것도 하지
    // 않고 Unsupported다(기록하지 않는다). 오류: 그 밖의 ConfigFileReloader::Start 오류(기록한다).
    Result<void> StartHotReload() noexcept;

    // 설정 문맥이다. 전제: Initialize 성공 — 아니면 Fatal.
    config::ConfigContext& Context() noexcept;

    ModuleRegistration& Modules() noexcept {
        return modules_;
    }

private:
    static RegistryOptions MakeOptions(NamePool& names) noexcept;
    // 거부를 진단과 함께 기록한다(진단은 비운다).
    void ReportRejected(StringView what, ErrorCode error) noexcept;
    // 남은 Warning 진단(오래된 사용자 파일의 버린 키 등)을 Warning 로그로 남기고 진단을 비운다.
    void ReportWarnings() noexcept;
    // Sync하고 남은 Error·Warning 진단을 기록한다(what은 단계 이름).
    Result<void> SyncAndReport(StringView what) noexcept;

    Allocator& allocator_;
    TypeRegistry types_;
    ModuleRegistration modules_;
    NamePool names_;
    ObjectRegistry objects_;
    UniquePtr<config::ConfigContext> context_;
    // 문맥보다 먼저 파괴한다(sync hook을 뺀다).
    UniquePtr<config::ConfigFileReloader> reloader_;
    config::DeviceTraits traits_;
    config::DeviceProfileSelection profile_;
    String profileArgument_;
    Span<const config::QualityGroupRule> qualityRules_{};
    Span<const config::QualityBudgetRule> qualityBudgets_{};
    bool namesReady_ = false;
};

} // namespace rocks
