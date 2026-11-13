#pragma once

#include <Platform/File.h>

namespace rocks::platform {

// 자식 프로세스 표준 스트림 하나의 연결 방식이다.
enum class ProcessStreamMode : uint8 {
    // 입력은 빈 스트림, 출력은 버린다(Windows NUL, POSIX /dev/null).
    Null,
    // 부모의 표준 스트림을 그대로 물려준다. 부모에 해당 스트림이 없으면 Null과 같다. macOS에서
    // 입력을 터미널에서 물려받으면 자식이 백그라운드 그룹이라 읽는 순간 SIGTTIN으로 멈추므로,
    // 입력은 Null이나 Pipe를 쓴다.
    Inherit,
    // 부모와 자식 사이에 pipe를 만든다. 부모는 Process::WriteInput·Read로 쓰고 읽는다.
    Pipe
};

// 부모가 읽는 자식 출력 스트림이다.
enum class ProcessStream : uint8 {
    Output,
    Error
};

// 자식 프로세스의 OS 우선순위다. 셰이더 컴파일 워커처럼 오래 도는 배경 작업은 Low를 쓴다(UE
// ShaderCompileWorker의 PriorityModifier -1과 같은 값). Windows는 BELOW_NORMAL_PRIORITY_CLASS로
// 시작한다. macOS는 posix_spawn이 nice를 정할 수 없어 시작 직후 nice +5로 바꾼다(그 사이 자식이
// 만든 프로세스에는 적용되지 않고, 바꾸지 못해도 실행은 계속한다).
enum class ProcessPriority : uint8 {
    Normal,
    Low
};

// 자식에게 넘기는 인자 하나다(UTF-8, 길이 지정, NUL을 포함하면 안 된다). 빈 인자도 그대로 전달된다.
struct ProcessArgument {
    const char* text = "";
    usize bytes = 0;
};

// Process::Start·RunProcess의 입력이다. 포인터가 가리키는 문자열·배열은 호출 동안만 읽고 보관하지
// 않는다.
struct ProcessStartInfo {
    // 실행 파일 경로(UTF-8). PATH 검색을 하지 않는다. 상대 경로는 부모의 현재 디렉터리 기준이다
    // (workingDirectory 기준이 아니다). 자식의 argv[0]은 이 문자열 그대로다. Windows는 확장자를
    // 포함한 이름이어야 하고 '"'를 쓸 수 없으며, batch 파일(.bat·.cmd)은 cmd.exe가 인자를 다시
    // 해석하므로(인자 주입) 거부한다.
    const char* executable = nullptr;
    usize executableBytes = 0;
    // argv[1..]이다. 자식은 인자를 받은 그대로 본다(Windows는 MSVC CRT·CommandLineToArgvW 규칙으로
    // 인용해 넘긴다).
    const ProcessArgument* arguments = nullptr;
    usize argumentCount = 0;
    // 자식의 현재 디렉터리(UTF-8)다. null이거나 빈 값이면 부모의 현재 디렉터리를 물려준다.
    // Windows는 CreateProcessW 제약으로 MAX_PATH(260) 미만이어야 한다.
    const char* workingDirectory = nullptr;
    usize workingDirectoryBytes = 0;
    ProcessStreamMode input = ProcessStreamMode::Null;
    ProcessStreamMode output = ProcessStreamMode::Inherit;
    ProcessStreamMode error = ProcessStreamMode::Inherit;
    // true면 자식의 표준 오류를 표준 출력과 같은 곳으로 보낸다(error는 무시하고 Read(Error)는
    // NotOpen이다). 도구의 진단 출력을 순서대로 한 버퍼에 모을 때 쓴다.
    bool mergeErrorIntoOutput = false;
    ProcessPriority priority = ProcessPriority::Normal;
};

// 관측한 종료의 종류다.
enum class ProcessExitKind : uint8 {
    // 스스로 끝났다. code는 종료 코드다(Windows는 DWORD 그대로, 예외 종료면 0xC0000005 같은
    // NTSTATUS. POSIX는 0–255).
    Exited,
    // POSIX 신호로 끝났다. code는 신호 번호다. Windows에서는 나오지 않는다.
    Signaled,
    // 부모의 Terminate(RunProcess 시간 초과·Close 포함)로 끝났다. Terminate는 먼저 자식이 이미
    // 끝났는지 확인해 그렇다면 Exited·Signaled로 둔다. 확인과 종료 사이에 스스로 끝난 경우는
    // Terminated다. code는 OS가 보고한 값(Windows 종료 코드, POSIX 신호 번호 또는 종료 코드)이다.
    Terminated
};

struct ProcessExit {
    ProcessExitKind kind = ProcessExitKind::Exited;
    uint32 code = 0;
};

struct ProcessRunOptions;
struct ProcessRunResult;

// 자식 프로세스 하나와 그 자손의 소유권이다(UE FGenericPlatformProcess의 CreateProc·
// IsProcRunning·WaitForProc·TerminateProc·GetProcReturnCode·CreatePipe·ReadPipe·WritePipe 대응).
//
// 사용 예(워커 실행):
//   const platform::ProcessArgument arguments[] = {{"--input", 7}, {path, pathBytes}};
//   platform::ProcessStartInfo info;
//   info.executable = workerPath;
//   info.executableBytes = workerPathBytes;
//   info.arguments = arguments;
//   info.argumentCount = 2;
//   info.input = platform::ProcessStreamMode::Pipe;   // 부모 생존 신호(lifeline)로 쓴다
//   info.priority = platform::ProcessPriority::Low;
//   platform::Process worker;
//   if (auto started = worker.Start(info); !started) { ... }
//   if (worker.Wait(0) == platform::WaitResult::Signaled) {   // 끝났는지 기다리지 않고 확인
//       platform::ProcessExit exit;
//       worker.GetExit(exit);
//   }
//   worker.Close();   // 아직 돌고 있으면 자손까지 종료하고 회수한다
//
// 수명 계약:
// - 자식과 그 자손은 이 객체가 소유한다. Close 또는 소멸은 살아 있는 자식과 자손을 강제 종료하고
//   회수한다. 직접 자식이 스스로 끝난 뒤에도 남은 자손은 끝낸다. 그래서 자손으로 공유 서버를 띄우는
//   도구(MSVC cl의 mspdbsrv, sccache 서버, 빌드 데몬)는 매 실행마다 서버를 잃고, 같은 서버를 쓰던
//   다른 실행이 실패할 수 있다. 분리 실행(detach)은 후속이다.
// - Close는 강제 종료한 자식이 끝나기를 최대 10초 기다린다. 커널 대기나 종료 중 교착으로 끝나지
//   않으면 handle만 놓는다(macOS는 회수하지 못한 zombie가 남는다).
// - Windows: 자식을 일시 정지 상태로 만들어 이 객체 전용 job object(KILL_ON_JOB_CLOSE,
//   DIE_ON_UNHANDLED_EXCEPTION, 탈출 불가)에 넣은 뒤 실행한다. 자손도 같은 job에 속한다. 부모가
//   비정상 종료해도 job handle이 닫히면서 자식·자손이 모두 끝난다. 처리되지 않은 예외는 오류
//   대화상자 없이 자식을 끝낸다. 서비스·COM·WMI·작업 스케줄러가 대신 만든 프로세스는 job 밖이다.
// - POSIX(macOS): 자식이 새 프로세스 그룹의 리더가 된다. 종료는 그룹 전체에 SIGKILL이다. 그룹을
//   벗어난 자손(setsid·setpgid)은 따라가지 못하고, 자식이 자기 Process로 만든 손자도 자기 그룹을
//   가지므로 자식이 SIGKILL로 끝나면 남는다. 부모가 비정상 종료하면 자식은 남는다. 자식의 종료는
//   회수하지 않고 관측만 하다가(WNOWAIT) Close에서 그룹을 끝낸 뒤 회수한다. 회수 전에는 PID·그룹
//   번호가 다른 프로세스에 재사용되지 않으므로 다른 그룹을 죽이지 않는다. 이 보장의 전제는 같은
//   프로세스의 다른 코드가 자식을 회수하지 않는 것이다(SIGCHLD를 SIG_IGN으로 두거나 waitpid(-1)을
//   부르면 안 된다). 다른 코드가 회수했으면 종료 코드는 0xFFFFFFFF이고 이후 신호를 보내지 않는다.
// - 부모 생존 감시: 오래 도는 워커는 입력을 Pipe로 받아 EOF를 부모 사망 신호로 쓴다(부모가 어떻게
//   끝나도 OS가 pipe를 닫는다). IsProcessRunning(부모 PID)는 PID 재사용 때문에 보조 수단이다.
// - 자식은 콘솔 창을 띄우지 않는다(Windows CREATE_NO_WINDOW). 자식은 이 객체가 만든 handle·fd만
//   물려받는다(Windows 상속 handle 목록 제한, macOS POSIX_SPAWN_CLOEXEC_DEFAULT). 다만 같은 시각
//   다른 스레드가 handle 목록 없이 자식을 만들면(CreateProcess bInheritHandles, fork) 그 자식이 이
//   객체의 pipe 끝을 물려받을 수 있다. 그러면 pipe의 EOF가 늦어진다.
// - 환경 변수는 부모 것을 그대로 물려준다. 자식의 오류 대화상자 억제(abort·assert)는 자식
//   프로그램의 책임이며, Windows 자식은 부모의 오류 모드(SetErrorMode)를 물려받는다.
//
// 스레드: 한 객체의 메서드는 한 번에 한 스레드에서만 호출한다(다른 스레드에서 Wait·Read를 깨우는
// 취소는 후속이다). 서로 다른 Process 객체는 동시에 써도 된다.
//
// 후속: 환경 변수 지정, 분리 실행, 프로세스 정체((PID, 시작 시각) 또는 handle), 권한 상승
// 조회·실행, 다른 스레드의 취소, 정상 종료 요청(SIGTERM·CTRL_BREAK), 숨김 GUI 실행, PATH 검색.
// Android는 아직 지원하지 않는다(Start·RunProcess가 Unsupported).
class Process final {
public:
    Process() noexcept = default;
    ~Process() noexcept;
    Process(Process&& other) noexcept;
    Process& operator=(Process&& other) noexcept;
    Process(const Process&) = delete;
    Process& operator=(const Process&) = delete;

