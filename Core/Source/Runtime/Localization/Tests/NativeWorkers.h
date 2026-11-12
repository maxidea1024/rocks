#pragma once

#include <Base/Types.h>
#include <Platform/Fatal.h>
#include <Platform/Thread.h>

namespace rocks::test::localization {

inline void YieldNativeWorker() noexcept {
    platform::SleepFor(1);
}

// context는 이 묶음보다 먼저 선언하여 완전 join 뒤에 파괴한다.
// entry 반환 deadline과 native 종료 join을 분리한다. timeout 후 context를 둔 채
// 반환하면 테스트 자체가 UAF를 만들므로 교착은 프로세스 실패로 드러낸다.
class NativeWorkers {
public:
    using Entry = platform::Thread::Entry;
    static constexpr usize Capacity = 4;
    NativeWorkers() noexcept = default;
    NativeWorkers(const NativeWorkers&) = delete;
    NativeWorkers& operator=(const NativeWorkers&) = delete;

    ~NativeWorkers() noexcept {
        JoinAll();
    }

    bool Start(Entry entry, void* context) noexcept {
        if (count_ == Capacity) {
            return false;
        }
        if (threads_[count_].Start(entry, context) != platform::ThreadStatus::Success) {
            return false;
        }
        ++count_;
        return true;
    }

    void JoinAll() noexcept {
        for (usize index = 0; index < count_; ++index) {
            if (threads_[index].WaitForCompletion(10000) != platform::WaitResult::Signaled) {
                platform::Fatal("Localization worker entry did not finish within 10 seconds");
            }
            if (threads_[index].Join() != platform::ThreadStatus::Success) {
                platform::Fatal("Localization worker native join failed");
            }
        }
        count_ = 0;
    }

private:
    platform::Thread threads_[Capacity];
    usize count_ = 0;
};
} // namespace rocks::test::localization
