#include "../Common/CrashReportingInternal.h"

#include <Base/Compiler.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/CrashReporting.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#if ROCKS_ADDRESS_SANITIZER
// ASan runtime의 보고 뒤 콜백 등록이다(sanitizer/common_interface_defs.h와 같은 선언).
extern "C" void __sanitizer_set_death_callback(void (*callback)(void));
#endif

// POSIX 크래시 수집기다(Docs/Design/CrashHandlingAndLaunch.ko.md 8.7절). 치명 신호 처리기가
// 소유권을 얻고 크래시 처리 스레드를 pipe write로 깨운 뒤 완료 표시를 단조 시계로 확인한다. 끝나면
// 보관한 이전 동작을 되돌려 원래 신호를 OS 수집기(debuggerd·ReportCrash)나 기본 동작에 넘긴다.
// 덤프는 쓰지 않는다(Unsupported). 크래시 처리 스레드가 종료 콜백 뒤 CrashInfo.json만 쓴다(open·
// write·mkdir). 처리기 쪽은 async-signal-safe 호출(원자 연산, write, clock_gettime,
// sched_yield, sigaction, kill, pthread_kill, pause)만 쓴다. 깨우기는 macOS가 이름 없는
// 세마포어(sem_init)를 지원하지 않으므로 pipe로 한다. 크래시 처리 스레드 쪽은 최선 노력이다.
namespace rocks::platform {

namespace {

constexpr int CrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT, SIGTRAP};
constexpr usize CrashSignalCount = sizeof(CrashSignals) / sizeof(CrashSignals[0]);
constexpr usize SignalMessageBytes = 32;
constexpr uint64 CrashNanosecondsPerSecond = 1000000000;
constexpr usize AlternateStackAlignment = 16;
constexpr char WakeByte = 1;
constexpr usize PathBytes = 4096;
constexpr mode_t DirectoryMode = 0755;
constexpr mode_t FileMode = 0644;
constexpr char DefaultRoot[] = "Saved/Crashes";
constexpr char FallbackSubdirectory[] = "Rocks/Crashes";
constexpr char DefaultTemporaryRoot[] = "/tmp";
constexpr char MetadataFileName[] = "CrashInfo.json";
constexpr char MetadataTemporaryName[] = "CrashInfo.json.tmp";
constexpr usize ErrorTextBytes = 16;

// 크래시 폴더 경로다(설치·SetCrashOutputDirectory 때 정한다).
char primaryFolder[PathBytes];
char fallbackFolder[PathBytes];
// 크래시 처리 스레드가 쓰는 경로 칸이다.
char filePath[PathBytes];
char temporaryPath[PathBytes];
// 크래시 처리 스레드가 서식할 기록 사본이다.
detail::CrashRecordSnapshot reportRecord;

constinit uint32 stopping = 0;
constinit uint32 reportDone = 0;
constinit uint32 handlersInstalled = 0;
// 크래시 처리 스레드를 깨우는 self-pipe다. 이름 없는 POSIX 세마포어는 macOS가 지원하지 않아
// write(async-signal-safe)로 깨운다. 쓰는 쪽은 non-blocking이라 처리기가 막히지 않는다(origin
// 8749b85b의 LG18 처리기와 같은 방식).
constexpr int WakeReadEnd = 0;
constexpr int WakeWriteEnd = 1;
constexpr int NoDescriptor = -1;
constinit int wakePipe[2] = {NoDescriptor, NoDescriptor};
struct sigaction previousActions[CrashSignalCount];
pthread_t reporterThread;

// 스레드마다 처리기용 대체 stack이다. 이미 대체 stack이 있으면(ASan 등) 덮어쓰지 않는다.
thread_local void* alternateStack = nullptr;

usize TextLength(const char* text) noexcept {
    usize length = 0;
    while (text[length] != '\0') {
        ++length;
    }

    return length;
}

bool AppendPath(char* destination, usize& length, const char* source) noexcept {
    for (usize index = 0; source[index] != '\0'; ++index) {
        if (length + 1 >= PathBytes) {
            return false;
        }

        destination[length++] = source[index];
    }

    destination[length] = '\0';
    return true;
}

// root(상대면 지금 작업 폴더 기준)에 크래시 폴더 이름을 붙여 folder에 쓴다.
bool BuildFolder(const char* root, char* folder) noexcept {
    usize length = 0;
    folder[0] = '\0';
    if (root[0] != '/') {
        if (getcwd(folder, PathBytes) == nullptr) {
            folder[0] = '\0';
            return false;
        }

        length = TextLength(folder);
        if (!AppendPath(folder, length, "/")) {
            return false;
        }
    }

    if (!AppendPath(folder, length, root) || !AppendPath(folder, length, "/") ||
        !AppendPath(folder, length, detail::CrashFolderName())) {
        folder[0] = '\0';
        return false;
    }

    return true;
}

bool BuildPrimaryFolder(const char* directory) noexcept {
    char folder[PathBytes];
    if (!BuildFolder(directory != nullptr ? directory : DefaultRoot, folder)) {
        return false;
    }

    usize length = 0;
    return AppendPath(primaryFolder, length, folder);
}

void BuildFallbackFolder() noexcept {
    const char* temporary = getenv("TMPDIR");
    char root[PathBytes];
    usize length = 0;
    root[0] = '\0';
    if (!AppendPath(root, length,
                    temporary != nullptr && temporary[0] != '\0' ? temporary
                                                                 : DefaultTemporaryRoot) ||
        !AppendPath(root, length, "/") || !AppendPath(root, length, FallbackSubdirectory) ||
        !BuildFolder(root, fallbackFolder)) {
        fallbackFolder[0] = '\0';
    }
}

// path의 각 상위 폴더를 만든 뒤 path 자체를 만든다. 폴더로 있으면 0, 아니면 errno다.
int CreateDirectoryTree(char* path) noexcept {
    for (usize index = 1; path[index] != '\0'; ++index) {
        if (path[index] == '/' && path[index - 1] != '/') {
            path[index] = '\0';
            (void)mkdir(path, DirectoryMode);
            path[index] = '/';
        }
    }

    if (mkdir(path, DirectoryMode) != 0 && errno != EEXIST) {
        return errno;
    }

    struct stat status{};
    if (stat(path, &status) != 0) {
        return errno;
    }

    return S_ISDIR(status.st_mode) ? 0 : ENOTDIR;
}

bool BuildFilePath(char* destination, const char* folder, const char* name) noexcept {
    usize length = 0;
    destination[0] = '\0';
    return AppendPath(destination, length, folder) && AppendPath(destination, length, "/") &&
           AppendPath(destination, length, name);
}

int WriteWholeFile(const char* path, const char* bytes, usize count) noexcept {
    const int file = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, FileMode);
    if (file < 0) {
        return errno;
    }

