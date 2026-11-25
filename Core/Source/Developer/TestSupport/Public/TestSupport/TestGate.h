#pragma once

#include <Platform/Atomics.h>
#include <Platform/Sync/WaiterSignal.h>

namespace rocks::test {

// 시험 순서를 만드는 gate다(Docs/Design/Synchronization 14.2절). sleep 대신 "다른 스레드가 이제 그
// 지점에 왔다"를 알린다. 시험 대상(ParkingLot·Mutex 계열)에 기대지 않도록 WaiterSignal 위에 둔다.
// 기다리는 스레드는 하나다(시험 본문). 여는 쪽은 여럿이어도 된다.
//
// 사용 예(대기자가 잠든 뒤에 깨운다):
//   TestGate parked;               // ParkingLot onParked 훅이 parked.Open()
//   parked.Wait();                 // 대기 스레드가 잠들 때까지
//   mutex.Unlock();
class TestGate {
public:
    void Open() noexcept {
        signal_.Notify();
    }

    void Wait() noexcept {
        signal_.Wait();
    }

    // 다음 Open을 기다리기 전에 닫는다(기다리는 스레드가 부른다).
    void Reset() noexcept {
        signal_.Reset();
    }

    bool IsOpen() const noexcept {
        return signal_.IsNotified();
    }

private:
    platform::WaiterSignal signal_;
};

// 열린 횟수를 세는 gate다. 여러 스레드가 각자 한 번씩 연 것을 시험이 모두 기다린다.
//
// 사용 예:
//   CountingGate parked;           // 대기 스레드마다 onParked에서 parked.Arrive()
//   parked.WaitFor(3);             // 셋 모두 잠들 때까지
class CountingGate {
public:
    void Arrive() noexcept {
        count_.FetchAdd<platform::MemoryOrder::AcquireRelease>(1);
        signal_.Notify();
    }

    // count번 이상 도착할 때까지 기다린다.
    void WaitFor(uint32 count) noexcept {
        while (count_.Load<platform::MemoryOrder::Acquire>() < count) {
            signal_.Wait();
            signal_.Reset();
        }
    }

    uint32 Count() const noexcept {
        return count_.Load<platform::MemoryOrder::Acquire>();
    }

private:
    platform::Atomic<uint32> count_;
    platform::WaiterSignal signal_;
};

} // namespace rocks::test
