#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <Platform/CrashReporting.h>

namespace rocks {
class Allocator;
class LogSystem;
class ModuleLoader;
} // namespace rocks

// 공통 실행 계층이다(UE Launch·GuardedMain 대응, Docs/Design/CrashHandlingAndLaunch.ko.md 6–8절).
// 실행 파일은 ApplicationDescription(이름·종류·설정·본문)만 주고, 진입점·크래시 수집 설치·명령줄
// 캡처·도구 로그·단계 표시·종료 코드·정리는 Launch가 한다. 진입점은 Launch/EntryPoint.h의 매크로가
// 만든다.
//
// 사용 예(실행 파일 소스 하나):
//   #include <Launch/EntryPoint.h>
//
//   namespace {
//   int Run(rocks::launch::ApplicationContext& context) noexcept {
//       for (const rocks::StringView argument : context.Arguments()) { ... }
//       return 0;
//   }
//
//   const rocks::launch::ApplicationDescription& Describe() noexcept {
//       static constexpr rocks::launch::ApplicationDescription description{
//           .name = "MyTool", .kind = rocks::launch::ApplicationKind::Tool, .run = &Run};
//       return description;
//   }
//   } // namespace
//
//   ROCKS_IMPLEMENT_APPLICATION(Describe)
namespace rocks::launch {

// 실행 파일의 종류다. 기본 정책이 다르다.
enum class ApplicationKind : uint8 {
    Tool,       // 명령줄 도구·벤치
    EngineHost, // 게임·Sandbox 엔진 실행(본문이 Engine·LogHost를 쓴다)
    TestRunner, // 시험 실행기와 그 자식 probe. OS·CRT 오류 대화상자를 늘 끈다
};

// Launch가 시작하는 도구 로그다. Stderr: Sync LogSystem(LogSystemConfig::ForTool) + stderr sink를
// 설치한다. 엔진 호스트는 None으로 두고 본문에서 LogHost를 쓴다.
enum class ToolLogging : uint8 {
    None,
    Stderr,
};

// 크래시 수집 방식이다(설계 7.4·8.8절). 내장(Embedded) 여부는 이 값이 아니라 진입 함수가 정한다
// (RunEmbeddedApplication). 내장 진입에서 Full·InProcess는 내장 보고(OS 처리기·도우미·덤프 없음,
// 보고 뒤 호스트로 전파)이고 Off는 끈다. configure가 내장과 독립 실행 사이를 오갈 수 없다.
enum class CrashReportingMode : uint8 {
    Full,      // OS 처리기 + 덤프 도우미(설정을 읽은 뒤 띄운다, 실패하면 프로세스 안 fallback)
    InProcess, // OS 처리기 + 프로세스 안 덤프
    Off,       // 설치하지 않는다(의도적 크래시 probe 등). Fatal은 비상 출력 뒤 바로 끝난다
};

// 프로세스의 범용 할당자 종류다(Docs/Design/GarbageCollectionScale.ko.md 18.10절). 문제를 가를 때
// System으로 바꿔 같은 프로그램을 두 할당자에서 비교한다.
enum class DefaultAllocatorKind : uint8 {
    Binned, // Launch의 SystemAllocator 위에 둔 BinnedAllocator(크기 등급 slab·스레드 캐시)
    System, // SystemAllocator 그대로(DefaultAllocator()가 SystemAllocator()와 같은 객체다)
};

// 본문을 돌리는 스레드를 정하는 호스트다(Docs/Design/MacPlatform.ko.md 3절). body(context)를 다른
// 스레드(게임 스레드)에서 돌리고, 부른 스레드(프로세스 메인 스레드)는 OS 이벤트 루프에 내준 뒤
// body가 끝나면 그 반환값을 돌려준다. body를 정확히 한 번 부르고 끝날 때까지 기다려야 한다.
//
// macOS 창 프로그램은 Cocoa가 창·이벤트를 메인 스레드에서만 받으므로 PlatformWindow의
// platform::RunCocoaMainThreadHost를 넣는다(UE MAC_SEPARATE_GAME_THREAD). 그 함수는 다른
// 플랫폼에서 body를 바로 부른다.
using MainThreadHostFunction = int (*)(int (*body)(void* context) noexcept, void* context) noexcept;

// 실행 설정이다. ApplicationDescription::settings가 기본값이고, configure가 명령줄을 본 뒤 고칠 수
// 있다.
struct ApplicationSettings {
    CrashReportingMode crashReporting = CrashReportingMode::Full;
    platform::CrashDumpDetail dumpDetail = platform::CrashDumpDetail::Standard;
    // 도우미의 추가 덤프(Detailed·Full) 마감이다. 0이면 종류별 기본값이다.
    uint32 helperDumpDeadlineMilliseconds = 0;
    // 크래시 처리 예산(밀리초)이다. 0이면 PlatformBase 기본값이다(platform::CrashReportingOptions의
    // 같은 이름 칸 설명). 종료 콜백 전체 예산, 도우미 캡처 마감, 프로세스 안 덤프 마감, 겹친 실패
    // 스레드의 여유 순이다. 범위 밖이면 설정 단계가 경고하고 이전 설정을 유지한다.
    uint32 crashCallbackBudgetMilliseconds = 0;
    uint32 crashCaptureTimeoutMilliseconds = 0;
    uint32 inProcessDumpTimeoutMilliseconds = 0;
    uint32 concurrentCrashGraceMilliseconds = 0;
    // 디버거가 붙어 있어도 보고한다(기본은 디버거에 넘긴다).
    bool reportWhenDebuggerAttached = false;
    // Windows 힙 손상(STATUS_HEAP_CORRUPTION)을 보고한다(선택 기능).
    bool captureHeapCorruption = false;
    // 크래시 폴더의 뿌리(UTF-8)다. nullptr이면 "Saved/Crashes"다. 설정 적용 단계까지 유효해야 한다
    // (ProcessCommandLine 인자의 Data()는 프로세스 수명 동안 유효하다).
    const char* crashDirectory = nullptr;
    // 덤프 도우미로 띄울 실행 파일(UTF-8)이다. nullptr이면 자기 실행 파일이다(배포 구성·시험용).
    const char* crashHelperPath = nullptr;
    // OS·CRT 오류 대화상자를 끈다(platform::SuppressSystemErrorDialogs). TestRunner는 늘 끈다.
    bool suppressSystemErrorDialogs = false;
    // Ctrl-C·Ctrl-Break(POSIX SIGINT·SIGTERM)의 첫 요청을 원자 표시(IsTerminationRequested)로
    // 받는다. 두 번째 요청은 OS 기본 동작(즉시 종료)이다. 끄면 지금처럼 바로 끝난다. POSIX에서 설치
    // 전 동작이 무시(SIG_IGN)인 신호는 그대로 둔다. 내장 진입은 호스트의 처리기를 지키려고 걸지
    // 않는다.
    bool handleTerminationRequests = false;
    ToolLogging toolLogging = ToolLogging::None;
    // 범용 할당자(ApplicationContext::DefaultAllocator)의 종류다. 범용 할당자를 만들기 전에(본문
    // 전에) 정해지므로 description의 기본값이나 configure에서 바꾼다.
    DefaultAllocatorKind defaultAllocator = DefaultAllocatorKind::Binned;
    // 본문을 다른 스레드에서 돌리는 호스트다(MainThreadHostFunction). nullptr이면 본문을 이
    // 스레드에서 바로 부른다(기본). 호스트가 있으면 ApplicationContext는 본문 스레드에서 쓴다. 내장
    // 진입은 호스트의 메인 스레드를 바꾸지 않으려고 무시한다.
    //
    // 사용 예(configure에서):
    //   settings.mainThreadHost = &platform::RunCocoaMainThreadHost;
    MainThreadHostFunction mainThreadHost = nullptr;
};

// 본문이 받는 실행 문맥이다. Launch가 만들고 본문이 돌아갈 때까지 유효하다(복사하지 않는다). 본문을
// 부른 스레드(주 스레드)에서 쓴다.
class ROCKS_LAUNCH_API ApplicationContext {
public:
    ApplicationContext(const ApplicationContext&) = delete;
    ApplicationContext& operator=(const ApplicationContext&) = delete;