    int error = 0;
    usize written = 0;
    while (written < count) {
        const ssize_t chunk = write(file, bytes + written, count - written);
        if (chunk < 0 && errno == EINTR) {
            continue;
        }

        if (chunk <= 0) {
            error = chunk < 0 ? errno : EIO;
            break;
        }

        written += static_cast<usize>(chunk);
    }

    (void)close(file);
    return error;
}

void WriteText(const char* text) noexcept {
    WriteDiagnostic(text, TextLength(text));
}

void WriteDirectoryFailure(int primaryError, int fallbackError) noexcept {
    char number[ErrorTextBytes];
    detail::WriteDiagnosticLine(
        "Rocks crash report directory unavailable; no crash files were written");
    WriteText("  primary: ");
    WriteText(primaryFolder);
    WriteText(" (errno ");
    (void)detail::FormatCodeMessage(number, sizeof(number), "", static_cast<uint32>(primaryError),
                                    false);
    WriteText(number);
    detail::WriteDiagnosticLine(")");
    WriteText("  fallback: ");
    WriteText(fallbackFolder);
    WriteText(" (errno ");
    (void)detail::FormatCodeMessage(number, sizeof(number), "", static_cast<uint32>(fallbackError),
                                    false);
    WriteText(number);
    detail::WriteDiagnosticLine(")");
}