    // 자식을 시작한다. 이미 시작했으면(Close 전) AlreadyOpen이다. 실패는 다음과 같다.
    // - InvalidArgument: 실행 파일이 비었거나, 문자열에 NUL·잘못된 UTF-8이 있거나, 포인터·개수·열거
    //   값이 맞지 않거나, Windows 실행 파일 경로에 '"'가 있다.
    // - Unsupported: Windows batch 파일(.bat·.cmd), Android.
    // - Overflow: 명령줄이 OS 한도를 넘는다(Windows 32767 UTF-16 단위, POSIX E2BIG).
    // - NotFound·AccessDenied: 실행 파일이나 작업 디렉터리를 찾거나 실행할 수 없다.
    // - WrongType: 작업 디렉터리가 디렉터리가 아니다.
    // - OutOfMemory, 그 밖은 OS 원인 코드다.
    // 실패하면 시작 전 상태로 남고 만든 pipe·job은 모두 정리한다.
    FileStatus Start(const ProcessStartInfo& info) noexcept;

    bool IsStarted() const noexcept {
        return state_ != nullptr;
    }

    // 자식의 OS 프로세스 ID다. 시작 전이면 0이다. 자식이 끝나도 Close 전까지 같은 값이다.
    uint32 Id() const noexcept;

