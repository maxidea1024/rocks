#include <TestSupport/HostFailure.h>
#include <Base/Compiler.h>
#include <Platform/CrashReporting.h>
#include <PlatformBase/Private/Win64/CrashHelper.h>
#include <Windows.h>
#include <io.h>
#include <stdio.h>
#include <string.h>

namespace rocks::test {

FILE* OpenReportFile(const char* path) noexcept {
    if (path == nullptr) {
        return nullptr;
    }
    FILE* file = nullptr;
    return fopen_s(&file, path, "wb") == 0 ? file : nullptr;
}

void PrepareFatalTestProcess() noexcept {
    // abort·assert·처리되지 않은 예외가 사람 입력을 기다리는 대화상자를 띄우지 않게 한다(CRT abort
    // 메시지·WER 보고 끄기, Debug CRT 보고를 stderr로, 오류 모드, WER UI 끄기).
    platform::SuppressSystemErrorDialogs();
}

void LockStandardErrorForTest() noexcept {
    _lock_file(stderr);
}

void UnlockStandardErrorForTest() noexcept {
    _unlock_file(stderr);
}

} // namespace rocks::test

namespace rocks::test {

namespace {

struct UnmanagedStart {
    void (*entry)(void*) noexcept;
    void* context;
};

DWORD WINAPI UnmanagedThreadMain(void* raw) noexcept {
    const UnmanagedStart start = *static_cast<UnmanagedStart*>(raw);
    HeapFree(GetProcessHeap(), 0, raw);
    start.entry(start.context);
    return 0;
}

} // namespace

bool StartUnmanagedThread(void (*entry)(void* context) noexcept, void* context) noexcept {
    auto* start =
        static_cast<UnmanagedStart*>(HeapAlloc(GetProcessHeap(), 0, sizeof(UnmanagedStart)));
    if (start == nullptr) {
        return false;
    }

    start->entry = entry;
    start->context = context;
    const HANDLE thread = CreateThread(nullptr, 0, &UnmanagedThreadMain, start, 0, nullptr);
    if (thread == nullptr) {
        HeapFree(GetProcessHeap(), 0, start);
        return false;
    }

    CloseHandle(thread);
    return true;
}

} // namespace rocks::test

namespace rocks::test {

namespace {

// 기반 클래스 생성 중 가상 호출은 그 클래스의 순수 가상 칸(_purecall)으로 간다.
class PureBase {
public:
    PureBase() noexcept;
    virtual ~PureBase() = default;
    virtual void Run() noexcept = 0;
};

// cdb stack 확인의 기대 프레임 이름이다(C# 크래시 시험이 같은 이름을 찾는다).
ROCKS_NOINLINE void CallPureVirtualFrom(PureBase* self) noexcept {
    // 컴파일러가 생성 중 동적 타입을 알아 호출을 바꾸지 않게 volatile 칸을 거친다.
    PureBase* volatile target = self;
    target->Run();
}

PureBase::PureBase() noexcept {
    CallPureVirtualFrom(this);
}

class PureDerived final : public PureBase {
public:
    void Run() noexcept override {
    }
};

} // namespace

ROCKS_NOINLINE bool PassInvalidParameter() noexcept {
    // UCRT _close는 범위 밖 파일 번호를 invalid parameter 처리기로 보낸다.
    int volatile descriptor = -1;
    (void)_close(descriptor);
    return true;
}

ROCKS_NOINLINE bool RaiseUserException() noexcept {
    constexpr DWORD UserExceptionCode = 0xE0001234;
    RaiseException(UserExceptionCode, 0, 0, nullptr);
    return true;
}

ROCKS_NOINLINE bool CallPureVirtual() noexcept {
    PureDerived derived;
    derived.Run();
    return true;
}

namespace {

// heap의 블록 머리를 덮고 두 번 해제한다(힙 손상 검출이 STATUS_HEAP_CORRUPTION을 낸다).
bool CorruptBlockOf(HANDLE heap) noexcept {
    constexpr usize BlockBytes = 64;
    constexpr usize HeaderBytes = 16;
    constexpr unsigned char Garbage = 0xCD;
    auto* block = static_cast<unsigned char*>(HeapAlloc(heap, 0, BlockBytes));
    if (block == nullptr) {
        return true;
    }

    unsigned char* volatile header = block - HeaderBytes;
    for (usize index = 0; index < HeaderBytes; ++index) {
        header[index] = Garbage;
    }

    (void)HeapFree(heap, 0, block);
    (void)HeapFree(heap, 0, block);
    return true;
}

} // namespace

ROCKS_NOINLINE bool CorruptProcessHeap() noexcept {
    return CorruptBlockOf(GetProcessHeap());
}

ROCKS_NOINLINE bool CorruptHeap() noexcept {
    constexpr usize BlockBytes = 64;
    constexpr usize HeaderBytes = 16;
    constexpr unsigned char Garbage = 0xCD;
    const HANDLE heap = HeapCreate(0, 0, 0);
    if (heap == nullptr) {
        return true;
    }

    auto* block = static_cast<unsigned char*>(HeapAlloc(heap, 0, BlockBytes));
    if (block == nullptr) {
        return true;
    }

    unsigned char* volatile header = block - HeaderBytes;
    for (usize index = 0; index < HeaderBytes; ++index) {
        header[index] = Garbage;
    }

    (void)HeapFree(heap, 0, block);
    (void)HeapFree(heap, 0, block);
    return true;
}

} // namespace rocks::test

