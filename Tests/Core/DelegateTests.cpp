#include "Tests/Support/NFailAllocator.h"
#include "Tests/Support/Test.h"
#include <Core/Delegate.h>
#include <Core/Memory/SystemAllocator.h>

namespace rocks::test {
namespace {

int staticCalls = 0;

void CountStatic(int amount) noexcept {
    staticCalls += amount;
}

int Twice(int value) noexcept {
    return value * 2;
}

struct Counter {
    int total = 0;

    void Add(int amount) noexcept {
        total += amount;
    }

    int Read() const noexcept {
        return total;
    }
};

struct Tracked {
    static inline int alive = 0;
    int value = 0;

    explicit Tracked(int number) noexcept : value(number) {
        ++alive;
    }

    Tracked(const Tracked& other) noexcept : value(other.value) {
        ++alive;
    }

    Tracked(Tracked&& other) noexcept : value(other.value) {
        ++alive;
    }

    ~Tracked() noexcept {
        --alive;
    }
};

struct Large {
    unsigned char bytes[128] = {};
};

// 컴파일 시간 계약: 이동 전용, 큰 lambda는 inline으로 만들 수 없다.
static_assert(!__is_constructible(Delegate<void()>, const Delegate<void()>&));
static_assert(__is_constructible(Delegate<void()>, Delegate<void()>&&));
static_assert(sizeof(Delegate<void()>) <= DelegateInlineBytes + 24);

} // namespace

TEST(Delegate_BindKindsAndExecute) {
    Delegate<void(int)> empty;
    CHECK(!empty.IsBound() && !empty.ExecuteIfBound(1) && empty.BoundObject() == nullptr);

    staticCalls = 0;
    auto staticDelegate = Delegate<void(int)>::CreateStatic(&CountStatic);
    CHECK(staticDelegate.IsBound() && staticDelegate.ExecuteIfBound(3));
    staticDelegate.Execute(4);
    CHECK(staticCalls == 7);
    CHECK(Delegate<int(int)>::CreateStatic(&Twice).Execute(21) == 42);
    CHECK(!Delegate<void(int)>::CreateStatic(nullptr).IsBound());

    Counter counter;
    auto raw = Delegate<void(int)>::CreateRaw(&counter, &Counter::Add);
    raw.Execute(5);
    CHECK(counter.total == 5 && raw.BoundObject() == &counter);
    const Counter& constCounter = counter;
    CHECK(Delegate<int()>::CreateRaw(&constCounter, &Counter::Read).Execute() == 5);
    CHECK(!Delegate<void(int)>::CreateRaw(static_cast<Counter*>(nullptr), &Counter::Add).IsBound());

    // mutable lambda의 상태는 실행 사이에 유지된다.
    int observed = 0;
    auto lambda = Delegate<int()>::CreateLambda([&observed, calls = 0]() mutable noexcept {
        ++calls;
        observed = calls;
        return calls;
    });
    CHECK(lambda.Execute() == 1 && lambda.Execute() == 2 && observed == 2);

    // 참조 인자는 그대로 전달된다.
    auto increment = Delegate<void(int&)>::CreateLambda([](int& value) noexcept { ++value; });
    int value = 1;
    increment.Execute(value);
    CHECK(value == 2);
}

TEST(Delegate_WeakTargetsExpire) {
    SystemAllocator allocator;
    auto created = SharedPtr<Counter>::TryMake(allocator);
    REQUIRE(created);
    SharedPtr<Counter> counter = created.Value();
    created.Value().Reset();
    auto weak = Delegate<void(int)>::CreateWeak(counter, &Counter::Add);
    auto weakRead = Delegate<int()>::CreateWeak(counter, &Counter::Read);
    int lambdaCalls = 0;
    auto weakLambda =
        Delegate<void()>::CreateWeakLambda(counter, [&lambdaCalls]() noexcept { ++lambdaCalls; });
    CHECK(weak.IsBound() && weak.BoundObject() == counter.Get());
    CHECK(counter.UseCount() == 1); // 약하게만 묶는다
    weak.Execute(3);
    CHECK(weakRead.Execute() == 3 && weakLambda.ExecuteIfBound() && lambdaCalls == 1);
    counter.Reset(); // 대상 소멸
    CHECK(!weak.IsBound() && !weakRead.IsBound() && !weakLambda.IsBound());
    CHECK(!weak.ExecuteIfBound(1) && !weakLambda.ExecuteIfBound() && lambdaCalls == 1);
    CHECK(weak.BoundObject() != nullptr); // 주소는 식별용으로 남는다
    weak.Unbind();
    weakRead.Unbind();
    weakLambda.Unbind();
    CHECK(allocator.Stats().liveAllocations == 0);
    CHECK(!Delegate<void(int)>::CreateWeak(SharedPtr<Counter>(), &Counter::Add).IsBound());
}

TEST(Delegate_LifetimeMoveAndHeapLambda) {
    SystemAllocator system;
    Tracked::alive = 0;
    {
        auto first = Delegate<int()>::CreateLambda(
            [tracked = Tracked(7)]() noexcept { return tracked.value; });
        CHECK(Tracked::alive == 1);
        Delegate<int()> second = Move(first);
        CHECK(!first.IsBound() && second.Execute() == 7 && Tracked::alive == 1);
        second = Delegate<int()>::CreateLambda([]() noexcept { return 1; });
        CHECK(Tracked::alive == 0 && second.Execute() == 1);
        second = Move(second); // 자기 이동은 무시
        CHECK(second.Execute() == 1);

        // inline 용량을 넘는 lambda는 allocator에 담는다.
        Large large;
        large.bytes[5] = 9;
        auto heap =
            Delegate<int()>::TryCreateLambda(system, [large, tracked = Tracked(2)]() noexcept {
                return large.bytes[5] + tracked.value;
            });
        REQUIRE(heap);
        CHECK(heap.Value().Execute() == 11 && system.Stats().liveAllocations == 1);
        Delegate<int()> moved = Move(heap.Value());
        CHECK(moved.Execute() == 11 && system.Stats().liveAllocations == 1);
        moved.Unbind();
        CHECK(system.Stats().liveAllocations == 0 && Tracked::alive == 0);

        NFailAllocator failing(system);
        failing.FailOnAttempt(1);
        auto failed = Delegate<int()>::TryCreateLambda(
            failing, [large]() noexcept { return static_cast<int>(large.bytes[0]); });
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    }
    CHECK(Tracked::alive == 0 && system.Stats().liveAllocations == 0);
}

TEST(Delegate_UnbindDuringOwnExecutionIsDeferred) {
    Tracked::alive = 0;
    Delegate<void()> oneShot;
    int calls = 0;
    int aliveDuringCall = -1;
    oneShot = Delegate<void()>::CreateLambda([&, tracked = Tracked(1)]() noexcept {
        ++calls;
        oneShot.Unbind(); // 자기 해제: 호출이 끝난 뒤 실제로 해제된다
        aliveDuringCall = Tracked::alive;
        CHECK(!oneShot.IsBound());
    });
    CHECK(Tracked::alive == 1);
    CHECK(oneShot.ExecuteIfBound());
    CHECK(calls == 1 && aliveDuringCall == 1 && Tracked::alive == 0 && !oneShot.IsBound());
    CHECK(!oneShot.ExecuteIfBound() && calls == 1);
}

TEST(MulticastDelegate_AddRemoveAndBroadcastOrder) {
    SystemAllocator allocator;
    {
        MulticastDelegate<void(int)> event(allocator);
        CHECK(!event.IsBound());
        int order[8] = {};
        int count = 0;
        auto first = event.TryAddLambda([&](int value) noexcept { order[count++] = value; });
        auto second = event.TryAddLambda([&](int value) noexcept { order[count++] = value * 10; });
        Counter counter;
        auto raw = event.TryAddRaw(&counter, &Counter::Add);
        staticCalls = 0;
        auto staticHandle = event.TryAddStatic(&CountStatic);
        REQUIRE(first && second && raw && staticHandle);
        CHECK(first.Value() != second.Value() && first.Value().IsValid());
        event.Broadcast(2);
        CHECK(count == 2 && order[0] == 2 && order[1] == 20 && counter.total == 2 &&
              staticCalls == 2);
        CHECK(event.Remove(second.Value()) && !event.Remove(second.Value()));
        CHECK(!event.Remove(DelegateHandle()));
        event.Broadcast(3);
        CHECK(count == 3 && order[2] == 3 && counter.total == 5);
        CHECK(event.RemoveAll(&counter) == 1);
        event.Broadcast(1);
        CHECK(counter.total == 5 && staticCalls == 6 && count == 4 && order[3] == 1);
        // 바인딩 없는 delegate는 등록하지 않는다.
        CHECK(event.TryAdd(Delegate<void(int)>()).Error() == ErrorCode::InvalidArgument);
        event.Clear();
        CHECK(!event.IsBound());
        event.Broadcast(9);
        CHECK(count == 4); // Clear 뒤에는 아무도 호출되지 않는다
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(MulticastDelegate_ModificationDuringBroadcast) {
    SystemAllocator allocator;
    {
        MulticastDelegate<void()> event(allocator);
        int a = 0;
        int b = 0;
        int added = 0;
        int nested = 0;
        DelegateHandle selfHandle;
        DelegateHandle bHandle;
        // 첫 대상: 자기 자신과 b를 제거하고 새 대상을 추가한다.
        auto self = event.TryAddLambda([&]() noexcept {
            ++a;
            CHECK(event.Remove(selfHandle));
            CHECK(event.Remove(bHandle));
            CHECK(event.TryAddLambda([&]() noexcept { ++added; }));
        });
        REQUIRE(self);
        selfHandle = self.Value();
        auto other = event.TryAddLambda([&]() noexcept { ++b; });
        REQUIRE(other);
        bHandle = other.Value();
        event.Broadcast();
        // 제거된 b는 이후 순서에서 호출되지 않고, 추가된 대상은 다음 Broadcast부터 호출된다.
        CHECK(a == 1 && b == 0 && added == 0);
        event.Broadcast();
        CHECK(a == 1 && b == 0 && added == 1);

        // 중첩 Broadcast와 Broadcast 중 Clear.
        MulticastDelegate<void(int)> chain(allocator);
        auto recurse = chain.TryAddLambda([&](int depth) noexcept {
            ++nested;
            if (depth > 0) {
                chain.Broadcast(depth - 1);
            } else {
                chain.Clear();
            }
        });
        REQUIRE(recurse);
        chain.Broadcast(3);
        CHECK(nested == 4 && !chain.IsBound());
        chain.Broadcast(3);
        CHECK(nested == 4);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(MulticastDelegate_WeakTargetsAndAllocationFailure) {
    SystemAllocator system;
    {
        auto created = SharedPtr<Counter>::TryMake(system);
        REQUIRE(created);
        SharedPtr<Counter> counter = created.Value();
        created.Value().Reset();
        MulticastDelegate<void(int)> event(system);
        auto weak = event.TryAddWeak(counter, &Counter::Add);
        int lambdaCalls = 0;
        auto weakLambda = event.TryAddWeakLambda(counter, [&](int) noexcept { ++lambdaCalls; });
        REQUIRE(weak && weakLambda);
        event.Broadcast(4);
        CHECK(counter->total == 4 && lambdaCalls == 1 && event.IsBound());
        counter.Reset();
        CHECK(!event.IsBound());
        event.Broadcast(4); // 사라진 대상은 건너뛰고 정리된다
        CHECK(lambdaCalls == 1 && !event.Remove(weak.Value()));

        NFailAllocator failing(system);
        MulticastDelegate<void()> limited(failing);
        failing.FailOnAttempt(1);
        auto failedReserve = limited.TryAddLambda([]() noexcept {});
        CHECK(!failedReserve && failedReserve.Error() == ErrorCode::OutOfMemory);
        failing.FailOnAttempt(2); // 배열 확보 후 항목 할당 실패
        auto failedEntry = limited.TryAddLambda([]() noexcept {});
        CHECK(!failedEntry && failedEntry.Error() == ErrorCode::OutOfMemory);
        CHECK(!limited.IsBound());
        auto ok = limited.TryAddLambda([]() noexcept {});
        CHECK(ok && limited.IsBound());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

} // namespace rocks::test
