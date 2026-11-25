#pragma once

#include <Base/Compiler.h>
#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <Platform/Sync/WaitTypes.h>

namespace rocks::platform {

// 스레드 하나가 기다리고 다른 스레드가 깨우는 수동 리셋 신호다(UE FPlatformManualResetEvent 대응,
// Docs/Design/Synchronization 7.3절). ParkingLot의 스레드 레코드·대기 노드와 시험 gate의 바탕이며,
// 모든 잠금·신호의 OS 의존은 이 타입 하나다.
//
// - 대기자는 늘 한 스레드다(둘이 같은 신호를 함께 기다리면 계약 위반). 깨우는 쪽은 여럿이어도 된다.
// - Notify는 신호를 세우고 대기자를 깨운다. Wait·WaitFor는 신호가 설 때까지 잔다. 신호는 Reset이
//   지운다(대기자가 다음 대기를 시작하기 전에 부른다). Notify 앞의 쓰기는 Wait가 돌아온 뒤 보인다.
// - Windows(WaitOnAddress)·Android(futex)의 깨우기는 주소를 키로만 쓴다. 대기자가 먼저 깨어 신호가
//   사라진 뒤에 깨우기가 불려도 그 메모리를 건드리지 않는다. macOS판은 깨우는 쪽이 내부 mutex를 쥔
//   채 신호를 세우고 cond를 깨운 뒤 푼다. 그 해제가 깨우는 쪽의 마지막 접근이다.
// - OS 대기의 예상 밖 실패(다시 시도할 수 없는 오류)는 Fatal이다.
//
// 사용 예:
//   WaiterSignal signal;          // 대기 스레드가 소유한다
//   signal.Reset();
//   Publish(&signal);             // 깨울 쪽에 알린다. 깨우는 쪽: signal->Notify();
//   signal.Wait();
class ROCKS_PLATFORMBASE_API WaiterSignal {
public:
    WaiterSignal() noexcept;
    ~WaiterSignal() noexcept;
    WaiterSignal(const WaiterSignal&) = delete;
    WaiterSignal& operator=(const WaiterSignal&) = delete;

    // 신호를 지운다. 대기자만 부르고, 깨우는 쪽이 이 신호를 더 건드리지 않음을 안 뒤에 부른다.
    void Reset() noexcept {
        state_.Store<MemoryOrder::Relaxed>(0);
    }

    // 신호가 섰는지 본다(Acquire). 시험·진단과 기다리기 전 빠른 확인에 쓴다. true여도 깨우는 쪽이
    // 아직 Notify 안에 있을 수 있으므로, 신호를 파괴·재사용해도 되는 것은 Wait·WaitFor가 돌아온
    // 뒤뿐이다.
    bool IsNotified() const noexcept {
        return state_.Load<MemoryOrder::Acquire>() != 0;
    }

    // 신호가 설 때까지 잔다.
    void Wait() noexcept;

    // timeoutMs 안에 신호가 서면 true, 시간이 다 되면 false다. 0은 즉시 검사, InfiniteTimeout은
    // Wait와 같다. 시한은 처음 부른 때의 단조 시계 기준이라 spurious wake로 늘어나지 않는다.
    bool WaitFor(uint32 timeoutMs) noexcept;

    // 신호를 세우고 대기자를 깨운다(Release). 대기자가 없거나 이미 섰으면 깨우기만 비용이다.
    void Notify() noexcept;

private:
    // Wait·WaitFor가 신호를 보고 돌아가기 직전에 부른다. 깨우는 쪽의 Notify가 이 객체를 더 건드리지
    // 않음을 보장한다(macOS: 내부 mutex를 한 번 잡았다 놓아 Notify의 해제를 기다린다. 주소 대기
    // 플랫폼은 깨우기가 주소를 키로만 써서 할 일이 없다). OS 구현 파일이 정의한다.
    void Settle() noexcept;

    // 신호가 없으면 OS 대기로 한 번 잔다(timeoutMs 또는 InfiniteTimeout). 깨우기·시한·spurious wake
    // 어느 쪽으로든 돌아오며, 호출자가 신호와 남은 시간을 다시 본다. OS 구현 파일이 정의한다.
    void WaitOnce(uint32 timeoutMs) noexcept;

    // 0 = 신호 없음, 1 = 신호. Windows·Android는 이 주소에서 잔다(Atomic<uint32>는 표준 배치라
    // 객체 주소가 값 주소다).
    Atomic<uint32> state_;
    // macOS 구현의 pthread_mutex_t + pthread_cond_t 자리다(주소 대기 플랫폼은 쓰지 않는다). 공개
    // 헤더는 OS를 가르지 않으므로 늘 둔다. OS 타입을 노출하지 않는다(구현이 크기를
    // static_assert한다).
    [[maybe_unused]] alignas(16) unsigned char storage_[128];
};

} // namespace rocks::platform