namespace rocks::test {

namespace {

constexpr int BadHelperArguments = 2;
constexpr int ExitOnRequestCode = 5;
constexpr int ExitNowCode = 6;
constexpr DWORD ControlCExitStatus = 0xC000013A; // STATUS_CONTROL_C_EXIT

bool ModeIs(const char* mode, const char* name) noexcept {
    return strcmp(mode, name) == 0;
}

} // namespace

ROCKS_NOINLINE bool FailFast() noexcept {
    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
}

bool ExitWithControlCStatus() noexcept {
    (void)TerminateProcess(GetCurrentProcess(), ControlCExitStatus);
    return true;
}

bool WaitForProcessExit(uint32 processId, uint32 timeoutMilliseconds) noexcept {
    const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
    if (process == nullptr) {
        return GetLastError() == ERROR_INVALID_PARAMETER;
    }

    const bool exited = WaitForSingleObject(process, timeoutMilliseconds) == WAIT_OBJECT_0;
    CloseHandle(process);
    return exited;
}

namespace {

// ROCKS_CRASH_PROBE_HELPER 값이다(정적 초기화에서 읽는다).
char helperMode[64];

// 고장 난 도우미 본체다(PlatformBase가 도우미 인자를 검사한 뒤 부모·요청 핸들로 부른다).
int RunFaultyCrashHelper(HANDLE parent, HANDLE request) noexcept {
    const char* mode = helperMode;
    if (ModeIs(mode, "hang")) {
        // 캡처 신호를 보내지 않는다. 부모가 끝나면 함께 끝난다(남지 않는다).
        (void)WaitForSingleObject(parent, INFINITE);
        return 0;
    }

    if (ModeIs(mode, "exit-on-request")) {
        const HANDLE waits[] = {request, parent};
        (void)WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        return ExitOnRequestCode;
    }

    return ModeIs(mode, "exit-now") ? ExitNowCode : BadHelperArguments;
}

// 부모가 끝나기를 기다리는 상한이다(부모의 캡처 마감·콜백·프로세스 안 덤프 예산보다 넉넉하다).
constexpr DWORD GateParentWaitMilliseconds = 120000;

void WaitForParentExit(HANDLE parent) noexcept {
    (void)WaitForSingleObject(parent, GateParentWaitMilliseconds);
}

} // namespace

bool InstallCrashHelperTestMode() noexcept {
    const DWORD length =
        GetEnvironmentVariableA("ROCKS_CRASH_PROBE_HELPER", helperMode, sizeof(helperMode));
    if (length == 0 || length >= sizeof(helperMode)) {
        helperMode[0] = '\0';
        return false;
    }

    if (ModeIs(helperMode, "gate-parent-first")) {
        platform::detail::SetCrashHelperCaptureHook(&WaitForParentExit);
    } else {
        platform::detail::SetCrashHelperOverride(&RunFaultyCrashHelper);
    }

    return true;
}

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER filterBeforeTest = nullptr;

LONG WINAPI TestUnhandledFilter(EXCEPTION_POINTERS*) noexcept {
    return EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

bool ReplaceUnhandledFilterForTest() noexcept {
    filterBeforeTest = SetUnhandledExceptionFilter(&TestUnhandledFilter);
    return true;
}

bool TestUnhandledFilterIsCurrent() noexcept {
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(&TestUnhandledFilter);
    if (current != &TestUnhandledFilter) {
        (void)SetUnhandledExceptionFilter(current);
        return false;
    }

    return true;
}

void RestoreUnhandledFilterForTest() noexcept {
    (void)SetUnhandledExceptionFilter(filterBeforeTest);
    filterBeforeTest = nullptr;
}

bool EnterKillOnCloseJob(bool allowBreakaway) noexcept {
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | (allowBreakaway ? JOB_OBJECT_LIMIT_BREAKAWAY_OK : 0u);
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
        !AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return false;
    }

    // 핸들은 프로세스 종료 때 닫힌다(그때 job 안 프로세스가 모두 끝난다).
    return true;
}

bool EnterBreakawayBlockingJob() noexcept {
    const HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        return false;
    }

    // 제한을 걸지 않은 새 job은 breakaway를 허락하지 않는다(JOB_OBJECT_LIMIT_BREAKAWAY_OK 없음).
    if (!AssignProcessToJobObject(job, GetCurrentProcess())) {
        CloseHandle(job);
        return false;
    }

    return true;
}

} // namespace rocks::test