    // 프로그램 인자(실행 파일 제외, UTF-8)다. ProcessCommandLine::Get().Arguments()와 같다. 각
    // Data()는 NUL로 끝난다.
    Span<const StringView> Arguments() const noexcept;

    // 실행 파일의 절대 경로(UTF-8)다.
    StringView ExecutablePath() const noexcept;

    // 프로세스의 범용 할당자다. 엔진·모듈 로더·컨테이너처럼 본문이 만드는 것에 넘긴다. 기본은
    // Launch의 SystemAllocator 위에 둔 BinnedAllocator이고(여러 스레드에서 함께 부를 수 있다),
    // ApplicationSettings::defaultAllocator가 System이면 SystemAllocator()와 같은 객체다.
    // - 수명: 본문이 돌아갈 때까지 유효하다. 본문은 돌아가기 전에 여기서 받은 할당을 모두 반환하고
    //   이 할당자를 쓰는 스레드를 끝낸다. 할당을 남기면 Launch의 정리가 Fatal로
    //   끝난다(BinnedAllocator "destroyed with live allocations" — SystemAllocator를 쓸 때와 같은
    //   계약이다).
    // - RequestExitWithoutStaticTeardown을 불렀으면 Launch가 할당자를 파괴하지 않고 끝내므로 남은
    //   할당을 검사하지 않는다(내장 진입은 그 요청을 무시하므로 검사한다).
    // - 이 할당자를 쓴 스레드마다 캐시가 생기고(최대 BinnedAllocator::MaxThreadCacheBytes, 약
    //   2.25 MiB) 그 스레드가 끝날 때 돌아간다.
    //
    // 사용 예:
    //   Allocator& memory = context.DefaultAllocator();
    //   Array<int32> values(memory);
    Allocator& DefaultAllocator() noexcept;