// folder에 CrashInfo.json을 쓴다(임시 파일에 쓴 뒤 바꿔 넣는다). POSIX는 예비본이 없으므로 임시
// 파일을 쓰지 못하면 직접 쓴다. 바꿔 넣기가 실패하면 직접 쓰고 임시 파일을 지운다. 0 또는 errno다.
int WriteMetadataInto(const char* folder, const char* bytes, usize count) noexcept {
    if (!BuildFilePath(temporaryPath, folder, MetadataTemporaryName) ||
        !BuildFilePath(filePath, folder, MetadataFileName)) {
        return ENAMETOOLONG;
    }

    if (WriteWholeFile(temporaryPath, bytes, count) == 0 && rename(temporaryPath, filePath) == 0) {
        return 0;
    }

    const int error = WriteWholeFile(filePath, bytes, count);
    (void)unlink(temporaryPath);
    return error;
}

// 크래시 처리 스레드의 파일 단계다. 덤프는 이 플랫폼에서 쓰지 않는다(Unsupported, OS 수집기에
// 넘긴다). 신호 처리기 안(처리 스레드 자신의 실패)에서도 불리므로 async-signal-safe 호출만 쓴다.
// 출력 폴더의 폴더나 파일을 만들 수 없으면 임시 폴더로 한 번 옮긴다.
void WriteReportFiles() noexcept {
    detail::CrashMetadataInput input;
    input.hasCrashUtc = TryUtcTicks(input.crashUtcTicks);
    input.final = true;
    input.dump = {"Unsupported",
                  nullptr,
                  nullptr,
                  nullptr,
                  0,
                  "no minidump writer on this platform; the OS crash reporter receives the signal"};
    // 처리 스레드가 디스크에 직접 쓴다. 결과에 늘 남긴다.
    input.limitations[input.limitationCount++] = "in-process";
    if (detail::CrashOptions().embedded) {
        input.limitations[input.limitationCount++] = "embedded host";
    }

    // Asan 구성은 Windows와 같은 한계 문구를 남긴다(ASan이 보고를 맡고 Rocks는 콜백·메타데이터만).
    if constexpr (ROCKS_ADDRESS_SANITIZER) {
        input.limitations[input.limitationCount++] = "sanitizer build";
    }

    char* buffer = detail::CrashMetadataBuffer();
    detail::CaptureCrashRecord(reportRecord);
    int primaryError = CreateDirectoryTree(primaryFolder);
    if (primaryError == 0) {
        const usize bytes =
            detail::FormatCrashMetadata(buffer, detail::CrashMetadataBytes, reportRecord, input);
        primaryError = WriteMetadataInto(primaryFolder, buffer, bytes);
        if (primaryError == 0) {
            WriteText("Rocks crash report: ");
            detail::WriteDiagnosticLine(primaryFolder);
            return;
        }
    }

    int fallbackError = fallbackFolder[0] != '\0' ? CreateDirectoryTree(fallbackFolder) : ENOENT;
    if (fallbackError == 0) {
        input.fallbackDirectory = true;
        const usize bytes =
            detail::FormatCrashMetadata(buffer, detail::CrashMetadataBytes, reportRecord, input);
        fallbackError = WriteMetadataInto(fallbackFolder, buffer, bytes);
        if (fallbackError == 0) {
            WriteText("Rocks crash report: ");
            detail::WriteDiagnosticLine(fallbackFolder);
            return;
        }
    }

    WriteDirectoryFailure(primaryError, fallbackError);
}

uint64 SignalSafeNow() noexcept {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64>(now.tv_sec) * CrashNanosecondsPerSecond +
           static_cast<uint64>(now.tv_nsec);
}

uint32 TotalWaitMilliseconds() noexcept {
    const CrashReportingOptions& options = detail::CrashOptions();
    return options.callbackBudgetMilliseconds + options.metadataTimeoutMilliseconds +
           detail::ReportMarginMilliseconds;
}

