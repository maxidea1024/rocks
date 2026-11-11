#pragma once

#include <Platform/AsyncFile.h>

// AsyncFile 공용 상태와 플랫폼 hook이다. 공용 수명·동기화 규칙은 Common/AsyncFile.cpp가 소유하고,
// OS 호출만 Private/<플랫폼>/AsyncFile.cpp가 구현한다.
//
// 동기화 규칙: 요청·파일·컨텍스트의 상태 필드는 모두 컨텍스트의 mutex로 보호한다. 완료 스레드는
// mutex 안에서 결과를 게시한 뒤에는 요청과 파일에 다시 접근하지 않는다(대기자가 즉시 파괴할 수
// 있다). 조건 변수와 mutex는 컨텍스트에 있으며 컨텍스트는 모든 파일·요청보다 오래 산다.
namespace rocks::platform::detail {

enum class AsyncPhase : uint8 {
    Idle,     // 제출한 적 없음
    Queued,   // 제출됨, 아직 OS·worker가 시작하지 않음
    Running,  // OS·worker가 진행 중
    Completed // 결과 게시됨
};

struct AsyncContextState;
struct AsyncFileState;

struct AsyncRequestState {
    AsyncContextState* context = nullptr;
    AsyncFileState* file = nullptr;
    uint64 offset = 0;
    void* destination = nullptr;  // 읽기
    const void* source = nullptr; // 쓰기
    usize bytes = 0;
    bool write = false;
    AsyncIoCallback callback = nullptr;
    void* callbackContext = nullptr;
    AsyncIoResult result{};
    AsyncPhase phase = AsyncPhase::Idle;
    bool cancelRequested = false;
    // Native: 제출 스레드가 OS 호출을 끝내기 전에는 완료를 게시하지 않는다. 제출 스레드가 OS 호출
    // 직후 요청을 다시 만지는 동안 대기자가 요청을 파괴하는 것을 막는다.
    bool submitting = false;
    AsyncRequestState* nextQueued = nullptr;
    // 플랫폼 요청 레코드(Windows OVERLAPPED 등). OS가 최종 완료를 알릴 때까지 주소가 고정되어야
    // 한다.
    alignas(16) unsigned char native[64]{};
};

struct AsyncFileState {
    AsyncContextState* context = nullptr;
    uptr handle = 0; // Windows HANDLE 또는 POSIX file descriptor
    uint64 pending = 0;
    bool open = false;
    bool writable = false;
};

struct AsyncContextState {
    Mutex mutex;
    ConditionVariable changed;
    AsyncIoBackend backend = AsyncIoBackend::WorkerPool;
    bool running = false;
    bool stopping = false;
    uint64 openFiles = 0;
    uint64 pending = 0;
    // WorkerPool
    AsyncRequestState* queueHead = nullptr;
    AsyncRequestState* queueTail = nullptr;
    Thread* workers = nullptr;
    uint32 workerCount = 0;
    // Native
    Thread completionThread;
    alignas(16) unsigned char native[32]{};
};

// ---- 공용 코드가 제공 ----
// 완료 스레드에서 호출한다. mutex를 잡지 않은 상태에서 호출하며, 게시 후 callback을 실행한다.
void CompleteAsyncRequest(AsyncRequestState& request, const AsyncIoResult& result) noexcept;

// ---- 플랫폼 hook ----
AsyncIoBackend PlatformDefaultAsyncIoBackend() noexcept;
// overlapped는 Native backend용 열기 여부다. 성공 시 handle을 소유한다. 일반 파일이 아니면
// Unsupported.
FileStatus AsyncOpenNative(const char* path, usize pathBytes, bool overlapped,
                           uptr& handle) noexcept;
// 쓰기용으로 연다. mode는 ReadWriteExisting·CreateNew·CreateAlways만 온다(공용 코드가 검사).
FileStatus AsyncOpenWriteNative(const char* path, usize pathBytes, bool overlapped,
                                FileOpenMode mode, uptr& handle) noexcept;
FileStatus AsyncCloseNative(uptr handle) noexcept;
// 이미 완료된 쓰기를 저장 장치까지 내린다.
FileStatus AsyncFlushNative(uptr handle) noexcept;
FileStatus AsyncSizeNative(uptr handle, uint64& bytes) noexcept;
// WorkerPool용 위치 지정 동기 읽기다. 짧은 읽기·EINTR을 반복하며 EOF에서 멈춘다. 실패해도
// transferred는 실패 전까지 읽은 byte 수다.
FileStatus AsyncPositionalRead(uptr handle, uint64 offset, void* destination, usize bytes,
                               usize& transferred) noexcept;
// WorkerPool용 위치 지정 동기 쓰기다. 짧은 쓰기·EINTR을 반복해 모두 쓰거나 실패한다. 진행 없는
// 0 byte 쓰기는 NoProgress다. 실패해도 transferred는 실패 전까지 쓴 byte 수다.
FileStatus AsyncPositionalWrite(uptr handle, uint64 offset, const void* source, usize bytes,
                                usize& transferred) noexcept;
// Native backend. 지원하지 않는 플랫폼은 Supported가 false이고 나머지는 호출되지 않는다.
bool AsyncNativeSupported() noexcept;
FileStatus AsyncNativeStart(AsyncContextState& context) noexcept;
void AsyncNativeStop(AsyncContextState& context) noexcept;
FileStatus AsyncNativeAttach(AsyncContextState& context, AsyncFileState& file) noexcept;
// mutex 밖에서 호출한다. OS 호출이 즉시 실패해도 완료는 완료 스레드로 전달해야 한다.
void AsyncNativeIssue(AsyncContextState& context, AsyncRequestState& request) noexcept;
// mutex 안에서 호출한다. 이미 끝났거나 아직 발행 전이면 효과가 없을 수 있다.
void AsyncNativeCancel(AsyncRequestState& request) noexcept;
void AsyncNativeCancelFile(AsyncFileState& file) noexcept;

} // namespace rocks::platform::detail
