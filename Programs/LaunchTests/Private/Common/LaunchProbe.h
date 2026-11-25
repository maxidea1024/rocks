#pragma once

#include <Launch/Application.h>

// Launch 시험 실행 파일의 공용 본문이다(Rocks.ToolTests Native.Launch.*가 자식으로 띄운다).
//
// 사용: <실행 파일> [옵션...] <mode> [mode 인자]
// 옵션(configure가 읽는다):
//   --crash-dir <dir>  크래시 폴더 뿌리
//   --crash-off        크래시 수집을 끈다(설치했다가 설정 단계에서 뗀다)
//   --in-process       도우미 없이 프로세스 안 덤프(설치했다가 설정 단계에서 다시 건다)
//   --crash-helper <p> 덤프 도우미 실행 파일 경로(없는 경로면 시작 실패 경고)
//   --bad-budget       범위 밖 크래시 예산(설정 단계가 경고하고 이전 설정을 유지한다)
//   --tool-log         도구 로그(stderr sink)를 시작한다
//   --terminate        종료 요청 처리기를 건다
//   --system-allocator 범용 할당자를 SystemAllocator로 둔다(기본은 BinnedAllocator)
//   --reset-sigint     물려받은 SIGINT 무시를 기본 동작으로 되돌린다(Launch 설치 전)
//   --ignore-sigint    SIGINT를 무시로 둔다(Launch 설치 전, 상속된 무시 보존 시험)
// mode(본문):
//   args         인자·실행 파일 경로를 UTF-8 hex 줄로 쓴다
//                ("LAUNCH ARG <i> <hex>", "LAUNCH EXE <hex>")
//   exit <n>     n을 종료 코드로 돌려준다
//   state        크래시 수집·도우미·도구 로그·종료 요청 상태 줄과 크래시 폴더 줄을 쓴다
//                (폴더 줄은 "LAUNCH FOLDER <hex>")
//   log          도구 로그로 Info 한 줄을 남긴다
//   crash        본문에서 접근 위반을 낸다
//   fatal        본문에서 platform::Fatal을 부른다
//   allocator    범용 할당자를 본다: 작은 블록 64개와 큰 블록 하나를 받고 돌려주며 Launch의 시스템
//                할당자에 닿은 수를 적는다("LAUNCH ALLOCATOR kind=<binned|system> distinct=<0|1>
//                smallInSystem=<n> largeInSystem=<n> clean=<0|1>"). 모두 돌려줬으면 0이다
//   allocator-leak  범용 할당자의 할당 하나를 남긴 채 AllocatorLeakExitCode를 돌려준다(Launch의
//                정리가 Fatal로 끝나야 한다)
//   allocator-leak-no-teardown  같지만 RequestExitWithoutStaticTeardown을 부른다(Launch가 할당자를
//                파괴하지 않으므로 Fatal 없이 그 종료 코드로 끝난다)
//   teardown     정적 소멸자 표시를 건 뒤 7을 돌려준다(정적 소멸이 한 번 돈다)
//   no-teardown  같지만 RequestExitWithoutStaticTeardown으로 정적 소멸 없이 끝낸다
//   signal-self  실제 OS 종료 요청(Ctrl-Break·SIGINT)을 자신에게 보내고, 종료 요청 표시가 켜지면
//                "LAUNCH TERMINATION REQUESTED"를 쓰고 0이다(--terminate와 함께). 처리기가 없으면
//                OS 기본 동작으로 끝난다. 시험할 수 없는 환경이면 "LAUNCH SIGINT IGNORED"
//                (SignalIgnoredExitCode)나 "LAUNCH CONSOLE SHARED"(SharedConsoleExitCode)를 쓰고
//                끝난다
//   signal-twice 같지만 표시가 켜진 뒤 한 번 더 보낸다(두 번째는 OS 기본 동작으로 끝난다)
//   install-invalid  잘못된 옵션 설치가 InvalidArgument로 거절되고 설치 상태가 아닌지 본다
//                (--crash-off와 함께)
//   threads <n>  platform::Thread 시작·join 왕복을 n번 재고 "LAUNCH THREADS <n> <ns 합계>"를
//                쓴다(비용 측정, 설계 18절. 스레드마다 stack 보장은 크래시 수집 설치와 무관하게
//                건다)
// 모르는 mode는 종료 코드 ProbeUsageExitCode다.
namespace rocks::launch::probe {

inline constexpr int ProbeUsageExitCode = 64;
// allocator-leak mode의 본문 종료 코드다(Fatal이 아니면 이 값으로 끝난다).
inline constexpr int AllocatorLeakExitCode = 9;

void Configure(Span<const StringView> arguments, ApplicationSettings& settings) noexcept;
int Run(ApplicationContext& context) noexcept;

// 시험 실행 파일 설명을 만든다(이름과 크래시 수집 기본값만 다르다). 내장 호스트는 같은 설명을
// 내장 진입(RunEmbeddedApplication)으로 부른다.
constexpr ApplicationDescription MakeDescription(const char* name,
                                                 CrashReportingMode crashReporting) noexcept {
    ApplicationDescription description;
    description.name = name;
    description.kind = ApplicationKind::TestRunner;
    description.settings.crashReporting = crashReporting;
    description.configure = &Configure;
    description.run = &Run;
    return description;
}

// 실제 OS 종료 요청을 보낼 수 있는 환경인가다. Ignored: POSIX SIGINT를 무시로 물려받았다.
// SharedConsole: Windows 콘솔을 다른 프로세스와 나눠 쓴다(Ctrl-Break가 그쪽에도 간다).
enum class SignalReadiness : uint8 {
    Ready,
    Ignored,
    SharedConsole,
};

// signal-* mode가 그 환경에서 시험할 수 없을 때의 종료 코드다(C#이 명시적으로 다룬다).
inline constexpr int SignalIgnoredExitCode = 65;
inline constexpr int SharedConsoleExitCode = 66;
// host-thread 모드가 메인 스레드 호스트가 만든 스레드에서 돌았을 때의 종료 코드다(호스트가 본문
// 반환값을 그대로 넘기는지 본다 — Docs/Design/MacPlatform.ko.md 3절).
inline constexpr int HostedBodyExitCode = 67;

// 물려받은 SIGINT 무시를 기본 동작으로 되돌린다(--reset-sigint, configure에서 Launch 설치 전에
// 부른다). Windows는 할 일이 없다.
void ResetIgnoredTerminationSignal() noexcept;

// SIGINT를 무시로 둔다(--ignore-sigint, configure에서 Launch 설치 전에 부른다). Launch가 상속된
// 무시를 보존하는지 보는 시험용이다. Windows는 할 일이 없다.
void IgnoreTerminationSignal() noexcept;

SignalReadiness CheckTerminationSignal() noexcept;

// 이 프로세스에 실제 OS 종료 요청을 보낸다(Windows 콘솔 Ctrl-Break, POSIX SIGINT). 보냈으면 true다.
bool RaiseTerminationSignal() noexcept;

// stderr에 한 줄을 쓴다(probe 출력, 로그와 무관하게 harness가 읽는다).
void WriteProbeLine(const char* text) noexcept;

} // namespace rocks::launch::probe