[[noreturn]] void ParkInSignal() noexcept {
    for (;;) {
        pause();
    }
}

void WakeReporter() noexcept {
    // 신호로 끊기면(EINTR) 다시 쓴다. 그 밖의 실패(가득 참 EAGAIN — 이미 깨울 byte가 있다, 닫힘)는
    // 무시한다. 처리 스레드가 깨지 않아도 기다리는 쪽의 마감이 끝을 보장한다.
    while (write(wakePipe[WakeWriteEnd], &WakeByte, 1) < 0 && errno == EINTR) {
    }
}

// 크래시 처리 스레드 자신이 실패했다. 종료 콜백 단계였으면 남은 콜백만 포기하고 메타데이터를
// 쓴다. 기다리는 소유자에게 알리고 멈춘다.
[[noreturn]] void FailReporter() noexcept {
    const bool continueFiles = detail::TakeOverFilesAfterReporterFailure();
    detail::WriteDiagnosticLine(continueFiles ? "Rocks crash reporter thread failed; skipping the "
                                                "remaining termination callbacks"
                                              : "Rocks crash reporter thread failed while writing "
                                                "crash files");
    if (continueFiles) {
        WriteReportFiles();
    }

    AtomicStore<MemoryOrder::Release>(&reportDone, 1u);
    ParkInSignal();
}

void WaitForReport(uint64 deadline) noexcept {
    while (AtomicLoad<MemoryOrder::Acquire>(&reportDone) == 0 && SignalSafeNow() < deadline) {
        sched_yield();
    }
}

usize SignalIndex(int signal) noexcept {
    for (usize index = 0; index < CrashSignalCount; ++index) {
        if (CrashSignals[index] == signal) {
            return index;
        }
    }

    return CrashSignalCount;
}

void RestorePreviousAction(int signal) noexcept {
    const usize index = SignalIndex(signal);
    if (index != CrashSignalCount && AtomicLoad<MemoryOrder::Acquire>(&handlersInstalled) != 0) {
        (void)sigaction(signal, &previousActions[index], nullptr);
    }
}

// 다른 프로세스나 스레드가 보낸 신호(kill·tgkill·sigqueue·raise·abort)인가다. 하드웨어 fault는
// 커널이 양수 si_code(SEGV_MAPERR 등)를 준다. Linux·Android는 보낸 신호의 si_code가 0 이하다
// (SI_USER 0, SI_QUEUE -1, SI_TKILL -6 등). macOS(XNU bsd/sys/signal.h)는 SI_USER 0x10001부터
// SI_MESGQ 0x10005까지 양수 범위를 쓰므로 그 범위를 따로 비교한다. XNU가 kill에 0을 주는 경우도
// 있어 0 이하도 보낸 신호로 본다.
bool IsSentSignal(const siginfo_t* info) noexcept {
    if (info == nullptr || info->si_code <= 0) {
        return true;
    }

#if ROCKS_PLATFORM_MAC
    return info->si_code >= SI_USER && info->si_code <= SI_MESGQ;
#else
    return false;
#endif
}

// 보관한 이전 동작을 되돌리고 원래 신호를 넘긴다. 하드웨어 fault는 반환해 같은 명령이 다시
// fault하며 원래 siginfo로 전달되게 한다. 보낸 신호(abort의 SIGABRT 포함)와 SIGTRAP(x86 int3은 명령
// 뒤에서 멈추므로 반환하면 다시 나지 않는다)은 pthread_kill로 다시 보낸다.
void RestoreAndRedeliver(int signal, const siginfo_t* info) noexcept {
    RestorePreviousAction(signal);
    if (signal == SIGTRAP || IsSentSignal(info)) {
        (void)pthread_kill(pthread_self(), signal);
    }
}

// 소유자가 프로세스를 끝내는 신호다. Fatal 소유자는 SIGABRT다.
int OwnerSignal() noexcept {
    const uint32 code = detail::OwnerExitCode();
    return code == RocksFatalExceptionCode || code == 0 ? SIGABRT : static_cast<int>(code);
}

