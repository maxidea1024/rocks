#pragma once

#include <Base/Compiler.h>
#include <Base/NumericLimits.h>
#include <Base/Types.h>

// 네이티브 크래시 수집기다(Docs/Design/CrashHandlingAndLaunch.ko.md). 프로세스에 하나이며 OS 처리기
// (Windows unhandled exception filter, POSIX 치명 신호)·크래시 처리 스레드·스레드별 여유 stack을
// 모두 이 수집기가 소유한다. 다른 모듈(로그 등)은 종료 콜백만 등록한다.
//
// 크래시 흐름: 실패한 스레드가 프로세스 전역 보고 소유권을 원자적으로 얻고, 예외 문맥을 정적
// 저장소로 복사한 뒤 크래시 처리 스레드를 깨워 기다린다. Windows는 설치 때 같은 실행 파일을 덤프
// 도우미 모드로 띄워 둔다. 크래시 처리 스레드는 도우미에게 기록을 넘기고, 도우미가 부모를
// snapshot(PssCaptureSnapshot)으로 캡처했다는 신호를 받은 뒤 종료 콜백(로그 flush)을 부른다. 폴더·
// CrashInfo.json·덤프는 도우미가 snapshot에서 쓴다(부모 종료와 무관). 도우미가 없거나 실패하면
// 크래시 처리 스레드가 종료 콜백 뒤 프로세스 안에서 같은 파일을 쓴다(결과에 표시). POSIX는 처리
// 스레드가 CrashInfo.json만 쓴다. 실패한 스레드는 그 뒤 프로세스를 끝낸다(Embedded는 호스트로
// 넘긴다). 겹쳐 들어온 스레드는 소유자의 종료를 기다리고, 같은 스레드의 재귀는 즉시 끝낸다.
// Windows는 abort()·CRT invalid parameter·pure virtual call도 같은 경로로 받는다(반환하지 않는다).
//
// 결과: <outputDirectory>/<programName>-<시작 UTC YYYYMMDDTHHMMSSZ>-<pid>/ 아래 CrashInfo.json과
// Minidump.dmp(Windows)다. 폴더를 만들 수 없으면 임시 폴더(Windows %TEMP%\Rocks\Crashes,
// POSIX $TMPDIR/Rocks/Crashes)에서 한 번 더 시도하고, 그래도 안 되면 stderr에 한 줄 남긴다.
//
// 비용: 정상 실행 중 기록·프레임마다 드는 비용은 없다. 프로세스당 대기만 하는 스레드 하나와 이벤트
// 둘, 정적 저장소(약 40 KiB), Windows dbghelp.dll 로드, 스레드마다 여유 stack
// CrashStackReserveBytes(Windows 보장분, POSIX sigaltstack)가 든다.
//
// 사용 예(실행 파일 진입점, 다른 스레드를 만들기 전):
//   platform::CrashReportingOptions options;
//   options.programName = "Sandbox";
//   if (platform::InstallCrashReporting(options) != platform::CrashReportingStatus::Installed) {
//   ... }
//   ... 본문 ...
//   platform::RemoveCrashReporting();   // 다른 Rocks 스레드가 모두 끝난 뒤
namespace rocks::platform {

// 이 플랫폼·구성에서 크래시 수집이 실제로 할 수 있는 것이다(비트 묶음). 할 수 없는 것은 비트를
// 끈다.
enum class CrashCapability : uint32 {
    None = 0,
    OsFaultHandling = 1u << 0,      // 접근 위반·치명 신호를 잡는다
    ThreadGuard = 1u << 1,          // platform::Thread 진입을 보호한다(Windows SEH)
    Minidump = 1u << 2,             // 분석 가능한 덤프 파일(Windows, sanitizer 구성 제외)
    OutOfProcessDump = 1u << 3,     // 도우미 프로세스가 덤프를 쓴다(Windows, sanitizer 구성 제외)
    Metadata = 1u << 4,             // CrashInfo.json
    TerminationCallbacks = 1u << 5, // 크래시 처리 스레드에서 종료 콜백을 부른다
    SystemCrashReport = 1u << 6,    // 처리 뒤 OS 수집기(ReportCrash·tombstone)에 넘긴다
    UnreportedExitRecord =
        1u << 7, // 도우미가 보고 없는 비정상 종료를 기록한다(OutOfProcessDump와 같다)
};

constexpr CrashCapability operator|(CrashCapability left, CrashCapability right) noexcept {
    return static_cast<CrashCapability>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr bool HasCrashCapability(CrashCapability set, CrashCapability flag) noexcept {
    return (static_cast<uint32>(set) & static_cast<uint32>(flag)) == static_cast<uint32>(flag);
}

// 이 플랫폼·구성에서 크래시 수집이 할 수 있는 기능이다. 설치 전에는 빌드가 지원하는 최대 범위이고,
// 설치 뒤에는 실제 준비 상태를 반영한다(Windows: 도우미가 떠 있지 않으면 OutOfProcessDump·
// UnreportedExitRecord가 꺼지고, 도우미도 DbgHelp도 없거나 내장 모드면 Minidump도 꺼진다).
ROCKS_PLATFORMBASE_API CrashCapability QueryCrashCapabilities() noexcept;

// 덤프에 담는 범위다. 크게 할수록 분석할 수 있는 것이 늘지만 파일·작성 시간·개인정보 노출이 는다.
enum class CrashDumpDetail : uint8 {
    // 스레드·모듈·stack·stack이 가리키는 메모리(기본). 명령줄·환경 블록(PEB)은 넣지 않는다.
    Standard,
    // Standard 덤프에 더해 Minidump.detail.dmp를 쓴다: 데이터 섹션·핸들·PEB/TEB(명령줄·환경 변수가
    // 들어간다)·메모리 지도. 사용자가 명시적으로 켠다.
    Detailed,
    // Detailed에 전체 프로세스 메모리까지 넣는다(크다).
    Full,
};

// 덤프를 누가 쓰는가다.
enum class CrashHelperPolicy : uint8 {
    // 설치 때 도우미 프로세스(같은 실행 파일, RunCrashHelperIfRequested)를 띄운다. 띄우지 못하거나
    // 크래시 때 응답하지 않으면 프로세스 안 덤프로 대신하고 결과에 표시한다(Windows만, 다른
    // 플랫폼은 InProcess와 같다).
    Preferred,
    // 도우미 없이 크래시 처리 스레드가 프로세스 안에서 쓴다.
    InProcess,
    // 설치 때는 띄우지 않고 StartCrashHelper()가 띄운다. 그 전의 크래시는 InProcess처럼 처리한다.
    // 공통 실행 계층(Launch)이 설정을 다 읽은 뒤 도우미를 띄울 때 쓴다(설정이 수집을 끄는 시험
    // probe가 도우미를 띄웠다 바로 끝내지 않는다).
    Deferred,
};

struct CrashReportingOptions {
    static constexpr uint32 DefaultCallbackBudgetMilliseconds = 2000;
    static constexpr uint32 DefaultCaptureTimeoutMilliseconds = 10000;
    // 도우미의 덤프 마감이다. 기본 덤프(Minidump.dmp, 늘 Standard)와 Detailed·Full 추가 덤프.
    static constexpr uint32 DefaultHelperStandardDumpDeadlineMilliseconds = 60000;
    static constexpr uint32 DefaultHelperDetailedDumpDeadlineMilliseconds = 120000;
    static constexpr uint32 DefaultHelperFullDumpDeadlineMilliseconds = 600000;
    static constexpr uint32 DefaultConcurrentCrashGraceMilliseconds = 5000;
    static constexpr uint32 DefaultInProcessDumpTimeoutMilliseconds = 30000;
    static constexpr uint32 DefaultMetadataTimeoutMilliseconds = 2000;
    // 문자열 칸의 byte 상한이다(NUL 제외). 넘으면 설치가 InvalidArgument다.
    static constexpr usize MaxProgramNameBytes = 63;
    static constexpr usize MaxOutputDirectoryBytes = 1023;
    static constexpr usize MaxIdentityBytes = 1023;
    static constexpr usize MaxHelperExecutableBytes = 1023;
    // argumentCount의 "모름" 표지다(메타데이터 "arguments"가 null이다).
    static constexpr uint32 UnknownArgumentCount = NumericLimits<uint32>::Max();

    // 필수. 크래시 폴더 이름과 메타데이터의 "program"이다. ASCII 글자·숫자·'.'·'_'·'-'만 쓴다
    // (파일 이름으로 그대로 쓴다). 설치 때 복사한다.
    const char* programName = nullptr;
    // 크래시 폴더의 뿌리(UTF-8)다. nullptr이면 "Saved/Crashes"다. 상대 경로는 설치 때 현재 작업
    // 폴더 기준 절대 경로로 굳힌다. 굳힌 절대 경로도 MaxOutputDirectoryBytes(Windows는 UTF-16 같은
    // 단위 수)를 넘으면 설치가 InvalidArgument다(모든 플랫폼 공통, macOS PATH_MAX에 맞춘다). 설치
    // 때 복사한다.
    const char* outputDirectory = nullptr;
    // 빌드 식별 정보다. JSON 객체의 멤버 목록을 중괄호 없이 UTF-8로 준다(예: 문자 그대로
    // "engineVersion": "0.1.0", "toolBuildId": "..."). 메타데이터 "build" 객체에
    // 구성·플랫폼·아키텍처 뒤에 그대로 붙는다. 올바른 JSON 조각인지는 부르는 쪽(Launch)이 책임진다.
    // nullptr이면 붙이지 않는다. 설치 때 복사한다.
    const char* identity = nullptr;
    // true: 호스트 프로세스에 내장된 경우다. OS 처리기·CRT hook을 걸지 않고, Rocks 보호 프레임·
    // Fatal에서 보고(종료 콜백·메타데이터, 덤프 없음) 뒤 예외를 호스트 처리기로 넘긴다. 프로세스는
    // 호스트가 끝낸다.
    bool embedded = false;
    // 디버거가 붙어 있어도 보고한다. 기본은 디버거에 넘겨 원래 실패 지점에서 멈추게 한다.
    bool reportWhenDebuggerAttached = false;
    // Windows: 힙 손상(STATUS_HEAP_CORRUPTION)을 vectored exception handler로 받아 보고한다(선택
    // 기능, 기본 끔). 힙 손상은 일반 unhandled filter에 오지 않는다. 다른 플랫폼은 무시한다.
    bool captureHeapCorruption = false;
    CrashDumpDetail dumpDetail = CrashDumpDetail::Standard;
    CrashHelperPolicy helper = CrashHelperPolicy::Preferred;
    // 도우미로 띄울 실행 파일(UTF-8)이다. nullptr이면 자기 실행 파일이다. 그 실행 파일의 진입점은
    // 다른 무엇보다 먼저 RunCrashHelperIfRequested를 불러야 한다. 배포 구성·시험용이다. 설치 때
    // 복사한다.
    const char* helperExecutable = nullptr;
    // 크래시 때 부모가 도우미의 캡처 끝 신호를 기다리는 상한이다. 넘으면 요청을 거두고 프로세스 안
    // 덤프로 대신한다.
    uint32 captureTimeoutMilliseconds = DefaultCaptureTimeoutMilliseconds;
    // 도우미의 추가 덤프(Detailed·Full) 마감이다. 0이면 종류별 기본값이다. 넘으면 취소하고 그
    // 파일을 지운다(기본 덤프는 남는다).
    uint32 helperDumpDeadlineMilliseconds = 0;
    // 종료 콜백 전체의 공용 예산이다.
    uint32 callbackBudgetMilliseconds = DefaultCallbackBudgetMilliseconds;
    // 겹쳐 실패한 스레드가 소유자의 마감 뒤 더 기다리는 시간이다(소유자가 멈춘 경우의 안전장치).
    uint32 concurrentCrashGraceMilliseconds = DefaultConcurrentCrashGraceMilliseconds;
    // 프로세스 안 덤프 작성의 마감이다(Detailed·Full의 추가 덤프까지 합친 시간). 넘으면 취소하고
    // 그 파일을 지운다(취소된 덤프는 열리지 않는다).
    uint32 inProcessDumpTimeoutMilliseconds = DefaultInProcessDumpTimeoutMilliseconds;
    // 폴더·CrashInfo.json 쓰기에 잡는 시간이다(취소할 수 없는 파일 API라 대기 예산에만 쓴다).
    uint32 metadataTimeoutMilliseconds = DefaultMetadataTimeoutMilliseconds;
    // 프로그램 인자 수(실행 파일 제외)다. 메타데이터에는 이 개수만 남긴다(원문은 수집하지 않는다).
    // Launch가 진입점 argc로 채운다.
    uint32 argumentCount = UnknownArgumentCount;
};

enum class CrashReportingStatus : uint8 {
    Installed,
    AlreadyInstalled, // 두 번째 설치(부트 순서 버그). 상태는 바뀌지 않는다
    InvalidArgument,  // 이름 없음·문법, 길이 상한, 예산 0·상한 초과
    OutOfResources,   // 크래시 처리 스레드·이벤트를 만들지 못했다. 아무것도 걸지 않는다
};

// 수집기를 설치한다. 부른 스레드(보통 주 스레드)의 여유 stack도 준비한다. 크래시 폴더 경로·크래시
// id·빌드 식별·모듈(PE CodeView) 정보를 여기서 고정 버퍼에 만든다. Windows는 OS unhandled filter·
// SIGABRT 처리기·invalid parameter·purecall 처리기(그리고 선택한 힙 손상 VEH)를 걸고, CRT abort
// 설정에서 메시지·WER 보고를 끈다(제거 때 모두 되돌린다). 실패하면 아무것도 걸지 않는다.
ROCKS_PLATFORMBASE_API CrashReportingStatus
InstallCrashReporting(const CrashReportingOptions& options) noexcept;

// InstallCrashReporting이 InvalidArgument로 거절할 옵션인지 미리 본다(이름 문법·문자열 길이·예산
// 범위). 상태를 바꾸지 않고 설치 여부와 무관하게 부를 수 있다. true여도 폴더 경로를 절대 경로로
// 굳히지 못하거나 자원이 모자라면 설치는 실패할 수 있다. 설치된 수집기를 다른 설정으로 바꾸기 전에
// 쓴다(떼기 전에 새 설정을 확인한다).
//
// 사용 예:
//   if (!platform::ValidateCrashReportingOptions(next)) { ... 경고, 지금 설치를 유지 ... }
ROCKS_PLATFORMBASE_API bool
ValidateCrashReportingOptions(const CrashReportingOptions& options) noexcept;

// 처리기를 떼고 이전 처리기를 되돌린 뒤 크래시 처리 스레드를 끝낸다. 다른 Rocks 스레드가 모두 끝난
// 종료 단계에서만 부른다. 이미 보고가 진행 중이면 돌아오지 않는다(종료는 크래시한 스레드가 한다).
// 설치하지 않았으면 아무 일도 하지 않는다. Windows 처리되지 않은 예외 filter는 지금 filter가 우리
// 것일 때만 설치 전 filter로 되돌리고, 다른 코드가 바꿔 둔 filter는 그대로 둔다(그 filter가 우리
// filter로 이어 주면 우리 filter는 설치 전 filter로 계속 넘긴다). 교환으로 확인하므로 두 호출
// 사이의 아주 짧은 창에서 다른 스레드의 처리되지 않은 예외는 설치 전 filter가 받는다.
ROCKS_PLATFORMBASE_API void RemoveCrashReporting() noexcept;

ROCKS_PLATFORMBASE_API bool IsCrashReportingInstalled() noexcept;

// 크래시 폴더의 뿌리를 바꾼다(UTF-8, 상대 경로는 지금 작업 폴더 기준으로 굳힌다). 설정을 읽은 뒤
// 엔진 시작 전, 다른 스레드가 크래시를 낼 수 없는 부트 단계에서만 부른다. 설치 전이거나 경로가
// 잘못됐거나 길면 false이고 이전 값을 유지한다.
//
// 사용 예:
//   if (!platform::SetCrashOutputDirectory(settings.crashDirectory)) { ... 경고 ... }
ROCKS_PLATFORMBASE_API bool SetCrashOutputDirectory(const char* directory) noexcept;

// Windows 처리되지 않은 예외 filter가 아직 수집기 것인지 교환으로 확인한다(설계 8.6절). 다른
// 코드(드라이버·외부 라이브러리)가 SetUnhandledExceptionFilter로 바꿨으면 그 filter를 그대로
// 되돌려 두고 false다(남의 선택을 조용히 빼앗지 않는다. Rocks 스레드와 주 스레드는 보호 프레임이
// 따로 있어 영향받지 않고, 바꾼 filter가 우리 filter로 이어 주지 않으면 외부 스레드의 크래시가
// 보고되지 않을 수 있다). 교환 두 호출 사이의 아주 짧은 창에서는 우리 filter가 잠시 다시 걸린다.
// 설치 전·내장 모드·다른 플랫폼은 true다. 엔진 초기화 끝과 종료 시작처럼 다른 스레드가 filter를
// 바꾸지 않는 지점에서 부른다.
//
// 사용 예:
//   if (!platform::CheckUnhandledCrashFilter()) { ... Log::Warning ... }
ROCKS_PLATFORMBASE_API bool CheckUnhandledCrashFilter() noexcept;

// 크래시 메타데이터에 남길 연관 파일·식별 정보 칸이다. 크래시 때 "associatedFiles"에 들어간다.
// LogFile은 FileLogSink가 파일을 열거나 회전할 때 출력 스레드에서 현재 로그 파일 경로를 쓴다.
enum class CrashAnnotation : uint8 {
    LogDirectory,
    LogFile,
    TraceFile,
    Custom0,
    Custom1,
    Count
};

inline constexpr usize MaxCrashAnnotationBytes = 512;

// 칸에 text(UTF-8, bytes byte, NUL 종료 불필요)를 복사한다. bytes가 0이면 칸을 비운다. 할당·lock이
// 없고(seqlock 칸) 설치 전에도 부를 수 있다. 값은 프로세스 수명 동안 남는다. 정상 실행 중
// 드물게(파일 열기·회전 때) 부른다. 칸마다 쓰는 스레드는 하나여야 한다(동시에 두 스레드가 같은 칸을
// 쓰면 값이 섞일 수 있다). 크래시 처리는 쓰는 도중의 칸을 몇 번 다시 읽고, 끝내 일관되지 않으면
// null로 적는다. slot이 범위 밖이거나 bytes가 MaxCrashAnnotationBytes를 넘거나 text가 nullptr인데
// bytes가 0이 아니면 false이고 칸은 그대로다.
//
// 사용 예(FileLogSink, 파일을 연 출력 스레드):
//   using platform::CrashAnnotation;
//   platform::SetCrashAnnotation(CrashAnnotation::LogFile, path.Data(), path.ByteCount());
ROCKS_PLATFORMBASE_API bool SetCrashAnnotation(CrashAnnotation slot, const char* text,
                                               usize bytes) noexcept;

// 이 프로세스의 크래시 폴더(설치 때 굳힌 절대 경로, UTF-8)를 buffer에 NUL 종료로 복사하고 길이(NUL
// 제외)를 돌려준다. 설치 전이거나 buffer가 모자라면 0이다. 폴더는 크래시 때 만들어진다(지금은 없을
// 수 있다). 만들 수 없으면 임시 폴더 아래 같은 이름을 쓴다. 시작 로그·진단용이다.
ROCKS_PLATFORMBASE_API usize CopyCrashReportFolder(char* buffer, usize capacity) noexcept;

// StartCrashHelper의 결과다.
enum class CrashHelperStart : uint8 {
    // 도우미가 떴다.
    Started,
    // 설치하지 않았거나 Deferred가 아니거나 이미 불렀거나, 내장·sanitizer 구성·도우미가 없는
    // 플랫폼(POSIX)이다. 아무 일도 하지 않았다.
    NotApplicable,
    // 띄우지 못했다. 크래시 때 프로세스 안 fallback이 쓰이고 결과에 "StartFailed"가 남는다.
    Failed,
};

// Deferred로 설치한 수집기의 덤프 도우미를 띄운다. 설정을 다 읽은 뒤, 다른 스레드가 크래시를 낼 수
// 없는 시작 단계에서 한 번 부른다.
//
// 사용 예(Launch, 설정 적용 단계):
//   if (platform::StartCrashHelper() == platform::CrashHelperStart::Failed) { ... 경고 ... }
ROCKS_PLATFORMBASE_API CrashHelperStart StartCrashHelper() noexcept;

// 지금 떠 있는 덤프 도우미의 프로세스 id다. 도우미가 없으면(설치 전, InProcess, 시작 실패, 내장,
// 다른 플랫폼) 0이다. 진단·시험용이다.
ROCKS_PLATFORMBASE_API uint32 CrashHelperProcessId() noexcept;

// 도우미 모드 진입점이다. 실행 파일의 main은 다른 무엇보다 먼저 부른다. 인자가 도우미 호출이면
// 도우미를 끝까지 돌리고 그 종료 코드를 exitCode에 둔 뒤 true다(main은 그 값을 반환한다). 아니면
// 아무 일도 하지 않고 false다. 도우미는 Rocks 로그·설정·할당기를 쓰지 않는다. 다른 플랫폼은 늘
// false다.
//
// 사용 예:
//   int main(int argc, char** argv) {
//       int helperExit = 0;
//       if (platform::RunCrashHelperIfRequested(argc, argv, helperExit)) { return helperExit; }
//       ...
//   }
ROCKS_PLATFORMBASE_API bool RunCrashHelperIfRequested(int argc, char** argv,
                                                      int& exitCode) noexcept;
// 넓은 문자 진입점(wmain·wWinMain)용이다. 도우미 인자는 ASCII다. 첫 인자가 도우미 표지가 아니면
// 다른 인자를 보지 않고 false다. 다른 플랫폼은 늘 false다.
ROCKS_PLATFORMBASE_API bool RunCrashHelperIfRequested(int argc, wchar_t** argv,
                                                      int& exitCode) noexcept;

// 자동 실행(시험·무인 도구)에서 OS·CRT 오류 대화상자를 끈다. 프로세스 설정이며 되돌리지 않는다.
// Windows: SetErrorMode(FAILCRITICALERRORS|NOGPFAULTERRORBOX|NOOPENFILEERRORBOX, 자식이
// 물려받는다), CRT abort 메시지·WER 보고 끄기, Debug CRT 보고(_CrtDbgReport)를 stderr로, WER
// 대화상자 끄기(WerSetFlags NO_UI). 레지스트리·시스템 WER 정책은 바꾸지 않는다. 다른 플랫폼은 할
// 일이 없다. 크래시를 성공 종료로 바꾸지 않는다(종료 코드는 그대로다).
ROCKS_PLATFORMBASE_API void SuppressSystemErrorDialogs() noexcept;

// 실행 단계다. stageName은 정적 수명 문자열(리터럴)이어야 한다. 크래시 기록이 남긴다.
enum class RunPhase : uint8 {
    Startup,
    Running,
    Shutdown
};

// 프로세스가 기록할 수 있는 서로 다른 단계 이름 수다.
inline constexpr uint32 MaxRunStageNames = 64;

// 단계와 단계 이름을 한 워드로 표시한다(다른 스레드의 크래시가 이름과 phase를 따로 읽지 않는다).
// 이름은 처음 쓸 때 프로세스 전역 이름 표(MaxRunStageNames칸)에 포인터로 한 번 등록하고, 같은
// 포인터는 같은 칸을 쓴다(문자열 내용은 비교하지 않으므로 같은 상수를 넘긴다). 표가 차면 그 뒤의 새
// 이름은 이름 없이 phase만 남는다(칸을 덮어쓰지 않는다). 등록한 칸은 풀지 않는다. stageName이
// nullptr이면 이름 없음이다. 아무 스레드에서 부를 수 있고 할당하지 않는다.
ROCKS_PLATFORMBASE_API void SetRunPhase(RunPhase phase, const char* stageName) noexcept;

// 지금 단계와 단계 이름이다(크래시 기록이 남길 값, 진단·시험용). 이름은 SetRunPhase에 넘긴 정적
// 문자열이고 아직 표시가 없으면 nullptr다. 아무 스레드에서 부를 수 있다.
ROCKS_PLATFORMBASE_API RunPhase CurrentRunPhase() noexcept;
ROCKS_PLATFORMBASE_API const char* CurrentRunStage() noexcept;

// 보호 호출이다. Windows는 SEH 경계 안에서 body를 부르고 처리되지 않은 예외를 공통 보고 경로로
// 보낸다. 다른 플랫폼은 그냥 부른다(신호 처리기가 프로세스 전역이다). body의 반환값을 돌려준다.
// body는 인라인되지 않는 별도 함수여야 한다(보호 블록 안 명령이 직접 낸 fault는 잡히지 않는다).
using GuardedBody = int (*)(void* context) noexcept;
ROCKS_PLATFORMBASE_API int RunWithCrashGuard(GuardedBody body, void* context) noexcept;

// 처리기용 여유 stack 크기다(Windows SetThreadStackGuarantee, POSIX sigaltstack). Asan 구성은
// ASan이 넘친 스레드에서 stack overflow 보고(stack trace 수집·기호 해석, 측정 약 29 KiB)를 한 뒤
// death callback으로 공통 경로에 오므로 크게 잡는다(설계 10.5·13.2절). 일반 구성은 보고·덤프가 4
// KiB 안에서 끝난다.
inline constexpr usize CrashStackReserveDefaultBytes = 32 * 1024;
inline constexpr usize CrashStackReserveSanitizerBytes = 128 * 1024;
inline constexpr usize CrashStackReserveBytes =
    ROCKS_ADDRESS_SANITIZER ? CrashStackReserveSanitizerBytes : CrashStackReserveDefaultBytes;

// 호출 스레드가 처리기를 돌릴 여유 stack을 확보한다. platform::Thread가 시작 때, 설치가 설치한
// 스레드에 부른다. 외부 라이브러리 스레드는 직접 부를 수 있다. 실패는 무시한다.
void PrepareThreadForCrashHandling() noexcept;

// PrepareThreadForCrashHandling이 그 스레드에 잡은 것을 돌려준다(POSIX 대체 stack).
// platform::Thread가 끝날 때 부른다.
void ReleaseThreadCrashHandling() noexcept;

// --- 종료 콜백(로그 flush 등)
// ---------------------------------------------------------------------

enum class TerminationReason : uint8 {
    Fatal,        // platform::Fatal·Log::Fatal
    Assert,       // ROCKS_ASSERT
    Exception,    // 하드웨어 예외(Windows)
    Signal,       // 치명 신호(POSIX)
    Abort,        // abort()·SIGABRT
    RuntimeCheck, // invalid parameter·pure virtual call
    Sanitizer,    // sanitizer 보고 뒤
};

enum class TerminationCallbackResult : uint8 {
    Completed,
    Skipped,
    TimedOut
};

inline constexpr uint32 MaxStoppedCrashThreads = 8;

// 콜백에 넘기는 값이다. 포인터는 콜백 동안 유효하다(정적 저장소).
struct TerminationContext {
    TerminationReason reason;
    // 처음 실패한 스레드의 CurrentThreadId() 값이다. 콜백을 부르는 스레드가 아니다.
    uint64 failingThreadId;
    // 겹쳐 실패해 멈춘 다른 스레드 id다. 콜백이 도는 동안에도 늘 수 있으므로 개수는
    // AtomicLoad로 읽는다(최대 MaxStoppedCrashThreads).
    const uint64* stoppedThreadIds;
    const uint32* stoppedThreadCount;
    // 종료 사유(NUL 종료)다. 비어 있을 수 있다.
    const char* message;
    usize messageBytes;
    // MonotonicNanoseconds() 기준 마감이다. 모든 콜백이 공유한다.
    uint64 deadlineNanoseconds;
};

// 크래시 처리 스레드에서 프로세스당 한 번 불린다. 막히는 lock·할당·레지스트리 순회를 하지 말고
// 마감 전에 돌아와야 한다. 결과는 크래시 기록에 남는다.
using TerminationCallback = TerminationCallbackResult (*)(const TerminationContext& context,
                                                          void* user) noexcept;

inline constexpr uint32 MaxTerminationCallbacks = 4;

struct TerminationCallbackHandle {
    uint32 slot = NumericLimits<uint32>::Max(); // "없음" 표지
    uint32 generation = 0;
};

// 콜백을 등록한다. 칸이 모자라거나 callback이 nullptr이면 false다. 수집기 설치 전에도 등록할 수
// 있다. 크래시 때 등록 역순으로 부른다. 수집기를 설치하지 않은 프로세스에서는 부르지 않는다.
ROCKS_PLATFORMBASE_API bool RegisterTerminationCallback(TerminationCallback callback, void* user,
                                                        TerminationCallbackHandle& handle) noexcept;

// 등록을 푼다. 그 콜백이 이미 크래시 처리로 불리고 있으면 돌아오지 않는다(프로세스가 곧 끝난다).
// 그래서 user가 가리키는 객체를 콜백 도중에 파괴하지 않는다. 이미 풀린 handle은 무시한다.
ROCKS_PLATFORMBASE_API void
UnregisterTerminationCallback(TerminationCallbackHandle& handle) noexcept;

} // namespace rocks::platform
