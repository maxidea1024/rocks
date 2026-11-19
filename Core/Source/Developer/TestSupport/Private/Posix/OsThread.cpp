#include <TestSupport/OsThread.h>

#include <pthread.h>

namespace rocks::test {
namespace {

struct OsThreadStart {
    void (*entry)(void*) noexcept;
    void* context;
};

void* OsThreadEntry(void* raw) noexcept {
    const auto& start = *static_cast<const OsThreadStart*>(raw);
    start.entry(start.context);
    return nullptr;
}

} // namespace

bool RunOnOsThread(void (*entry)(void*) noexcept, void* context) noexcept {
    OsThreadStart start{entry, context};
    pthread_t thread{};
    if (pthread_create(&thread, nullptr, OsThreadEntry, &start) != 0) {
        return false;
    }

    // pthread_join은 key 소멸자까지 끝난 뒤에 돌아온다.
    return pthread_join(thread, nullptr) == 0;
}

} // namespace rocks::test
