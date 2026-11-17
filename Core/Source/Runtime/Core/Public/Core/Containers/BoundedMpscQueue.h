#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

// 생산자 여럿·소비자 하나의 유한 lock-free 큐다(Vyukov 방식, 칸마다 64-bit 순번).
// Docs/Design/LoggingSystem.ko.md 8.2절의 로그 큐이며 범용으로 쓸 수 있다.
//
// 성질:
// - 용량은 2의 거듭제곱이고 Initialize에서 한 번 할당한다. 그 뒤 push·pop은 할당하지 않는다.
// - 가득 차면 TryPush가 false를 돌려준다(기다리지 않는다).
// - 생산자는 lock-free다(다른 생산자가 멈춰도 자기 칸은 예약·공개할 수 있다).
// - 소비자는 엄밀한 lock-free가 아니다. 칸을 예약한 생산자가 공개하기 전에 멈추면, 소비자는 그
//   칸에서 TryPop이 false가 되어 기다린다. 예약과 공개 사이에는 fill 함수만 있으므로 짧게 쓴다.
// - 순서: 예약 순번(tail)이 전역 순서다. 한 생산자의 push 순서는 pop 순서와 같다.
//
// 메모리 순서:
// - 생산자는 칸 순번을 acquire로 읽어 이전 소비자의 release(칸 비움)를 관찰한 뒤 값을 쓰고, 순번을
//   release로 공개한다. 소비자는 순번을 acquire로 읽어 생산자가 쓴 값을 본다.
// - tail 예약 CAS는 relaxed로 충분하다(칸 데이터의 발행은 칸 순번이 맡는다).
// - head는 소비자만 쓰고, ApproximateSize를 위해 relaxed로 공개한다.
//
// T는 trivially copyable이어야 한다. 스레드 계약: TryPush는 어느 스레드에서나, TryPop·IsEmpty는
// 소비자 한 스레드에서만 부른다. 파괴는 모든 호출이 끝난 뒤 한다.
//
// 사용 예:
//   BoundedMpscQueue<Record> queue;
//   if (!queue.Initialize(allocator, 1024)) { ... }
//   queue.TryPushWith([&](Record& slot) noexcept { slot = record; });
//   queue.TryPopWith([&](Record& slot) noexcept { Consume(slot); });
template <typename T>
class BoundedMpscQueue {
    static_assert(IsTriviallyCopyableV<T>, "BoundedMpscQueue requires a trivially copyable type");

public:
    BoundedMpscQueue() noexcept = default;
    BoundedMpscQueue(const BoundedMpscQueue&) = delete;
    BoundedMpscQueue& operator=(const BoundedMpscQueue&) = delete;

    ~BoundedMpscQueue() noexcept {
        Shutdown();
    }

    // capacity칸을 allocator에서 한 번 할당한다. capacity는 2 이상의 2의 거듭제곱이어야 한다.
    // 실패: 잘못된 capacity·이미 초기화됨은 InvalidArgument, 크기 계산 넘침은 Overflow, 그 밖은
    // allocator 오류. 실패하면 상태를 바꾸지 않는다.
    Result<void> Initialize(Allocator& allocator, usize capacity) noexcept {
        if (cells_ != nullptr || capacity < 2 || (capacity & (capacity - 1)) != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (capacity > MaxSize / sizeof(Cell)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        auto memory = allocator.Allocate(capacity * sizeof(Cell), alignof(Cell));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        cells_ = static_cast<Cell*>(memory.Value());
        allocator_ = &allocator;
        mask_ = capacity - 1;
        for (usize index = 0; index < capacity; ++index) {
            cells_[index].sequence = index;
        }

        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&tail_.value, uint64{0});
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&head_.value, uint64{0});
        return Result<void>::Success();
    }

    // 할당을 돌려준다. 다른 스레드가 쓰지 않을 때만 부른다.
    void Shutdown() noexcept {
        if (cells_ != nullptr) {
            if (!allocator_->Deallocate(cells_)) {
                platform::Fatal("BoundedMpscQueue storage deallocation was refused");
            }

            cells_ = nullptr;
        }
    }

    usize Capacity() const noexcept {
        return cells_ == nullptr ? 0 : mask_ + 1;
    }