    // Launch가 소유한 프로세스 모듈 로더다(Docs/Design/ModuleLoader 3.9·3.11절, UE
    // FModuleManager의 자리 — 전역 접근자는 없다). 본문 수명 동안 유효하다. Monolithic은 범용
    // 할당자로 이 실행 파일의 모듈 표(GeneratedModuleTable)에서, Modular는 Launch의 Modular 진입이
    // 시동 할당자(SystemAllocator)로 모듈 manifest에서 만들었다(3.14절 — 범용 할당자의 종류를
    // 정하는 앱 기술이 그 로더가 실은 호스트 모듈 안에 있다). 부트 모듈(EarliestPossible 단계 —
    // Core· ModuleLoader·Launch)이 시작해 있다. 본문은 단계를 이어 돈다: 엔진 호스트는 설정·로그 뒤
    // StartPhase(PostConfigInit)와 끝에 ShutdownGroup(Host)를, Engine(EngineConfig::modules)은 엔진
    // 단계를 돈다. 프로세스 묶음은 본문이 돌아간 뒤 Launch가 내린다(도구 로그 정리 전). 본문은 이
    // 로더의 lease를 잡은 등록(설정 문맥 등)을 돌아가기 전에 놓는다. 남으면 Launch가 Error로
    // 기록하고 로더 파괴가 Fatal이다(RequestExitWithoutStaticTeardown이면 내리지도 파괴하지도
    // 않는다).
    //
    // 사용 예:
    //   rocks::ModuleLoader& modules = context.Modules();
    //   rocks::EngineConfig config;
    //   config.modules = &modules;
    ModuleLoader& Modules() noexcept;

    // Launch가 소유한 시스템 할당기다(본문 수명 동안 유효). 범용 할당자의 상위이고 Launch의 도구
    // 로그가 쓴다. 본문은 보통 DefaultAllocator()를 쓰고, 이것은 두 할당자를 비교하거나 진단할 때
    // 쓴다.
    Allocator& SystemAllocator() noexcept;

    // ToolLogging::Stderr이고 도구 로그를 시작했으면 그 LogSystem(설치된 상태)이고, 아니면
    // nullptr이다.
    LogSystem* ToolLog() noexcept;

    // configure까지 적용한 실제 설정이다.
    const ApplicationSettings& Settings() const noexcept;

    // 크래시 기록의 단계 표시다(platform::SetRunPhase). stageName은 정적 수명 문자열이다.
    //
    // 사용 예: context.SetStage(platform::RunPhase::Running, "Commandlet.Run");
    void SetStage(platform::RunPhase phase, const char* stageName) noexcept;

