#include <TestSupport/OsThread.h>

#include <Windows.h>

namespace rocks::test {
namespace {

struct OsThreadStart {
    void (*entry)(void*) noexcept;
    void* context;
};

DWORD WINAPI OsThreadEntry(void* raw) noexcept {
    const auto& start = *static_cast<const OsThreadStart*>(raw);
    start.entry(start.context);
    return 0;
}

} // namespace

bool RunOnOsThread(void (*entry)(void*) noexcept, void* context) noexcept {
    OsThreadStart start{entry, context};
    const HANDLE thread = CreateThread(nullptr, 0, OsThreadEntry, &start, 0, nullptr);
    if (thread == nullptr) {
        return false;
    }

    // 스레드 handle은 스레드 종료 정리(FLS callback 포함)가 끝난 뒤에 신호된다.
    const bool finished = WaitForSingleObject(thread, INFINITE) == WAIT_OBJECT_0;
    CloseHandle(thread);
    return finished;
}

} // namespace rocks::test
