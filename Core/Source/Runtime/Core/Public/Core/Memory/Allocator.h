#pragma once

#include <Base/Types.h>
#include <Core/Result.h>

namespace rocks {

// 성공적으로 게시된 사용자 할당의 통계다. bytes는 요청 크기이며 alignment padding이나
// allocator 자체 ledger·OS 메모리 사용량까지 포함하는 값은 아니다.
struct AllocationStats {
    uint64 liveAllocations = 0;
    uint64 liveBytes = 0;
    uint64 totalAllocations = 0;
};

// byte storage의 할당·해제를 분리한다. 객체 생성/소멸은 수행하지 않는다.
// allocator는 반환 storage와 진행 중 호출보다 오래 살아야 한다. 동시 호출 허용 여부는 구현별
// 계약이다.
class Allocator {
public:
    virtual ~Allocator() = default;

    // 유효한 alignment는 0이 아닌 2의 거듭제곱이며 bytes 단위로 요청한다.
    // 0-byte는 성공한 nullptr, 실패는 caller에게 소유 allocation을 넘기지 않는다.
    virtual Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept = 0;
    // 같은 allocator의 살아 있는 allocation을 모든 사용자 종료 후 반환한다. nullptr는 무효
    // 연산이다. raw 주소만으로는 해제된 주소가 이후 다른 allocation에 재사용된 경우를 구별할 수
    // 없다.
    virtual Result<void> Deallocate(void* address) noexcept = 0;

    // 쥐고만 있고 쓰지 않는 메모리를 OS나 상위 allocator에 돌려준다. 살아 있는 allocation은
    // 건드리지 않는다. 큰 작업이 끝난 뒤(전체 GC의 회수가 끝났을 때, World를 파괴한 뒤)나 메모리를
    // 줄여야 할 때 부른다. 구현의 스레드 계약 안에서 어느 스레드에서나 부를 수 있다.
    //
    // 이 함수와 아래 두 표시의 기본 구현은 아무것도 하지 않는다. 구현별 구분:
    // - 돌려줄 캐시가 있는 구현(BinnedAllocator)이 재정의한다.
    // - 상위 allocator 하나에 Allocate·Deallocate를 그대로 넘기는 래퍼(BudgetAllocator·
    //   DebugAllocator 등)는 이 셋도 그대로 넘긴다 — 래퍼를 끼워도 사라지지 않는다.
    // - 상위가 없거나 자기 chunk를 따로 다루는 구현(SystemAllocator·ExternalAllocator·
    //   ArenaAllocator·PoolAllocator·StackAllocator)은 기본 그대로다. 뒤 셋의 Trim()은 이것과 다른
    //   고유 연산이다(살아 있는 allocation이 없을 때만 chunk를 돌려준다).
    //
    // 사용 예:
    //   allocator.TrimMemory();   // 레벨을 내린 뒤
    virtual void TrimMemory() noexcept {
    }

    // 호출 스레드가 지금부터 이 allocator를 쓰지 않고 쉰다고(막히는 대기에 들어간다고) 알린다.
    // 스레드 캐시가 있는 구현은 그 사이에 다른 스레드의 TrimMemory가 이 스레드의 캐시를 대신 비울
    // 수 있게 한다. 막히지 않고 호출자 쪽으로 재진입하지 않는다(호출자가 자기 잠금을 쥔 채 부를 수
    // 있다). 세지 않는 상태다: 두 번 알려도 MarkThreadActive 한 번이면 쓰는 중이다. 표시한 사이에
    // 이 스레드가 Allocate·Deallocate를 불러도 된다(구현이 느린 경로로 보낸다).
    //
    // 사용 예(worker의 대기):
    //   allocator.MarkThreadIdle();
    //   event.Wait();
    //   allocator.MarkThreadActive();
    virtual void MarkThreadIdle() noexcept {
    }

    // 호출 스레드가 이 allocator를 다시 쓴다고 알린다. 이 스레드의 캐시를 다른 스레드의
    // TrimMemory가 비우는 중이면 끝날 때까지 막힐 수 있다(그래서 호출자는 다른 스레드를 붙잡는
    // 잠금을 풀고 부른다). MarkThreadIdle 없이 불러도 된다.
    virtual void MarkThreadActive() noexcept {
    }
};

} // namespace rocks