// 소유자: 크래시 처리 스레드를 깨우고 기다린다.
void ReportAsOwner(TerminationReason reason, uint64 self, const char* message, usize bytes,
                   const char* file, uint32 line, const void* returnAddress,
                   uint32 exitCode) noexcept {
    const uint64 deadline =
        SignalSafeNow() + uint64{TotalWaitMilliseconds()} * detail::NanosecondsPerMillisecond;
    detail::PublishOwnerTermination(exitCode, deadline);
    detail::RecordCrashOrigin(reason, self, message, bytes, file, line, returnAddress);
    WakeReporter();
    WaitForReport(deadline);
}

// 겹친 스레드: 스스로 끝내지 않고 소유자가 신호로 프로세스를 끝내기를 기다린다. 소유자의 마감과
// 여유가 지나도 끝나지 않았을 때만(소유자가 멈춘 경우) 소유자의 신호로 프로세스를 끝낸다.
[[noreturn]] void WaitForOwner(uint64 self) noexcept {
    detail::RecordStoppedThread(self);
    uint64 deadline = detail::OwnerDeadline();
    if (deadline == 0) {
        deadline =
            SignalSafeNow() + uint64{TotalWaitMilliseconds()} * detail::NanosecondsPerMillisecond;
    }

    const uint64 limit =
        deadline + uint64{detail::CrashOptions().concurrentCrashGraceMilliseconds} *
                       detail::NanosecondsPerMillisecond;
    while (SignalSafeNow() < limit) {
        sched_yield();
    }

    const int signal = OwnerSignal();
    RestorePreviousAction(signal);
    (void)kill(getpid(), signal);
    ParkInSignal();
}

// ASan 구성에서 하드웨어 fault를 ASan에 먼저 넘길 신호인가다. 설치 때 보관한 이전 동작이
// 처리기(SIG_DFL·SIG_IGN이 아님)면 넘긴다. 그 처리기가 ASan 것인지는 확인하지 않는다(ASan은
// 공개 API로 자기 처리기 주소를 알려 주지 않는다). 그래서 ASan 구성에서 Rocks보다 먼저 다른
// 라이브러리가 그 신호에 처리기를 걸었으면 그쪽에 넘기고 Rocks는 보고하지 않는다(설계 13.2절 한계).
// ASan은 보고한 뒤 death callback으로 공통 보고(reason Sanitizer)를 부른다. 이전 동작이 기본·무시인
// 신호와 SIGABRT·SIGTRAP은 Rocks가 보고한다.
bool DeferToSanitizer(int signal) noexcept {
    if constexpr (!ROCKS_ADDRESS_SANITIZER) {
        return false;
    }

    if (signal != SIGSEGV && signal != SIGBUS && signal != SIGILL && signal != SIGFPE) {
        return false;
    }

    const usize index = SignalIndex(signal);
    if (index == CrashSignalCount) {
        return false;
    }

    const struct sigaction& previous = previousActions[index];
    return (previous.sa_flags & SA_SIGINFO) != 0 ||
           (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN);
}

void CrashSignalHandler(int signal, siginfo_t* info, void*) noexcept {
    if (!detail::CrashStateInstalled()) {
        RestoreAndRedeliver(signal, info);
        return;
    }

    // 처리 스레드 자신의 실패는 ASan에 넘기기 전에 본다. ASan death callback(공통 Fatal 경로)이
    // 도는 동안 ASan은 보고 잠금을 쥐고 있으므로, 처리 스레드의 fault를 ASan에 넘기면 그 잠금에서
    // 멈추고 CrashInfo.json을 잃는다(LC7 리뷰).
    const uint64 self = CurrentThreadId();
    if (self == detail::ReporterThreadId()) {
        FailReporter();
    }

    if (DeferToSanitizer(signal)) {
        RestoreAndRedeliver(signal, info);
        return;
    }

    switch (detail::ClaimCrash(self)) {
    case detail::CrashClaim::Owner: {
        detail::RecordCrashException(detail::CrashCodeKind::Signal, static_cast<uint32>(signal),
                                     info != nullptr ? reinterpret_cast<uptr>(info->si_addr) : 0,
                                     detail::FaultAccess::None, 0);
        char message[SignalMessageBytes];
        const usize bytes = detail::FormatCodeMessage(message, sizeof(message), "Fatal signal ",
                                                      static_cast<uint32>(signal), false);
        ReportAsOwner(signal == SIGABRT ? TerminationReason::Abort : TerminationReason::Signal,
                      self, message, bytes, nullptr, 0, info != nullptr ? info->si_addr : nullptr,
                      static_cast<uint32>(signal));
        break;
    }
    case detail::CrashClaim::Concurrent:
        WaitForOwner(self);
    case detail::CrashClaim::Recursive:
        detail::WriteDiagnosticLine("Rocks crash handling failed again on the crashing thread");
        break;
    case detail::CrashClaim::Shutdown:
        break;
    }

    RestoreAndRedeliver(signal, info);
}