    // 칸 하나를 예약하고 fill(T&)로 채운 뒤 공개한다. 가득 찼으면 fill을 부르지 않고 false다.
    // fill은 noexcept이고 짧아야 한다(예약부터 공개까지 소비자가 이 칸에서 기다린다).
    template <typename Fill>
    bool TryPushWith(Fill&& fill) noexcept {
        uint64 position = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&tail_.value);
        for (;;) {
            Cell& cell = cells_[position & mask_];
            const uint64 sequence =
                platform::AtomicLoad<platform::MemoryOrder::Acquire>(&cell.sequence);
            const auto difference = static_cast<int64>(sequence - position);
            if (difference == 0) {
                // 이 칸이 비었다. 예약에 성공하면 이 생산자만 이 칸을 쓴다.
                if (platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Relaxed,
                                                        platform::MemoryOrder::Relaxed>(
                        &tail_.value, position, position + 1)) {
                    fill(cell.value);
                    platform::AtomicStore<platform::MemoryOrder::Release>(&cell.sequence,
                                                                          position + 1);
                    return true;
                }
                // 실패한 CAS는 position을 최신 tail로 바꿨다.
            } else if (difference < 0) {
                // 소비자가 아직 이 칸을 비우지 않았다(한 바퀴 앞). 가득 찼다.
                return false;
            } else {
                // 다른 생산자가 앞서 예약했다. tail을 다시 읽는다.
                position = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&tail_.value);
            }
        }
    }

    bool TryPush(const T& value) noexcept {
        return TryPushWith([&](T& slot) noexcept { slot = value; });
    }

    // 맨 앞 칸이 공개됐으면 consume(T&)를 부르고 칸을 비운 뒤 true다. 비었거나 맨 앞 칸을 예약한
    // 생산자가 아직 공개하지 않았으면 false다. 소비자 한 스레드에서만 부른다.
    template <typename Consume>
    bool TryPopWith(Consume&& consume) noexcept {
        const uint64 position = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&head_.value);
        Cell& cell = cells_[position & mask_];
        const uint64 sequence =
            platform::AtomicLoad<platform::MemoryOrder::Acquire>(&cell.sequence);
        if (static_cast<int64>(sequence - (position + 1)) != 0) {
            return false;
        }

        consume(cell.value);
        // 한 바퀴 뒤 같은 칸을 쓸 생산자가 이 순번을 기다린다.
        platform::AtomicStore<platform::MemoryOrder::Release>(&cell.sequence, position + mask_ + 1);
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&head_.value, position + 1);
        return true;
    }

    bool TryPop(T& value) noexcept {
        return TryPopWith([&](T& slot) noexcept { value = slot; });
    }

    // 예약된 칸이 없는지다(공개 전 칸이 있으면 false). 소비자 스레드에서 부른다.
    bool IsEmpty() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&tail_.value) ==
               platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&head_.value);
    }

    // 예약된 칸 수의 근사값이다(어느 스레드에서나, relaxed). 포화 판정용이다.
    usize ApproximateSize() const noexcept {
        const uint64 tail = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&tail_.value);
        const uint64 head = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&head_.value);
        return tail > head ? static_cast<usize>(tail - head) : 0;
    }

    // 지금까지 예약된 칸의 누적 수다(Flush가 "이 시점까지 넣은 기록"을 표시하는 데 쓴다).
    uint64 PushPosition() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::SequentiallyConsistent>(&tail_.value);
    }

    // 지금까지 꺼낸 칸의 누적 수다.
    uint64 PopPosition() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&head_.value);
    }

private:
    // 칸은 캐시 라인 배수 크기로 둔다(이웃 칸의 생산자끼리 false sharing하지 않게).
    struct alignas(CacheLineSize) Cell {
        uint64 sequence;
        T value;
    };

    // 머리와 꼬리를 서로 다른 캐시 라인에 둔다(생산자의 꼬리 CAS가 소비자의 머리 쓰기와 false
    // sharing하지 않게). 생산자가 늘 읽는 칸 배열 정보는 그 앞의 별도 줄에 둔다.
    struct alignas(CacheLineSize) Counter {
        uint64 value = 0;
    };

    Cell* cells_ = nullptr;
    Allocator* allocator_ = nullptr;
    usize mask_ = 0;
    Counter tail_;
    Counter head_;
};

} // namespace rocks
