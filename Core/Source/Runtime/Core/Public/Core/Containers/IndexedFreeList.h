#pragma once

#include <Base/PlacementNew.h>
#include <Base/Types.h>
#include <Core/Containers/StepPolicy.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

// 작은 태그 폭을 허용하는지다. 기본 정책은 허용하지 않는다(태그 감김 ABA 시험만 연다).
template <class StepPolicy>
inline constexpr bool AllowsSmallFreeListTag = requires { StepPolicy::AllowSmallTag; };

// 미리 잡은 칸 배열 위의 lock-free Treiber 스택이다(UE TLockFreePointerListLIFO의 색인 링크 대응,
// Docs/Design/Synchronization 12.2·12.4절). 칸 색인을 넣고 빼며, 칸 내용은 호출자가 배열로 가진다.
// 블록·노드 풀의 빈 칸 목록에 쓴다(회수 없음: 칸은 목록 수명 동안 해제하지 않는다).
//
// ABA: 머리는 64-bit 하나에 [태그 TagBits | 색인 IndexBits]를 담고 push·pop마다 태그를 1 올린다.
// 기본은 태그 40 bit·색인 24 bit(칸 약 1,677만 개)다. 태그는 UE(38 bit) 이상이어야 한다(시험 정책만
// 작게 연다). 태그 감김은 정상 동작이다. 128-bit CAS·포인터 상위 bit 태그는 쓰지 않는다.
//
// 성질: Push·TryPop은 어느 스레드에서나 부른다(lock-free). 다음 색인은 칸마다 Atomic<uint32>다.
// 넣은 색인은 다시 꺼낼 때까지 넣은 쪽이 쓰지 않는다.
//
// 메모리 순서: Push의 CAS(release)가 넣기 전 칸에 쓴 내용을 발행하고, TryPop의 CAS(acquire)가 본다.
//
// 사용 예:
//   IndexedFreeList<> free;
//   if (!free.Initialize(allocator, blockCount, IndexedFreeListStart::AllFree)) { ... }
//   uint32 index = 0;
//   if (free.TryPop(index)) { Use(blocks[index]); free.Push(index); }
enum class IndexedFreeListStart : uint8 {
    // 처음에 모든 색인이 목록에 있다(0이 맨 위).
    AllFree,
    // 처음에 비어 있다(호출자가 Push로 채운다).
    Empty
};

template <uint32 IndexBits = 24, uint32 TagBits = 64 - IndexBits, class StepPolicy = NoSteps>
class IndexedFreeList {
    static_assert(IndexBits >= 1 && IndexBits <= 32, "IndexBits must be 1..32");
    static_assert(IndexBits + TagBits <= 64, "IndexBits + TagBits must fit in 64 bits");
    static_assert(TagBits >= 38 || AllowsSmallFreeListTag<StepPolicy>,
                  "IndexedFreeList tag must be at least 38 bits (ABA margin as in UE)");

public:
    // 색인 "없음"이다(목록 끝). 그래서 쓸 수 있는 칸은 IndexNone개(0..IndexNone−1)다.
    static constexpr uint32 IndexNone = static_cast<uint32>((uint64{1} << IndexBits) - 1);
    static constexpr usize MaxCapacity = IndexNone;

    IndexedFreeList() noexcept = default;
    IndexedFreeList(const IndexedFreeList&) = delete;
    IndexedFreeList& operator=(const IndexedFreeList&) = delete;

    ~IndexedFreeList() noexcept {
        Shutdown();
    }