// 보관한 이전 SIGABRT 동작(debuggerd 등)을 되돌리고 abort한다.
[[noreturn]] void RestoreAbortAndAbort() noexcept {
    RestorePreviousAction(SIGABRT);
    abort();
}

[[noreturn]] void FailFatally(TerminationReason reason, const char* message, const char* file,
                              uint32 line, const void* returnAddress) noexcept;

#if ROCKS_ADDRESS_SANITIZER
// ASan이 오류 보고를 다 쓴 뒤 프로세스를 끝내기 직전에 부른다(설계 13.2절). 공통 Fatal 경로로 종료
// 콜백(로그 flush)과 메타데이터(reason Sanitizer)를 남기고 abort로 끝낸다. 반환하지 않는다.
ROCKS_NOINLINE void SanitizerDeathCallback() noexcept {
    FailFatally(TerminationReason::Sanitizer, "AddressSanitizer reported an error", nullptr, 0,
                ROCKS_RETURN_ADDRESS());
}
#endif

[[noreturn]] void FailFatally(TerminationReason reason, const char* message, const char* file,
                              uint32 line, const void* returnAddress) noexcept {
    const char* text = message != nullptr ? message : "Rocks fatal error";
    usize length = 0;
    while (length < detail::MaxFatalDiagnosticBytes && text[length] != '\0') {
        ++length;
    }

    WriteDiagnostic(text, length);
    WriteDiagnostic("\n", 1);
    if (!detail::CrashStateInstalled()) {
        abort();
    }

    const uint64 self = CurrentThreadId();
    if (self == detail::ReporterThreadId()) {
        FailReporter();
    }

    switch (detail::ClaimCrash(self)) {
    case detail::CrashClaim::Owner:
        ReportAsOwner(reason, self, text, length, file, line, returnAddress,
                      RocksFatalExceptionCode);
        break;
    case detail::CrashClaim::Concurrent:
        WaitForOwner(self);
    case detail::CrashClaim::Recursive:
        detail::WriteDiagnosticLine("Rocks fatal error while reporting a crash on this thread");
        break;
    case detail::CrashClaim::Shutdown:
        break;
    }

    RestoreAbortAndAbort();
}

// 보호 블록이 없으므로 인라인돼도 되지만, Windows와 같은 모양을 지킨다.
ROCKS_NOINLINE void ReporterLoop() noexcept {
    PrepareThreadForCrashHandling();
    detail::SetReporterThreadId(CurrentThreadId());
    char byte = 0;
    for (;;) {
        const ssize_t received = read(wakePipe[WakeReadEnd], &byte, 1);
        if (received == 1 || (received < 0 && errno != EINTR) || received == 0) {
            break;
        }
    }

    if (AtomicLoad<MemoryOrder::Acquire>(&stopping) == 0) {
        detail::RunTerminationCallbacks(MonotonicNanoseconds() +
                                        uint64{detail::CrashOptions().callbackBudgetMilliseconds} *
                                            detail::NanosecondsPerMillisecond);
        WriteReportFiles();
        detail::MarkReportCompleted();
        AtomicStore<MemoryOrder::Release>(&reportDone, 1u);
    }
}

