#include <Base/TypeTraits.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Core/Delegate.h>
#include <Core/Memory/SystemAllocator.h>

namespace rocks::test {
namespace delegate_tests_file {

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
static_assert(!IsConstructibleV<Delegate<void()>, const Delegate<void()>&>);
static_assert(IsConstructibleV<Delegate<void()>, Delegate<void()>&&>);
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

namespace {

// 소멸될 때 같은 MulticastDelegate를 다시 호출한다(재검토 재현: UI 해제 중 구독 해제 패턴).
struct RemoveOnDestroy {
    MulticastDelegate<void()>* event;
    DelegateHandle* other;
    int* removed;

    RemoveOnDestroy(MulticastDelegate<void()>* target, DelegateHandle* handle, int* count) noexcept
        : event(target), other(handle), removed(count) {
    }

    RemoveOnDestroy(RemoveOnDestroy&& source) noexcept
        : event(source.event), other(source.other), removed(source.removed) {
        source.event = nullptr;
    }

    ~RemoveOnDestroy() noexcept {
        if (event != nullptr) {
            *removed += event->Remove(*other) ? 1 : 0;
            (void)event->IsBound();
        }
    }
};

struct OwnerObject {
    int value = 0;
};

} // namespace

TEST(MulticastDelegate_TargetDestructorMayReenter) {
    SystemAllocator allocator;
    {
        // Remove(A) 중 A의 캡처 소멸자가 B를 제거한다.
        MulticastDelegate<void()> event(allocator);
        DelegateHandle a;
        DelegateHandle b;
        int removed = 0;
        auto first =
            event.TryAddLambda([guard = RemoveOnDestroy(&event, &b, &removed)]() noexcept {});
        auto second = event.TryAddLambda([]() noexcept {});
        REQUIRE(first && second);
        a = first.Value();
        b = second.Value();
        CHECK(event.Remove(a));
        CHECK(removed == 1 && !event.IsBound());

        // Clear 중 소멸자가 아직 남은 다른 항목을 제거한다.
        DelegateHandle c;
        removed = 0;
        auto x = event.TryAddLambda([guard = RemoveOnDestroy(&event, &c, &removed)]() noexcept {});
        auto y = event.TryAddLambda([]() noexcept {});
        auto z = event.TryAddLambda([]() noexcept {});
        REQUIRE(x && y && z);
        c = z.Value();
        event.Clear();
        CHECK(!event.IsBound());
        event.Broadcast();

        // 약한 대상이 사라진 항목의 정리 중 자기 handle을 제거한다(정리는 Broadcast 끝에서
        // 일어난다).
        DelegateHandle self;
        removed = 0;
        auto owner = SharedPtr<OwnerObject>::TryMake(allocator);
        REQUIRE(owner);
        auto weak = event.TryAddWeakLambda(
            owner.Value(), [guard = RemoveOnDestroy(&event, &self, &removed)]() noexcept {});
        REQUIRE(weak);
        self = weak.Value();
        owner.Value().Reset();
        event.Broadcast();
        CHECK(removed == 0 && !event.IsBound()); // 정리 중이라 이미 목록에서 빠진 뒤다
        int calls = 0;
        auto after = event.TryAddLambda([&calls]() noexcept { ++calls; });
        REQUIRE(after);
        event.Broadcast();
        CHECK(calls == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

// 재진입 시험의 문맥이다(guard는 이 포인터 하나만 쥐어 약한 lambda가 inline 저장에 들어간다).
struct Reentry {
    MulticastDelegate<void(int)>* event;
    DelegateHandle victim;
    int order[8];
    int written;
};

// 소멸될 때 같은 MulticastDelegate에서 다른 항목을 제거하고 새 항목을 더한다(정리 중 재진입).
struct ReenterOnDestroy {
    Reentry* context;

    explicit ReenterOnDestroy(Reentry* target) noexcept : context(target) {
    }

    ReenterOnDestroy(ReenterOnDestroy&& source) noexcept : context(source.context) {
        source.context = nullptr;
    }

    ~ReenterOnDestroy() noexcept {
        if (context != nullptr) {
            Reentry* state = context;
            CHECK(state->event->Remove(state->victim));
            CHECK(state->event->TryAddLambda(
                [state](int) noexcept { state->order[state->written++] = 9; }));
        }
    }
};

} // namespace

// 정리(SL 5.5절 C3)는 살아 있는 항목의 등록 순서를 지키고, CompactDead는 해제한 수를 돌려준다.
TEST(MulticastDelegate_CompactKeepsOrder) {
    SystemAllocator allocator;
    {
        MulticastDelegate<void(int)> event(allocator);
        constexpr int Count = 6;
        int order[Count + 2] = {};
        int written = 0;
        SharedPtr<Counter> owners[Count];
        for (int index = 0; index < Count; ++index) {
            auto owner = SharedPtr<Counter>::TryMake(allocator);
            REQUIRE(owner);
            owners[index] = owner.Value();
            REQUIRE(event.TryAddWeakLambda(owners[index], [&order, &written, index](int) noexcept {
                order[written++] = index;
            }));
        }

        // 1·3·4번 대상이 사라진다(정리 전이라 배열에는 남아 있다).
        owners[1].Reset();
        owners[3].Reset();
        owners[4].Reset();
        MulticastDelegateStats before = event.Stats();
        CHECK(before.entries == Count && before.bound == 3);
        CHECK(event.CompactDead() == 3);
        MulticastDelegateStats after = event.Stats();
        CHECK(after.entries == 3 && after.bound == 3 && after.liveAtLastCompact == 3);
        CHECK(event.CompactDead() == 0);
        event.Broadcast(0);
        CHECK(written == 3 && order[0] == 0 && order[1] == 2 && order[2] == 5);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// Broadcast 없이 TryAdd만 계속해도 죽은 항목이 상한(직전 정리 뒤 남은 수의 두 배 + 여유분)을
// 넘지 않는다(SL 5.5절 C2).
TEST(MulticastDelegate_TryAddBoundsDeadEntries) {
    SystemAllocator allocator;
    {
        MulticastDelegate<void()> event(allocator);
        auto keeper = SharedPtr<Counter>::TryMake(allocator);
        REQUIRE(keeper);
        REQUIRE(event.TryAddWeakLambda(keeper.Value(), []() noexcept {}));
        constexpr int Adds = 200;
        for (int index = 0; index < Adds; ++index) {
            auto owner = SharedPtr<Counter>::TryMake(allocator);
            REQUIRE(owner);
            REQUIRE(event.TryAddWeakLambda(owner.Value(), []() noexcept {}));
            owner.Value().Reset(); // 대상이 곧바로 사라진다
            const MulticastDelegateStats stats = event.Stats();
            CHECK(stats.entries <= stats.liveAtLastCompact * 2 + MulticastCompactSlack);
            CHECK(stats.bound == 1);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 정리 중 해제되는 대상의 소멸자가 같은 delegate에서 다른 항목을 제거하고 새 항목을 더해도 순서와
// 해제가 맞다(재진입한 TryAdd는 죽은 묶음 뒤에 붙고, 재진입한 제거는 한 번 더 도는 정리가 거둔다).
TEST(MulticastDelegate_CompactAllowsReentrantTargets) {
    SystemAllocator allocator;
    {
        MulticastDelegate<void(int)> event(allocator);
        Reentry state{&event, {}, {}, 0};
        auto owner = SharedPtr<Counter>::TryMake(allocator);
        REQUIRE(owner);
        REQUIRE(event.TryAddLambda([&state](int) noexcept { state.order[state.written++] = 1; }));
        REQUIRE(event.TryAddWeakLambda(owner.Value(),
                                       [guard = ReenterOnDestroy(&state)](int) noexcept {}));
        auto removed =
            event.TryAddLambda([&state](int) noexcept { state.order[state.written++] = 3; });
        REQUIRE(removed);
        state.victim = removed.Value();
        REQUIRE(event.TryAddLambda([&state](int) noexcept { state.order[state.written++] = 4; }));

        owner.Value().Reset();
        CHECK(event.CompactDead() == 2); // 약한 대상 항목 + 재진입으로 제거된 항목
        const MulticastDelegateStats stats = event.Stats();
        CHECK(stats.entries == 3 && stats.bound == 3);
        event.Broadcast(0);
        CHECK(state.written == 3 && state.order[0] == 1 && state.order[1] == 4 &&
              state.order[2] == 9);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

// 생존을 밖에서 판정하는 lambda 대상(SL 5.5절 C1): 호출 대상의 IsAlive가 false면 IsBound·
// ExecuteIfBound가 그것을 따르고 MulticastDelegate는 부르지 않으며 CompactDead가 한 pass로
// 해제한다. inline·heap 저장 모두.
struct LivenessTarget {
    const bool* alive;
    int* calls;

    void operator()(int value) noexcept {
        *calls += value;
    }

    bool IsAlive() const noexcept {
        return *alive;
    }
};

struct LargeLivenessTarget {
    const bool* alive;
    int* calls;
    unsigned char padding[DelegateInlineBytes];

    void operator()(int value) noexcept {
        *calls += value;
    }

    bool IsAlive() const noexcept {
        return *alive;
    }
};

static_assert(detail::DelegateCallableHasLiveness<LivenessTarget>);
static_assert(!detail::DelegateCallableHasLiveness<decltype([](int) noexcept {})>);

TEST(Delegate_ExternalLiveness) {
    SystemAllocator allocator;
    {
        bool alive = true;
        int calls = 0;
        auto single = Delegate<void(int)>::CreateLambda(LivenessTarget{&alive, &calls});
        auto heap = Delegate<void(int)>::TryCreateLambda(allocator,
                                                         LargeLivenessTarget{&alive, &calls, {}});
        REQUIRE(heap);
        CHECK(single.IsBound() && heap.Value().IsBound());
        CHECK(single.ExecuteIfBound(1) && heap.Value().ExecuteIfBound(2));
        CHECK(calls == 3);

        MulticastDelegate<void(int)> event(allocator);
        REQUIRE(event.TryAddLambda(LivenessTarget{&alive, &calls}));
        REQUIRE(event.TryAdd(Move(heap.Value())));
        int others = 0;
        REQUIRE(event.TryAddLambda([&](int value) noexcept { others += value; }));
        event.Broadcast(10);
        CHECK(calls == 23 && others == 10);

        alive = false;
        CHECK(!single.IsBound() && !single.ExecuteIfBound(1));
        CHECK(event.Stats().bound == 1);
        event.Broadcast(10);
        CHECK(calls == 23 && others == 20);
        CHECK(event.CompactDead() == 0); // Broadcast 끝의 정리가 이미 거뒀다
        CHECK(event.Stats().entries == 1);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
} // namespace delegate_tests_file

} // namespace rocks::test
