#pragma once

#include <Core/Containers/HashMap.h>
#include <Core/Memory/Allocator.h>

// 할당자 내부 공용 구현이다(공개 API 아님 — Docs/Design/GarbageCollectionScale.ko.md 18.6절).
// SystemAllocator와 BinnedAllocator의 큰 할당이 함께 쓴다.
namespace rocks::detail {

// 플랫폼 할당을 그대로 부르는 추적 없는 allocator다. 할당자가 자기 원장의 저장소를 받을 때 쓴다 —
// 원장 저장소를 자기 자신에게서 받으면 추적이 되돌아 들어온다. 상태가 없어 여러 스레드에서 불러도
// 되지만, FailOnAttempt를 쓰는 동안은 호출자가 직렬화한다(원장은 소유 할당자의 잠금 안에서만 쓴다).
class UntrackedPlatformAllocator final : public Allocator {
public:
    // 잘못된 정렬(0, 2의 거듭제곱 아님)은 InvalidArgument, 0 byte는 성공한 nullptr다.
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override;
    // 이 allocator가 준 주소만 넘긴다(검사하지 않는다). nullptr는 무동작이다.
    Result<void> Deallocate(void* address) noexcept override;

    // 시험 전용: 지금부터 oneBasedAttempt번째 Allocate를 OutOfMemory로 만든다(0이면 끈다).
    void FailOnAttempt(uint64 oneBasedAttempt) noexcept {
        attempts_ = 0;
        failureAttempt_ = oneBasedAttempt;
    }

private:
    uint64 attempts_ = 0;
    uint64 failureAttempt_ = 0;
};

// 살아 있는 할당의 주소 → 크기 원장이다. 주소를 역참조하지 않고 값으로만 찾으므로, 남의 주소나 이미
// 반환한 주소를 물어도 안전하다. 찾기·넣기·지우기는 상환 O(1)이다(Core HashMap — 지운 칸은 표가
// 스스로 정리한다). 동기화는 호출자가 한다(한 번에 한 스레드).
//
// 사용 예(할당자의 잠금 안에서):
//   if (auto inserted = ledger_.Insert(address, bytes); !inserted) { /* 블록을 반환하고 실패 */ }
//   usize bytes = 0;
//   if (!ledger_.Remove(address, bytes)) { return Result<void>::Failure(ErrorCode::NotFound); }
class AddressLedger {
public:
    // storage는 원장 표의 저장소이고 이 원장보다 오래 살아야 한다.
    explicit AddressLedger(Allocator& storage) noexcept;

    AddressLedger(const AddressLedger&) = delete;
    AddressLedger& operator=(const AddressLedger&) = delete;

    // 주소를 적는다. 표를 늘리지 못하면 그 오류(OutOfMemory 등)이고 원장은 그대로다. null이거나
    // 이미 적힌 주소는 InvalidArgument다(살아 있는 주소를 두 번 내줄 수 없다 — 호출자의 결함).
    Result<void> Insert(const void* address, usize bytes) noexcept;

    // 적힌 주소면 지우고 그 크기를 bytes에 쓴 뒤 참이다. 없으면 bytes를 건드리지 않고 거짓이다.
    bool Remove(const void* address, usize& bytes) noexcept;

    // 적힌 주소 수다.
    usize Count() const noexcept;

private:
    HashMap<uptr, usize> entries_;
};

} // namespace rocks::detail