void* ReporterMain(void*) noexcept {
    ReporterLoop();
    return nullptr;
}

void CloseWakePipe() noexcept {
    for (int& descriptor : wakePipe) {
        if (descriptor != NoDescriptor) {
            (void)close(descriptor);
            descriptor = NoDescriptor;
        }
    }
}

// pipe를 만들고 양쪽에 FD_CLOEXEC, 쓰는 쪽에 O_NONBLOCK을 건다. pipe2는 macOS에 없어 fcntl로 건다.
// 하나라도 실패하면 닫고 false다.
bool OpenWakePipe() noexcept {
    if (pipe(wakePipe) != 0) {
        wakePipe[WakeReadEnd] = NoDescriptor;
        wakePipe[WakeWriteEnd] = NoDescriptor;
        return false;
    }

    for (const int descriptor : wakePipe) {
        if (fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
            CloseWakePipe();
            return false;
        }
    }

    const int flags = fcntl(wakePipe[WakeWriteEnd], F_GETFL);
    if (flags < 0 || fcntl(wakePipe[WakeWriteEnd], F_SETFL, flags | O_NONBLOCK) < 0) {
        CloseWakePipe();
        return false;
    }

    return true;
}

} // namespace

CrashCapability QueryCrashCapabilities() noexcept {
    return CrashCapability::OsFaultHandling | CrashCapability::TerminationCallbacks |
           CrashCapability::SystemCrashReport | CrashCapability::Metadata;
}

CrashReportingStatus InstallCrashReporting(const CrashReportingOptions& options) noexcept {
    if (detail::CrashStateInstalled()) {
        return CrashReportingStatus::AlreadyInstalled;
    }

    if (!detail::ValidateCrashOptions(options) || !detail::PrepareCrashIdentity(options) ||
        !BuildPrimaryFolder(options.outputDirectory)) {
        return CrashReportingStatus::InvalidArgument;
    }

    BuildFallbackFolder();
    detail::ResetCrashState(options);
    AtomicStore<MemoryOrder::Relaxed>(&stopping, 0u);
    AtomicStore<MemoryOrder::Relaxed>(&reportDone, 0u);
    if (!OpenWakePipe()) {
        return CrashReportingStatus::OutOfResources;
    }

    if (pthread_create(&reporterThread, nullptr, &ReporterMain, nullptr) != 0) {
        CloseWakePipe();
        return CrashReportingStatus::OutOfResources;
    }

    PrepareThreadForCrashHandling();
    if (!options.embedded) {
        struct sigaction action{};
        action.sa_sigaction = &CrashSignalHandler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (usize index = 0; index < CrashSignalCount; ++index) {
            (void)sigaction(CrashSignals[index], &action, &previousActions[index]);
        }

        AtomicStore<MemoryOrder::Release>(&handlersInstalled, 1u);
#if ROCKS_ADDRESS_SANITIZER
        // 프로세스 전역 칸이다. 설치 때 덮어쓰고 해제 때 비운다(설계 13.2절).
        __sanitizer_set_death_callback(&SanitizerDeathCallback);
#endif
    }

    detail::SetCrashStateInstalled(true);
    return CrashReportingStatus::Installed;
}

void RemoveCrashReporting() noexcept {
    if (!detail::CrashStateInstalled()) {
        return;
    }

    if (!detail::BlockCrashOwnerForShutdown() && !detail::ReportCompleted()) {
        detail::ParkCurrentThread();
    }

    if (AtomicLoad<MemoryOrder::Acquire>(&handlersInstalled) != 0) {
#if ROCKS_ADDRESS_SANITIZER
        __sanitizer_set_death_callback(nullptr);
#endif
        for (usize index = 0; index < CrashSignalCount; ++index) {
            (void)sigaction(CrashSignals[index], &previousActions[index], nullptr);
        }

        AtomicStore<MemoryOrder::Release>(&handlersInstalled, 0u);
    }

    detail::SetCrashStateInstalled(false);
    AtomicStore<MemoryOrder::Release>(&stopping, 1u);
    WakeReporter();
    (void)pthread_join(reporterThread, nullptr);
    CloseWakePipe();
    detail::SetReporterThreadId(0);
    detail::ReleaseCrashOwnerAfterShutdown();
}

