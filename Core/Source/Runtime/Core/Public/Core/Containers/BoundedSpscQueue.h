#pragma once

#include <Base/Compiler.h>
#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/StepPolicy.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>

namespace rocks {

// 생산자 하나·소비자 하나의 유한 lock-free 큐다(Docs/Design/Synchronization 12.2절). lane 사이 한
// 방향 전달과 실시간 스레드 입력(오디오 AudioRT 명령 링 — 잠금·할당·대기가 없다)에 쓴다.
//
// 성질:
// - 용량은 2 이상의 2의 거듭제곱이고 Initialize에서 한 번 할당한다. 그 뒤 push·pop은 할당하지
// 않는다.
// - 가득 차면 TryPush가 false, 비었으면 TryPop이 false다(기다리지 않는다).
// - 생산자·소비자 모두 wait-free다(상대가 멈춰도 자기 연산은 유한 단계로 끝난다).
// - head·tail을 각자 cache line에 두고 상대 값을 자기 줄에 캐시한다(가득·빔을 볼 때만 상대 값을
// 다시
//   읽는다).
// - 칸을 재사용한다(회수 없음, 12.3절). ABA가 없다(순번이 64-bit로 단조 증가한다).
//
// 메모리 순서: 생산자는 칸을 쓴 뒤 tail을 release로 올리고, 소비자는 tail을 acquire로 읽은 뒤 칸을
// 읽는다. 소비자는 칸을 다 읽은 뒤 head를 release로 올리고, 생산자는 head를 acquire로 읽은 뒤 그
// 칸을 다시 쓴다.
//
// 스레드 계약: TryPush·TryPushWith·IsFull은 생산자 한 스레드, TryPop·TryPopWith·IsEmpty는 소비자 한
// 스레드에서만 부른다(스레드가 바뀌면 그 사이에 happens-before가 있어야 한다). T는 trivially
// copyable이다. Initialize·Shutdown·파괴는 다른 호출이 없을 때 한다.
//
// 사용 예:
//   BoundedSpscQueue<Command> commands;
//   if (!commands.Initialize(allocator, 256)) { ... }
//   if (!commands.TryPush(command)) { /* 가득 참: 호출자가 정책대로 처리 */ }
//   // 소비자:
//   Command next;
//   while (commands.TryPop(next)) { Apply(next); }
template <typename T, class StepPolicy = NoSteps>
class BoundedSpscQueue {
    static_assert(IsTriviallyCopyableV<T>, "BoundedSpscQueue requires a trivially copyable type");

public:
    BoundedSpscQueue() noexcept = default;
    BoundedSpscQueue(const BoundedSpscQueue&) = delete;
    BoundedSpscQueue& operator=(const BoundedSpscQueue&) = delete;

    ~BoundedSpscQueue() noexcept {
        Shutdown();
    }

