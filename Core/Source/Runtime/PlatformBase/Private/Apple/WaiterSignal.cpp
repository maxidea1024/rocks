#include <Platform/Sync/WaiterSignal.h>
#include <Platform/Fatal.h>

#include "../Posix/PosixPlatform.h"

#include <errno.h>
#include <pthread.h>

namespace rocks::platform {

namespace {

struct SignalObjects {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
};

SignalObjects& Objects(unsigned char* storage) noexcept {
    return *reinterpret_cast<SignalObjects*>(storage);
}

} // namespace

// macOS 최소 13.0에는 주소 대기(os_sync_wait_on_address, 14.4부터)가 없고 __ulock_wait는 비공개
// API라 쓰지 않는다. UE도 Apple은 조건 변수판이다(Docs/Design/Synchronization 7.3절).
WaiterSignal::WaiterSignal() noexcept {
    static_assert(sizeof(SignalObjects) <= sizeof(storage_));
    static_assert(alignof(SignalObjects) <= alignof(WaiterSignal));
    SignalObjects& objects = Objects(storage_);
    if (pthread_mutex_init(&objects.mutex, nullptr) != 0) {
        Fatal("pthread_mutex_init failed");
    }

    detail::PosixInitializeCondition(&objects.condition);
}

WaiterSignal::~WaiterSignal() noexcept {
    SignalObjects& objects = Objects(storage_);
    pthread_cond_destroy(&objects.condition);
    pthread_mutex_destroy(&objects.mutex);
}

// mutex를 쥔 채 신호를 세우고 깨운다. 대기자는 그 mutex를 다시 잡아 신호를 본 뒤에야 돌아가므로
// 깨우는 쪽의 마지막 접근은 unlock이다(7.3절).
void WaiterSignal::Notify() noexcept {
    SignalObjects& objects = Objects(storage_);
    pthread_mutex_lock(&objects.mutex);
    state_.Store<MemoryOrder::Release>(1);
    pthread_cond_signal(&objects.condition);
    pthread_mutex_unlock(&objects.mutex);
}

// 신호를 잠금 없이 보고 돌아가면 Notify가 아직 mutex를 쥐고 있을 수 있다(그 뒤 파괴하면 쥔 mutex의
// 파괴다). 한 번 잡았다 놓아 Notify의 unlock 뒤로 순서를 맞춘다.
void WaiterSignal::Settle() noexcept {
    SignalObjects& objects = Objects(storage_);
    pthread_mutex_lock(&objects.mutex);
    pthread_mutex_unlock(&objects.mutex);
}

void WaiterSignal::WaitOnce(uint32 timeoutMs) noexcept {
    SignalObjects& objects = Objects(storage_);
    pthread_mutex_lock(&objects.mutex);
    if (state_.Load<MemoryOrder::Acquire>() == 0) {
        const int result =
            timeoutMs == InfiniteTimeout
                ? pthread_cond_wait(&objects.condition, &objects.mutex)
                : detail::PosixTimedWait(&objects.condition, &objects.mutex, timeoutMs);
        if (result != 0 && result != ETIMEDOUT) {
            Fatal("pthread_cond_wait failed");
        }
    }

    pthread_mutex_unlock(&objects.mutex);
}

} // namespace rocks::platform