bool CheckUnhandledCrashFilter() noexcept {
    // POSIX에는 프로세스 전역 unhandled filter가 없다(신호 처리기는 LC7에서 본다).
    return true;
}

bool SetCrashOutputDirectory(const char* directory) noexcept {
    return detail::CrashStateInstalled() && detail::IsValidCrashDirectoryText(directory) &&
           BuildPrimaryFolder(directory);
}

usize CopyCrashReportFolder(char* buffer, usize capacity) noexcept {
    if (buffer == nullptr || capacity == 0 || !detail::CrashStateInstalled()) {
        return 0;
    }

    const char* folder = primaryFolder;
    usize length = 0;
    while (folder[length] != '\0') {
        ++length;
    }

    if (length + 1 > capacity) {
        buffer[0] = '\0';
        return 0;
    }

    for (usize index = 0; index <= length; ++index) {
        buffer[index] = folder[index];
    }

    return length;
}

uint32 CrashHelperProcessId() noexcept {
    // POSIX에는 덤프 도우미가 없다(덤프는 Unsupported, OS 수집기에 넘긴다).
    return 0;
}

bool RunCrashHelperIfRequested(int, char**, int&) noexcept {
    return false;
}

bool RunCrashHelperIfRequested(int, wchar_t**, int&) noexcept {
    return false;
}

CrashHelperStart StartCrashHelper() noexcept {
    // POSIX에는 덤프 도우미가 없다(Deferred는 InProcess와 같다).
    return CrashHelperStart::NotApplicable;
}

void SuppressSystemErrorDialogs() noexcept {
    // macOS·Android의 abort·assert는 대화상자를 띄우지 않는다. 시스템 crash reporter는 시스템
    // 설정을 따르며 바꾸지 않는다.
}

namespace detail {

const char* CrashModuleJson() noexcept {
    return nullptr;
}

const char* CrashPlatformName() noexcept {
#if ROCKS_PLATFORM_MAC
    return "MacOS";
#else
    return "Android";
#endif
}

bool ProcessStartUtcTicks(int64& ticks) noexcept {
    // 설치 시각으로 대신한다(설치는 시작 단계에서 한다).
    return TryUtcTicks(ticks);
}

} // namespace detail

int RunWithCrashGuard(GuardedBody body, void* context) noexcept {
    return body(context);
}

void PrepareThreadForCrashHandling() noexcept {
    if (alternateStack != nullptr) {
        return;
    }

    stack_t current{};
    if (sigaltstack(nullptr, &current) == 0 && (current.ss_flags & SS_DISABLE) == 0) {
        return;
    }

    void* memory = AlignedAllocate(CrashStackReserveBytes, AlternateStackAlignment);
    if (memory == nullptr) {
        return;
    }

    stack_t stack{};
    stack.ss_sp = memory;
    stack.ss_size = CrashStackReserveBytes;
    stack.ss_flags = 0;
    if (sigaltstack(&stack, nullptr) != 0) {
        AlignedFree(memory);
        return;
    }

    alternateStack = memory;
}

void ReleaseThreadCrashHandling() noexcept {
    if (alternateStack == nullptr) {
        return;
    }

    stack_t disable{};
    disable.ss_flags = SS_DISABLE;
    (void)sigaltstack(&disable, nullptr);
    AlignedFree(alternateStack);
    alternateStack = nullptr;
}

ROCKS_NOINLINE void Fatal(const char* message) noexcept {
    FailFatally(TerminationReason::Fatal, message, nullptr, 0, ROCKS_RETURN_ADDRESS());
}

ROCKS_NOINLINE void FatalAt(const char* message, const char* file, uint32 line) noexcept {
    FailFatally(TerminationReason::Assert, message, file, line, ROCKS_RETURN_ADDRESS());
}

} // namespace rocks::platform