    // 자식(직접 자식)이 끝날 때까지 최대 timeoutMilliseconds 기다린다. 끝났으면 Signaled이고 이후
    // GetExit로 결과를 읽는다. 이미 끝난 것을 관측했으면 바로 Signaled다. 0은 기다리지 않고 확인만
    // 한다. 시작 전이거나 OS 오류면 Failed다. 자손의 종료는 기다리지 않는다.
    WaitResult Wait(uint32 timeoutMilliseconds = InfiniteTimeout) noexcept;

    // Wait가 끝남을 관측했으면 true와 결과를 준다. 아직이면 false다(exit는 바꾸지 않는다).
    bool GetExit(ProcessExit& exit) const noexcept;

    // 자식과 자손을 강제 종료한다(기다리지 않는다. 결과는 Wait로 확인한다). 직접 자식이 이미
    // 끝났으면 종료 종류는 그대로 두고 남은 자손만 끝낸다. 이미 요청했으면 Success다. 실패하면
    // 요청을 기록하지 않으므로 다시 시도할 수 있다. 시작 전이면 NotOpen이다.
    FileStatus Terminate() noexcept;

    // 입력 pipe에 data를 쓴다. 다 쓰거나 시간이 다할 때까지 기다린다. bytes는 실제로 쓴 바이트
    // 수다. 시간이 다하면 NoProgress(쓴 만큼은 이미 전달됨)다. 입력이 Pipe가 아니거나 CloseInput
    // 뒤면 NotOpen, 자식이 입력을 닫았으면 EndOfFile이다.
    FileTransfer WriteInput(const void* data, usize bytes,
                            uint32 timeoutMilliseconds = InfiniteTimeout) noexcept;