    // 정적 소멸·CRT 종료를 돌리면 안 될 때(멈춘 로그 출력 스레드 등) 부른다. Launch가 정리(도구
    // 로그, 종료 요청 처리기, 크래시 수집 해제)를 마친 뒤 platform::ExitProcessWithoutTeardown(본문
    // 종료 코드)로 끝낸다(공유 라이브러리의 정적 소멸도 돌지 않는다 — Modular). stdio
    // 버퍼는 비우지 않으므로 본문이 먼저 비운다. 내장 진입(RunEmbeddedApplication)은 호스트
    // 프로세스를 끝내지 않으므로 무시한다.
    void RequestExitWithoutStaticTeardown() noexcept;

private:
    friend class ApplicationRunner;

    ApplicationContext() noexcept = default;

    Allocator* allocator_ = nullptr;
    Allocator* defaultAllocator_ = nullptr;
    ModuleLoader* modules_ = nullptr;
    LogSystem* toolLog_ = nullptr;
    const ApplicationSettings* settings_ = nullptr;
    bool exitWithoutStaticTeardown_ = false;
};

// 명령줄을 캡처하지 못했을 때의 기본 종료 코드다(stderr에 한 줄을 남긴다).
inline constexpr int CommandLineFailureExitCode = 1;

// 모듈 로더를 만들지 못했거나 부트 모듈이 시작하지 못했을 때의 종료 코드다(Launch가 Error로 원인을
// 한 번 기록한다 — Docs/Design/ModuleLoader 3.11절).
inline constexpr int ModuleStartupFailureExitCode = 1;

// 실행 파일 설명이다. 보통 정적 상수로 두고 Describe 함수가 돌려준다.
struct ApplicationDescription {
    // 프로그램 이름(정적)이다. 크래시 폴더·메타데이터 "program"이므로 ASCII 글자·숫자·'.'·'_'·'-'만
    // 쓴다.
    const char* name = nullptr;
    ApplicationKind kind = ApplicationKind::Tool;
    // 설정 기본값이다. 크래시 수집은 명령줄 캡처보다 먼저 이 값으로 설치한다.
    ApplicationSettings settings{};
    // 선택. 명령줄 캡처 뒤, 본문 전에 설정을 고친다(시험 probe의 크래시 수집 끄기 등). 크래시
    // 설정이 바뀌면 Launch가 수집기를 다시 설치한다(그 사이 아주 짧게 보호가 없다).
    void (*configure)(Span<const StringView> arguments,
                      ApplicationSettings& settings) noexcept = nullptr;
    // 본문이다. 반환값이 프로세스 종료 코드다. 필수다.
    int (*run)(ApplicationContext& context) noexcept = nullptr;
    // 명령줄 캡처 실패(잘못된 인코딩·상한 초과)의 종료 코드다. 사용법 오류를 따로 정한 도구가
    // 바꾼다(예: Commandlet은 2).
    int commandLineFailureExitCode = CommandLineFailureExitCode;
};

// 진입점 공통 본문이다. 실행 파일은 직접 부르지 않고 EntryPoint.h의 매크로를 쓴다.
// 순서(설계 8.1절): 도우미 모드 분기 → 이른 크래시 수집 설치 → 보호 프레임 안에서 명령줄 캡처 →
// configure → 설정 적용(대화상자·크래시 설정·도우미 시작) → 종료 요청 처리기 → 범용 할당자 → 도구
// 로그 → 모듈 로더(부트 모듈) → 본문 → 시작한 단계만 역순 정리 → 보호 프레임 밖에서 크래시 수집
// 해제 → 종료 코드. 모듈 로더 단계가 실패하면 본문 없이 ModuleStartupFailureExitCode다.
// 초기화 실패(명령줄 캡처)는 성공한 단계만 역순으로 정리하고 종료 코드는
// description.commandLineFailureExitCode다(stderr 한 줄에 원인과 상한을 적는다). 정리는 반환 경로
// 하나에서 한 번만 한다. 크래시 종료는 이 경로를 타지 않는다. description.name이나 run이 없으면
// platform::Fatal이다(프로그램 버그).
int RunApplication(const ApplicationDescription& description, int argc, wchar_t** argv) noexcept;
int RunApplication(const ApplicationDescription& description, int argc, char** argv) noexcept;

// Rocks를 내장한 호스트가 자기 진입점에서 부른다(프로세스에 한 번, 설계 8.8절). 순서는
// RunApplication과 같되 다음이 다르다. 덤프 도우미 모드로 분기하지 않는다. 크래시 수집은 내장
// 보고다(호스트의 처리되지 않은 예외 filter·신호 처리기를 바꾸지 않고, Rocks 보호 프레임·Fatal에서
// 보고한 뒤 예외를 호스트로 넘긴다. Off면 끈다). 종료 요청 처리기를 걸지 않는다.
// RequestExitWithoutStaticTeardown은 무시한다(프로세스 종료는 호스트가 정한다).
//
// 사용 예(호스트의 wmain):
//   return rocks::launch::RunEmbeddedApplication(Describe(), argc, argv);
int RunEmbeddedApplication(const ApplicationDescription& description, int argc,
                           wchar_t** argv) noexcept;
int RunEmbeddedApplication(const ApplicationDescription& description, int argc,
                           char** argv) noexcept;

// Windows GUI 진입점(wWinMain)용이다. CRT가 나눈 넓은 문자 인자(__argc·__wargv)로 RunApplication을
// 부른다. 인자를 얻지 못하면 stderr에 한 줄을 남기고 description.commandLineFailureExitCode다.
int RunWindowedApplication(const ApplicationDescription& description) noexcept;

// Modular 호스트 모듈이 export하는 앱 기술 함수의 이름이다(Launch/EntryPoint.h — 창 실행 파일은
// Windows에서 따로).
inline constexpr const char* ApplicationDescriptionSymbol = "RocksApplicationDescription";
inline constexpr const char* WindowedApplicationDescriptionSymbol =
    "RocksWindowedApplicationDescription";

// 프로세스 종료 요청을 원자 표시로 남긴다. 아무 스레드·신호 처리기에서 부를 수 있다. 엔진 호스트는
// 게임 스레드에서 IsTerminationRequested()를 보고 Engine::RequestExit()로 옮긴다(설계 20절).
ROCKS_LAUNCH_API void RequestTermination() noexcept;
ROCKS_LAUNCH_API bool IsTerminationRequested() noexcept;

} // namespace rocks::launch

