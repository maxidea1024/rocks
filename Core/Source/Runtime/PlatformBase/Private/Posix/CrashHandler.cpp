#include "../Common/TerminationInternal.h"

#include <Platform/Atomics.h>
#include <Platform/CrashHandler.h>
#include <Platform/Memory.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

namespace rocks::platform {

namespace {

// 처리하는 치명 신호다.
constexpr int CrashSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
constexpr usize CrashSignalCount = sizeof(CrashSignals) / sizeof(CrashSignals[0]);
// 종료 사유 표기 "Fatal signal " + 10진 최대 3자리 + NUL이다.
constexpr usize CrashMessageBytes = 32;
constexpr int DecimalBase = 10;
constexpr int64 NanosecondsPerMillisecond = 1000000;
constexpr int64 NanosecondsPerSecond = 1000000000;

constinit uint32 installed = 0;
constinit uint32 crashing = 0;
constinit uint32 stopping = 0;
constinit uint32 flushed = 0;
constinit uint32 timeoutMilliseconds = 0;
constinit char crashMessage[CrashMessageBytes] = {};
constinit usize crashMessageBytes = 0;
struct sigaction previousActions[CrashSignalCount];
// flush 스레드를 깨우는 self-pipe다. 이름 없는 POSIX 세마포어는 macOS가 지원하지 않아(deprecated,
// 실행 때 실패) write(async-signal-safe)로 깨운다. 쓰는 쪽은 non-blocking이라 처리기가 막히지
// 않는다.
constexpr int WakeReadEnd = 0;
constexpr int WakeWriteEnd = 1;
constexpr int NoDescriptor = -1;
constinit int wakePipe[2] = {NoDescriptor, NoDescriptor};
pthread_t flushThread;

// 스레드마다 처리기용 대체 stack이다. 정적 TLS(모든 스레드가 32 KiB를 차지하고 dlopen 정적 TLS가
// 모자랄 수 있다) 대신 스레드를 준비할 때 따로 잡고 스레드가 끝나면 돌려준다. 이미 대체 stack이
// 있으면(ASan 등) 덮어쓰지 않는다.
constexpr usize AlternateStackAlignment = 16;
thread_local void* alternateStack = nullptr;

void FormatCrashMessage(int signal) noexcept {
    constexpr char Prefix[] = "Fatal signal ";
    usize length = 0;
    for (; Prefix[length] != '\0'; ++length) {
        crashMessage[length] = Prefix[length];
    }

    char digits[CrashMessageBytes];
    usize count = 0;
    int value = signal > 0 ? signal : 0;
    do {
        digits[count++] = static_cast<char>('0' + value % DecimalBase);
        value /= DecimalBase;
    } while (value != 0 && count < sizeof(digits));

    while (count > 0 && length + 1 < CrashMessageBytes) {
        crashMessage[length++] = digits[--count];
    }

    crashMessage[length] = '\0';
    crashMessageBytes = length;
}

int64 MonotonicNow() noexcept {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now); // async-signal-safe
    return static_cast<int64>(now.tv_sec) * NanosecondsPerSecond + now.tv_nsec;
}

// flush 스레드를 깨운다. 처리기에서 부르므로 async-signal-safe 호출만 쓴다. pipe가 차 있으면
// (EAGAIN) 이미 깨울 byte가 있으므로 그대로 둔다.
void WakeFlushThread() noexcept {
    const char byte = 0;
    while (write(wakePipe[WakeWriteEnd], &byte, 1) < 0 && errno == EINTR) {
    }
}

// 깨울 byte가 올 때까지 막혀 기다린다. 신호로 끊기면(EINTR) 다시 기다린다.
void WaitForWake() noexcept {
    char byte = 0;
    while (read(wakePipe[WakeReadEnd], &byte, 1) < 0 && errno == EINTR) {
    }
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

// 크래시를 기다리다가 종료 hook을 부르는 스레드다.
void* FlushThreadMain(void*) noexcept {
    PrepareThreadForCrashHandling();
    WaitForWake();

    if (AtomicLoad<MemoryOrder::Acquire>(&stopping) == 0) {
        detail::RunTerminationHook(crashMessage, crashMessageBytes);
        AtomicStore<MemoryOrder::Release>(&flushed, 1u);
    }

    return nullptr;
}

usize SignalIndex(int signal) noexcept {
    for (usize index = 0; index < CrashSignalCount; ++index) {
        if (CrashSignals[index] == signal) {
            return index;
        }
    }

    return CrashSignalCount;
}

// 신호 처리기다. flush 스레드를 깨우고(pipe write는 async-signal-safe) 완료 표시를 제한 시간까지
// 확인한다. 그 뒤 앞 동작을 되돌리고 신호를 다시 낸다(최선 노력).
void CrashSignalHandler(int signal, siginfo_t*, void*) noexcept {
    // 처음 들어온 스레드가 flush 스레드를 깨운다. 겹쳐 들어온 스레드도 같은 제한 시간까지 기다린 뒤
    // 연쇄한다(첫 스레드의 flush를 끊지 않게).
    if (AtomicExchange<MemoryOrder::AcquireRelease>(&crashing, 1u) == 0) {
        detail::RecordTerminatingThread(CurrentThreadId());
        FormatCrashMessage(signal);
        WakeFlushThread();
    }

    const int64 deadline =
        MonotonicNow() +
        static_cast<int64>(AtomicLoad<MemoryOrder::Relaxed>(&timeoutMilliseconds)) *
            NanosecondsPerMillisecond;
    while (AtomicLoad<MemoryOrder::Acquire>(&flushed) == 0 && MonotonicNow() < deadline) {
        sched_yield();
    }

    const usize index = SignalIndex(signal);
    if (index != CrashSignalCount) {
        (void)sigaction(signal, &previousActions[index], nullptr);
    }

    (void)raise(signal);
}

} // namespace

bool InstallCrashHandler(const CrashHandlerOptions& options) noexcept {
    if (AtomicLoad<MemoryOrder::Acquire>(&installed) != 0) {
        return true;
    }

    AtomicStore<MemoryOrder::Relaxed>(&timeoutMilliseconds, options.flushTimeoutMilliseconds);
    AtomicStore<MemoryOrder::Relaxed>(&stopping, 0u);
    if (!OpenWakePipe()) {
        return false;
    }

    if (pthread_create(&flushThread, nullptr, &FlushThreadMain, nullptr) != 0) {
        CloseWakePipe();
        return false;
    }

    PrepareThreadForCrashHandling();
    struct sigaction action{};
    action.sa_sigaction = &CrashSignalHandler;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (usize index = 0; index < CrashSignalCount; ++index) {
        (void)sigaction(CrashSignals[index], &action, &previousActions[index]);
    }

    AtomicStore<MemoryOrder::Release>(&installed, 1u);
    return true;
}

void RemoveCrashHandler() noexcept {
    if (AtomicExchange<MemoryOrder::AcquireRelease>(&installed, 0u) == 0) {
        return;
    }

    for (usize index = 0; index < CrashSignalCount; ++index) {
        (void)sigaction(CrashSignals[index], &previousActions[index], nullptr);
    }

    AtomicStore<MemoryOrder::Release>(&stopping, 1u);
    WakeFlushThread();
    (void)pthread_join(flushThread, nullptr);
    CloseWakePipe();
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

} // namespace rocks::platform