    // 입력 pipe를 닫는다. 자식은 EOF를 본다. 입력이 Pipe가 아니거나 이미 닫았으면 NotOpen이다.
    FileStatus CloseInput() noexcept;

    // stream의 pipe에서 최대 capacity 바이트를 읽는다. 데이터가 오거나 시간이 다할 때까지 기다린다
    // (자식의 0 byte 쓰기는 기다림을 끝내지 않는다). 데이터가 있으면 Success와 1바이트 이상이다.
    // 시간이 다하면 NoProgress와 0이다. 모든 쓰는 쪽이 닫혔으면 EndOfFile과 0이다. 해당 스트림이
    // Pipe가 아니거나 출력에 합쳐졌으면 NotOpen이다. capacity가 0이면 InvalidArgument다.
    // 주의: 출력과 오류를 모두 Pipe로 두고 한쪽만 오래 읽으면 다른 쪽 pipe가 차서 자식이 멈출 수
    // 있다. 둘 다 모아야 하면 RunProcess를 쓰거나 짧은 timeout으로 번갈아 읽는다.
    FileTransfer Read(ProcessStream stream, void* buffer, usize capacity,
                      uint32 timeoutMilliseconds = InfiniteTimeout) noexcept;

    // 아직 살아 있는 자식·자손을 강제 종료하고 회수한 뒤 모든 handle·pipe를 닫는다(수명 계약 참고).
    // 이후 다시 Start할 수 있다. 여러 번 호출해도 안전하다.
    void Close() noexcept;

    // 플랫폼 구현(Private/<플랫폼>/Process.cpp)만 정의하는 상태다. 공개 API가 아니며 이 헤더에서는
    // 불완전 타입이다.
    struct State;

private:
    friend ProcessRunResult RunProcess(const ProcessStartInfo& info,
                                       const ProcessRunOptions& options) noexcept;

