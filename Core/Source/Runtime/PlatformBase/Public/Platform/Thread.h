#pragma once

#include <Platform/Sync.h>

namespace rocks::platform {

enum class ThreadStatus {
    Success,
    InvalidArgument,
    AlreadyJoinable,
    NotJoinable,
    SelfJoin,
    Busy,
    SystemError
};

// native thread 하나의 join 소유권이다. entry/context는 Start 전에 준비하고 Join 성공까지 유지한다.
// 실행 함수는 noexcept이며 OS 종료 API로 wrapper를 건너뛰거나 thread를 강제 취소하지 않는다.
// Start 실패는 새 join 소유권을 만들지 않는다. Wait timeout/Join 실패는 소유권을 유지한다.
// Join 성공 뒤 재시작 가능하다. detach는 없고 joinable 상태 소멸은 Fatal로 수명 오류를 드러낸다.
// 동시 제어 요청은 gate로 중재한다. 진행 중 Join은 다른 Join/Start에 Busy/AlreadyJoinable을
// 반환한다. self Join은 SelfJoin이다. 객체 파괴는 모든 제어 호출이 끝난 뒤 수행한다.
class Thread {
public:
    using Entry = void (*)(void*) noexcept;

    Thread() noexcept;
    ~Thread() noexcept;
    Thread(const Thread&) = delete;
    Thread& operator=(const Thread&) = delete;
    Thread(Thread&&) = delete;
    Thread& operator=(Thread&&) = delete;

    ThreadStatus Start(Entry entry, void* context = nullptr) noexcept;
    // entry 함수의 반환까지 기다린다. TLS destructor/native thread 종료까지를 뜻하지 않는다.
    // timeout은 소유권을 바꾸지 않는다. 비joinable/self wait는 Failed다.
    WaitResult WaitForCompletion(uint32 timeoutMs = InfiniteTimeout) noexcept;
    // TLS 정리와 native 종료까지 회수한다. 제한시간 없는 대기이며 성공 후 context를 해제한다.
    ThreadStatus Join() noexcept;
    bool IsJoinable() const noexcept;

private:
    alignas(16) unsigned char storage_[256]{};
};

// SleepFor의 모든 값은 유한 millisecond다. 0은 즉시 반환한다. 실제 wake 시점의 정확성은 보장하지
// 않는다.
void SleepFor(uint32 milliseconds) noexcept;
void YieldThread() noexcept;

} // namespace rocks::platform