    // capacity칸의 다음 색인 배열을 할당한다. 실패: capacity 0·MaxCapacity 초과·이미 초기화됨은
    // InvalidArgument, 그 밖은 allocator 오류(상태는 그대로).
    Result<void> Initialize(Allocator& allocator, usize capacity,
                            IndexedFreeListStart start) noexcept {
        if (next_ != nullptr || capacity == 0 || capacity > MaxCapacity) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        auto memory = allocator.Allocate(capacity * sizeof(platform::Atomic<uint32>),
                                         alignof(platform::Atomic<uint32>));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        next_ = static_cast<platform::Atomic<uint32>*>(memory.Value());
        allocator_ = &allocator;
        capacity_ = static_cast<uint32>(capacity);
        for (uint32 index = 0; index < capacity_; ++index) {
            const uint32 next = start == IndexedFreeListStart::AllFree && index + 1 < capacity_
                                    ? index + 1
                                    : IndexNone;
            new (&next_[index]) platform::Atomic<uint32>(next);
        }

        head_.Store<platform::MemoryOrder::Relaxed>(
            Pack(0, start == IndexedFreeListStart::AllFree ? 0 : IndexNone));
        return Result<void>::Success();
    }

    // 배열을 돌려준다. 다른 스레드가 쓰지 않을 때만 부른다.
    void Shutdown() noexcept {
        if (next_ != nullptr) {
            if (!allocator_->Deallocate(next_)) {
                platform::Fatal("IndexedFreeList storage deallocation was refused");
            }

            next_ = nullptr;
            capacity_ = 0;
        }
    }

    uint32 Capacity() const noexcept {
        return capacity_;
    }

    // index를 맨 위에 넣는다. index는 Capacity() 미만이고 지금 목록에 없어야 한다.
    void Push(uint32 index) noexcept {
        if (index >= capacity_) {
            platform::Fatal("IndexedFreeList index out of range");
        }

        uint64 head = head_.Load<platform::MemoryOrder::Relaxed>();
        while (true) {
            next_[index].Store<platform::MemoryOrder::Relaxed>(IndexOf(head));
            StepPolicy::Step("IndexedFreeList.PushLinked");
            if (head_.CompareExchangeWeak<platform::MemoryOrder::Release,
                                          platform::MemoryOrder::Relaxed>(
                    head, Pack(TagOf(head) + 1, index))) {
                return;
            }
        }
    }

    // 맨 위 색인을 꺼낸다. 비었으면 false다(index는 그대로).
    bool TryPop(uint32& index) noexcept {
        uint64 head = head_.Load<platform::MemoryOrder::Acquire>();
        while (true) {
            const uint32 top = IndexOf(head);
            if (top == IndexNone) {
                return false;
            }

            // top이 그 사이 다른 스레드에 꺼내져 다시 쓰였으면 next는 낡은 값이지만, 그때는 태그가
            // 바뀌어 CAS가 실패한다(칸 배열은 해제하지 않으므로 읽기는 안전하다).
            const uint32 next = next_[top].Load<platform::MemoryOrder::Relaxed>();
            StepPolicy::Step("IndexedFreeList.PopReadNext");
            if (head_.CompareExchangeWeak<platform::MemoryOrder::Acquire,
                                          platform::MemoryOrder::Acquire>(
                    head, Pack(TagOf(head) + 1, next))) {
                index = top;
                return true;
            }
        }
    }

    // 지금 목록이 비었는지다(어느 스레드에서나, 곧 낡을 수 있다).
    bool IsEmpty() const noexcept {
        return IndexOf(head_.Load<platform::MemoryOrder::Acquire>()) == IndexNone;
    }

private:
    static constexpr uint64 IndexMask = (uint64{1} << IndexBits) - 1;
    static constexpr uint64 TagMask =
        TagBits == 64 ? NumericLimits<uint64>::Max() : (uint64{1} << TagBits) - 1;

    static uint64 Pack(uint64 tag, uint32 index) noexcept {
        return ((tag & TagMask) << IndexBits) | index;
    }

    static uint32 IndexOf(uint64 head) noexcept {
        return static_cast<uint32>(head & IndexMask);
    }

    static uint64 TagOf(uint64 head) noexcept {
        return (head >> IndexBits) & TagMask;
    }

    alignas(CacheLineSize) platform::Atomic<uint64> head_;
    platform::Atomic<uint32>* next_ = nullptr;
    Allocator* allocator_ = nullptr;
    uint32 capacity_ = 0;
};

} // namespace rocks