    // capacity칸을 allocator에서 한 번 할당한다. 실패: 잘못된 capacity(2 미만·2의 거듭제곱
    // 아님)·이미 초기화됨은 InvalidArgument, 크기 넘침은 Overflow, 그 밖은 allocator 오류. 실패하면
    // 상태를 바꾸지 않는다.
    Result<void> Initialize(Allocator& allocator, usize capacity) noexcept {
        if (slots_ != nullptr || capacity < 2 || (capacity & (capacity - 1)) != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (capacity > MaxSize / sizeof(T)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        auto memory = allocator.Allocate(capacity * sizeof(T), alignof(T));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        slots_ = static_cast<T*>(memory.Value());
        allocator_ = &allocator;
        mask_ = capacity - 1;
        producer_.tail.template Store<platform::MemoryOrder::Relaxed>(0);
        producer_.cachedHead = 0;
        consumer_.head.template Store<platform::MemoryOrder::Relaxed>(0);
        consumer_.cachedTail = 0;
        return Result<void>::Success();
    }

    // 할당을 돌려준다. 다른 스레드가 쓰지 않을 때만 부른다.
    void Shutdown() noexcept {
        if (slots_ != nullptr) {
            if (!allocator_->Deallocate(slots_)) {
                platform::Fatal("BoundedSpscQueue storage deallocation was refused");
            }

            slots_ = nullptr;
        }
    }

    usize Capacity() const noexcept {
        return slots_ == nullptr ? 0 : mask_ + 1;
    }

    // 칸 하나를 fill(T&)로 채워 공개한다. 가득 찼으면 fill을 부르지 않고 false다. 생산자 스레드만
    // 부른다.
    template <typename Fill>
    bool TryPushWith(Fill&& fill) noexcept {
        const uint64 tail = producer_.tail.template Load<platform::MemoryOrder::Relaxed>();
        if (tail - producer_.cachedHead > mask_) {
            producer_.cachedHead = consumer_.head.template Load<platform::MemoryOrder::Acquire>();
            StepPolicy::Step("BoundedSpscQueue.PushReadHead");
            if (tail - producer_.cachedHead > mask_) {
                return false;
            }
        }

        fill(slots_[tail & mask_]);
        StepPolicy::Step("BoundedSpscQueue.PushFilled");
        producer_.tail.template Store<platform::MemoryOrder::Release>(tail + 1);
        return true;
    }

    bool TryPush(const T& value) noexcept {
        return TryPushWith([&](T& slot) noexcept { slot = value; });
    }

    // 맨 앞 칸을 consume(T&)에 넘기고 비운다. 비었으면 consume을 부르지 않고 false다. 소비자
    // 스레드만 부른다.
    template <typename Consume>
    bool TryPopWith(Consume&& consume) noexcept {
        const uint64 head = consumer_.head.template Load<platform::MemoryOrder::Relaxed>();
        if (head == consumer_.cachedTail) {
            consumer_.cachedTail = producer_.tail.template Load<platform::MemoryOrder::Acquire>();
            StepPolicy::Step("BoundedSpscQueue.PopReadTail");
            if (head == consumer_.cachedTail) {
                return false;
            }
        }

        consume(slots_[head & mask_]);
        StepPolicy::Step("BoundedSpscQueue.PopConsumed");
        consumer_.head.template Store<platform::MemoryOrder::Release>(head + 1);
        return true;
    }

    bool TryPop(T& value) noexcept {
        return TryPopWith([&](T& slot) noexcept { value = slot; });
    }

    // 소비자 스레드에서: 꺼낼 칸이 없는지다.
    bool IsEmpty() const noexcept {
        return consumer_.head.template Load<platform::MemoryOrder::Relaxed>() ==
               producer_.tail.template Load<platform::MemoryOrder::Acquire>();
    }

    // 생산자 스레드에서: 넣을 칸이 없는지다.
    bool IsFull() const noexcept {
        return producer_.tail.template Load<platform::MemoryOrder::Relaxed>() -
                   consumer_.head.template Load<platform::MemoryOrder::Acquire>() >
               mask_;
    }

    // 들어 있는 칸 수의 근사값이다(어느 스레드에서나). 포화 판정·통계용이다.
    usize ApproximateSize() const noexcept {
        const uint64 tail = producer_.tail.template Load<platform::MemoryOrder::Relaxed>();
        const uint64 head = consumer_.head.template Load<platform::MemoryOrder::Relaxed>();
        return tail > head ? static_cast<usize>(tail - head) : 0;
    }

private:
    // 생산자가 쓰는 줄: 꼬리와 마지막으로 본 머리.
    struct alignas(CacheLineSize) ProducerLine {
        platform::Atomic<uint64> tail;
        uint64 cachedHead = 0;
    };

    // 소비자가 쓰는 줄: 머리와 마지막으로 본 꼬리.
    struct alignas(CacheLineSize) ConsumerLine {
        platform::Atomic<uint64> head;
        uint64 cachedTail = 0;
    };

    T* slots_ = nullptr;
    Allocator* allocator_ = nullptr;
    usize mask_ = 0;
    ProducerLine producer_;
    ConsumerLine consumer_;
};

} // namespace rocks