    State* state_ = nullptr;
};

// RunProcess의 입력 데이터와 출력 버퍼다. 포인터는 호출 동안만 쓴다.
struct ProcessRunOptions {
    // info.input이 Pipe일 때 자식에 쓸 데이터다. 다 쓰면 입력을 닫는다. Pipe가 아닌데 데이터를 주면
    // InvalidArgument다.
    const void* input = nullptr;
    usize inputBytes = 0;
    // info.output·info.error가 Pipe일 때 모을 버퍼다. 용량을 넘는 나머지는 읽어서 버린다(자식이
    // 멈추지 않게 끝까지 읽는다). 용량 0이면 모두 버린다. Pipe가 아니거나 오류를 출력에 합친
    // 스트림에 버퍼를 주면 InvalidArgument다.
    void* output = nullptr;
    usize outputCapacity = 0;
    void* error = nullptr;
    usize errorCapacity = 0;
    // 자식 시작부터 잰 제한 시간이다. 넘으면 자식·자손을 강제 종료하고 timedOut을 켠다.
    uint32 timeoutMilliseconds = InfiniteTimeout;
};

struct [[nodiscard]] ProcessRunResult {
    // Start 실패나 pipe I/O의 OS 오류다. Success이면 exit가 유효하다(시간 초과도 Success다).
    FileStatus status;
    ProcessExit exit;
    bool timedOut = false;
    usize outputBytes = 0;
    // 용량을 넘었거나, 남은 출력을 상한 안에 다 모으지 못했다.
    bool outputTruncated = false;
    usize errorBytes = 0;
    bool errorTruncated = false;
};

// 자식을 실행해 입력을 넣고 출력·오류를 동시에 모으며 끝날 때까지 기다린다(UE ExecProcess 대응,
// 외부 도구 실행용). 한 스레드에서 pipe 여러 개를 함께 기다리므로 출력이 커도 막히지 않는다. 직접
// 자식이 끝나거나 시간이 다해 강제 종료하면 남은 자손을 끝내고, 남은 출력은 최대 10초 더 모은다
// (job·그룹 밖의 프로세스가 pipe 끝을 잡고 있으면 그 뒤를 버리고 truncated를 켠다). 그래서 전체
// 시간은 timeoutMilliseconds + 10초를 넘지 않는다.
// 사용 예:
//   char output[64 * 1024];
//   platform::ProcessRunOptions options;
//   options.output = output;
//   options.outputCapacity = sizeof(output);
//   options.timeoutMilliseconds = 60'000;
//   info.output = platform::ProcessStreamMode::Pipe;
//   info.mergeErrorIntoOutput = true;
//   auto run = platform::RunProcess(info, options);
//   if (run.status && !run.timedOut && run.exit.kind == platform::ProcessExitKind::Exited &&
//       run.exit.code == 0) { ... }
ProcessRunResult RunProcess(const ProcessStartInfo& info,
                            const ProcessRunOptions& options) noexcept;

// 현재 프로세스의 OS 프로세스 ID다.
uint32 CurrentProcessId() noexcept;

// processId의 프로세스가 아직 있는지 확인한다(UE IsApplicationRunning 대응). 권한이 없어 확인할 수
// 없는 프로세스도 있다고 본다. 끝난 프로세스의 의미는 플랫폼마다 다르다: Windows는 handle이 열려
// 있어도 끝났으면 false, POSIX는 끝났지만 회수되지 않은(zombie) 프로세스도 true다. PID는 재사용될
// 수 있으므로 오래 끝난 프로세스의 PID에 대해서는 다른 프로세스를 보고 true일 수 있다(부모 감시는
// 수명 계약의 lifeline을 먼저 쓴다). 0은 false다.
bool IsProcessRunning(uint32 processId) noexcept;

// 현재 실행 파일의 절대 경로를 UTF-8로 buffer에 쓰고 길이를 bytes에 둔다(NUL 포함하지 않음,
// buffer에는 NUL도 쓴다). macOS·Android는 link를 푼 경로이고 Windows는 로더가 쓴 경로 그대로다.
// buffer가 부족하면 Overflow이며 bytes에 필요한 길이(NUL 제외)를 둔다. buffer가 null인데 capacity가
// 0이 아니면 InvalidArgument다. 그 밖은 OS 원인 코드다.
FileStatus GetExecutablePath(char* buffer, usize capacity, usize& bytes) noexcept;

} // namespace rocks::platform