#if !ROCKS_MONOLITHIC
// Modular 실행 파일의 진입이다(Docs/Design/ModuleLoader 3.14절). 빌드가 만든 실행 파일 진입
// TU(<모듈>.Launcher.cpp)만 부른다. 실행 파일 폴더의 manifest(manifest — 파일 이름)로 모든 모듈
// 공유 라이브러리를 싣는 로더를 시동 할당자에서 만들고, 호스트 모듈이 export한 앱 기술로
// RunApplication과 같은 흐름을 돈다("Launch.Modules"는 이 로더로 부트 모듈만 시작한다). 로더는 실행
// 흐름이 끝난 뒤(크래시 수집 해제 뒤) 파괴한다 — 앱 기술과 그 문자열이 호스트 모듈 안에 있다.
// manifest·적재·앱 기술 조회가 실패하면 stderr에 한 줄(경로·report)을 쓰고
// ModuleStartupFailureExitCode다(크래시 수집·도구 로그는 앱 기술이 정하므로 아직 없다).
// 크래시 도우미 분기가 맨 먼저다. 세 진입은 모든 플랫폼에 있고 진입 TU가 플랫폼에 맞는 것을 고른다
// (Windows 콘솔 wmain → 넓은 문자, Windows 창 wWinMain → 창, POSIX main → 좁은 문자).
extern "C" {
// 좁은 문자 진입(POSIX main)이다.
ROCKS_LAUNCH_API int RocksLaunchModularApplication(int argc, char** argv,
                                                   const char* manifest) noexcept;
// 넓은 문자 진입(Windows wmain)이다.
ROCKS_LAUNCH_API int RocksLaunchModularWideApplication(int argc, wchar_t** argv,
                                                       const char* manifest) noexcept;
// 창 실행 파일 진입(Windows wWinMain)이다. CRT가 나눈 넓은 문자 인자를 쓰고(없으면 stderr 한 줄과
// CommandLineFailureExitCode) 앱 기술은 RocksWindowedApplicationDescription이다.
ROCKS_LAUNCH_API int RocksLaunchModularWindowedApplication(const char* manifest) noexcept;
}
#endif
