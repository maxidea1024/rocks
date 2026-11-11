#include "Tests/Support/Test.h"
#include "Tests/Support/NFailAllocator.h"
#include <Core/Memory/SharedPtr.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Thread.h>

namespace rocks::test {
namespace {

struct Counted {
    static inline int alive = 0;
    static inline int destroyed = 0;
    int value;

    explicit Counted(int number) noexcept : value(number) {
        ++alive;
    }

    ~Counted() noexcept {
        --alive;
        ++destroyed;
    }
};

// 가상 소멸자가 없는 기반이다. 제어 블록이 원래 타입 소멸 함수를 보관하므로 Derived가 소멸되어야
// 한다.
struct PlainBase {
    int baseValue = 5;
};

struct Padding {
    double padding[3] = {};
};

struct DerivedFromPlain : Padding, PlainBase {
    static inline int destroyed = 0;

    ~DerivedFromPlain() noexcept {
        ++destroyed;
    }
};

// 자기 자신을 가리키는 WeakPtr를 멤버로 가진다. 마지막 strong 해제 중 weak 해제가 일어나도
// 저장소가 객체 소멸 완료 후에만 반환되어야 한다.
struct SelfObserver {
    WeakPtr<SelfObserver> self;
    int value = 9;
};

struct Node : EnableSharedFromThis<Node> {
    int value = 4;
    bool linkedInConstructor = true;

    Node() noexcept {
        // 생성자 안에서는 아직 제어 블록이 연결되지 않았다.
        linkedInConstructor = static_cast<bool>(SharedFromThis());
    }
};

struct StressContext {
    SharedPtr<Counted>* shared;
    WeakPtr<Counted>* weak;
    int successfulLocks = 0;
    int failures = 0;
};

void StressEntry(void* raw) noexcept {
    auto* context = static_cast<StressContext*>(raw);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        SharedPtr<Counted> copy = *context->shared; // 다른 인스턴스 복사(공유 읽기)
        WeakPtr<Counted> observer = copy;
        SharedPtr<Counted> locked = context->weak->Lock();
        if (locked) {
            ++context->successfulLocks;
            if (locked->value != 77) {
                ++context->failures;
            }
        }
        if (!copy || copy->value != 77) {
            ++context->failures;
        }
    }
}

struct RaceContext {
    WeakPtr<Counted>* weak;
    platform::ManualResetEvent* start;
    int* aliveSignals; // 원자적으로 갱신하는 공유 계수
    int observedAlive = 0;
    int observedDead = 0;
    int failures = 0;
};

// 각 작업자는 Lock 성공을 한 번 이상 알리고, 만료를 관찰할 때까지(최대 반복 수 이내) 계속 Lock한다.
// 주 스레드는 모든 작업자의 성공 신호를 본 뒤 마지막 해제를 하므로 Lock과 해제가 실제로 겹친다.
void RaceEntry(void* raw) noexcept {
    auto* context = static_cast<RaceContext*>(raw);
    context->start->Wait();
    bool signaled = false;
    for (int iteration = 0; iteration < 5000000; ++iteration) {
        SharedPtr<Counted> locked = context->weak->Lock();
        if (locked) {
            ++context->observedAlive;
            // Lock 성공 뒤에는 소멸이 끝난 객체를 보면 안 된다.
            if (locked->value != 13) {
                ++context->failures;
            }
            if (!signaled) {
                __atomic_fetch_add(context->aliveSignals, 1, __ATOMIC_ACQ_REL);
                signaled = true;
            }
        } else {
            ++context->observedDead;
            if (!context->weak->IsExpired()) {
                ++context->failures; // 한번 만료되면 다시 살아나지 않는다.
            }
            if (context->observedDead > 100) {
                return;
            }
        }
    }
}

} // namespace

TEST(SharedPtr_CountsAndLifetime) {
    SystemAllocator allocator;
    Counted::alive = 0;
    Counted::destroyed = 0;
    {
        auto created = SharedPtr<Counted>::TryMake(allocator, 3);
        REQUIRE(created);
        SharedPtr<Counted> first = created.Value();
        CHECK(first.UseCount() == 2 && first->value == 3);
        created.Value().Reset();
        CHECK(first.UseCount() == 1);
        SharedPtr<Counted> second = first;
        CHECK(first.UseCount() == 2 && first == second);
        SharedPtr<Counted> third = Move(second);
        CHECK(!second && second == nullptr && third.UseCount() == 2);
        CHECK(allocator.Stats().liveAllocations == 1); // 객체와 제어 블록은 한 번의 할당이다.
        first.Reset();
        CHECK(Counted::alive == 1 && third.UseCount() == 1);
        SharedPtr<Counted>& alias = third;
        third = alias; // 자기 대입
        CHECK(third.UseCount() == 1 && Counted::alive == 1);
    }
    CHECK(Counted::alive == 0 && Counted::destroyed == 1);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SharedPtr_WeakExpiresAndHoldsStorage) {
    SystemAllocator allocator;
    Counted::alive = 0;
    WeakPtr<Counted> weak;
    CHECK(weak.IsExpired() && !weak.Lock());
    {
        auto created = SharedPtr<Counted>::TryMake(allocator, 8);
        REQUIRE(created);
        weak = created.Value();
        CHECK(!weak.IsExpired());
        SharedPtr<Counted> locked = weak.Lock();
        CHECK(locked && locked->value == 8 && locked.UseCount() == 2);
    }
    // 객체는 소멸했지만 WeakPtr가 남아 있어 저장소는 아직 반환되지 않는다.
    CHECK(Counted::alive == 0 && weak.IsExpired() && !weak.Lock());
    CHECK(allocator.Stats().liveAllocations == 1);
    WeakPtr<Counted> copy = weak;
    weak.Reset();
    CHECK(allocator.Stats().liveAllocations == 1);
    copy = WeakPtr<Counted>();
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SharedPtr_DerivedToBaseDestroysDerived) {
    SystemAllocator allocator;
    DerivedFromPlain::destroyed = 0;
    {
        auto created = SharedPtr<DerivedFromPlain>::TryMake(allocator);
        REQUIRE(created);
        SharedPtr<PlainBase> base = created.Value();
        CHECK(static_cast<void*>(base.Get()) != static_cast<void*>(created.Value().Get()));
        WeakPtr<PlainBase> weakBase = created.Value();
        created.Value().Reset();
        CHECK(base->baseValue == 5 && weakBase.Lock().Get() == base.Get());
        SharedPtr<PlainBase> movedBase = SharedPtr<PlainBase>(SharedPtr<DerivedFromPlain>());
        CHECK(!movedBase);
    }
    CHECK(DerivedFromPlain::destroyed == 1);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SharedPtr_SelfWeakReferenceReleasesAfterDestruction) {
    SystemAllocator allocator;
    {
        auto created = SharedPtr<SelfObserver>::TryMake(allocator);
        REQUIRE(created);
        created.Value()->self = created.Value();
        CHECK(created.Value()->self.Lock()->value == 9);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SharedPtr_AllocationFailurePropagates) {
    SystemAllocator system;
    NFailAllocator allocator(system);
    allocator.FailOnAttempt(1);
    auto failed = SharedPtr<Counted>::TryMake(allocator, 1);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(SharedPtr_ConcurrentCopyAndLockAreBalanced) {
    SystemAllocator allocator;
    Counted::alive = 0;
    {
        auto created = SharedPtr<Counted>::TryMake(allocator, 77);
        REQUIRE(created);
        SharedPtr<Counted> shared = created.Value();
        WeakPtr<Counted> weak = shared;
        StressContext contexts[4];
        platform::Thread threads[4];
        for (int index = 0; index < 4; ++index) {
            contexts[index].shared = &shared;
            contexts[index].weak = &weak;
            REQUIRE(threads[index].Start(StressEntry, &contexts[index]) ==
                    platform::ThreadStatus::Success);
        }
        for (auto& thread : threads) {
            REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        }
        for (const auto& context : contexts) {
            CHECK(context.failures == 0 && context.successfulLocks == 20000);
        }
        // 모든 임시 소유권이 반환되어 두 원본만 남는다.
        CHECK(shared.UseCount() == 2);
    }
    CHECK(Counted::alive == 0 && allocator.Stats().liveAllocations == 0);
}

TEST(SharedPtr_LockRacesWithLastRelease) {
    SystemAllocator allocator;
    for (int round = 0; round < 20; ++round) {
        Counted::alive = 0;
        Counted::destroyed = 0;
        auto created = SharedPtr<Counted>::TryMake(allocator, 13);
        REQUIRE(created);
        SharedPtr<Counted> owner = Move(created.Value());
        WeakPtr<Counted> weak = owner;
        platform::ManualResetEvent start;
        int aliveSignals = 0;
        RaceContext contexts[3];
        platform::Thread threads[3];
        for (int index = 0; index < 3; ++index) {
            contexts[index].weak = &weak;
            contexts[index].start = &start;
            contexts[index].aliveSignals = &aliveSignals;
            REQUIRE(threads[index].Start(RaceEntry, &contexts[index]) ==
                    platform::ThreadStatus::Success);
        }
        start.Signal();
        // 모든 작업자가 Lock에 성공한 뒤 해제해 경합 구간을 보장한다.
        while (__atomic_load_n(&aliveSignals, __ATOMIC_ACQUIRE) < 3) {
            platform::YieldThread();
        }
        owner.Reset(); // Lock 중인 스레드와 마지막 해제가 경합한다.
        for (auto& thread : threads) {
            REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        }
        for (const auto& context : contexts) {
            CHECK(context.failures == 0);
            CHECK(context.observedAlive > 0 && context.observedDead > 0);
        }
        CHECK(Counted::alive == 0 && Counted::destroyed == 1);
        CHECK(weak.IsExpired());
        weak.Reset();
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(SharedPtr_EnableSharedFromThis) {
    SystemAllocator allocator;
    {
        auto created = SharedPtr<Node>::TryMake(allocator);
        REQUIRE(created);
        SharedPtr<Node> node = created.Value();
        CHECK(!node->linkedInConstructor);
        SharedPtr<Node> self = node->SharedFromThis();
        CHECK(self == node && node.UseCount() == 3);
        WeakPtr<Node> weakSelf = node->WeakFromThis();
        CHECK(weakSelf.Lock() == node);
        // SharedPtr로 관리되지 않는 객체는 빈 포인터를 반환한다.
        Node local;
        CHECK(!local.SharedFromThis() && local.WeakFromThis().IsExpired());
        // 복사된 객체는 원본의 소유권 연결을 이어받지 않는다.
        Node copied = *node;
        CHECK(!copied.SharedFromThis());
    }
    // 객체가 보유한 weak 토큰이 순환 소유를 만들지 않아 저장소가 반환된다.
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct SharedNode {
    SharedPtr<SharedNode> next;
    int value;

    explicit SharedNode(int number) noexcept : value(number) {
    }
};

} // namespace

TEST(SharedPtr_AssignFromOwnedMemberKeepsSourceAlive) {
    SystemAllocator allocator;
    auto first = SharedPtr<SharedNode>::TryMake(allocator, 1);
    auto second = SharedPtr<SharedNode>::TryMake(allocator, 2);
    REQUIRE(first && second);
    SharedPtr<SharedNode> head = Move(first.Value());
    head->next = Move(second.Value());
    // 복사 원본이 마지막 참조로 해제될 객체의 멤버다.
    head = head->next;
    CHECK(head->value == 2 && head.UseCount() == 1);
    CHECK(allocator.Stats().liveAllocations == 1);
    head = Move(head->next); // 빈 멤버로 이동 대입
    CHECK(!head && allocator.Stats().liveAllocations == 0);
}

TEST(SharedRef_NeverEmptyAndSharesOwnership) {
    SystemAllocator allocator;
    Counted::alive = 0;
    Counted::destroyed = 0;
    {
        auto created = SharedRef<Counted>::TryMake(allocator, 7);
        REQUIRE(created);
        SharedRef<Counted> first = created.Value();
        CHECK(first->value == 7 && (*first).value == 7 && first.Get().value == 7);
        CHECK(first.UseCount() == 2);
        // 이동은 복사다: 원본도 여전히 같은 객체를 가리킨다.
        SharedRef<Counted> moved = Move(first);
        CHECK(first.UseCount() == 3 && moved == first && first->value == 7);
        // SharedPtr·WeakPtr 상호 변환.
        SharedPtr<Counted> pointer = moved;
        CHECK(pointer && pointer == moved && moved == pointer);
        WeakPtr<Counted> weak = moved;
        CHECK(!weak.IsExpired());
        auto back = SharedRef<Counted>::TryFrom(pointer);
        REQUIRE(back);
        CHECK(back.Value() == moved);
        auto viaPointer = pointer.ToSharedRef();
        CHECK(viaPointer == moved);
        CHECK(&moved.AsSharedPtr() != &pointer && moved.AsSharedPtr() == pointer);
        // 대입(자기 대입 포함)과 다른 객체로의 재지정.
        auto other = SharedRef<Counted>::TryMake(allocator, 9);
        REQUIRE(other);
        SharedRef<Counted> target = other.Value();
        SharedRef<Counted>& alias = target;
        target = alias;
        CHECK(target->value == 9);
        target = moved;
        CHECK(target->value == 7 && Counted::alive == 2);
        other.Value() = moved; // 9를 가진 마지막 소유가 사라진다.
        CHECK(Counted::alive == 1 && Counted::destroyed == 1);
        created.Value() = moved;
        pointer.Reset();
        back.Value() = moved;
        CHECK(!weak.IsExpired() && Counted::alive == 1);
    }
    CHECK(Counted::alive == 0 && Counted::destroyed == 2);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(SharedRef_FailuresAndDerivedToBase) {
    SystemAllocator system;
    // 빈 SharedPtr는 SharedRef가 될 수 없다.
    SharedPtr<Counted> empty;
    auto fromEmpty = SharedRef<Counted>::TryFrom(empty);
    CHECK(!fromEmpty && fromEmpty.Error() == ErrorCode::InvalidArgument);
    // 할당 실패는 그대로 전파한다.
    NFailAllocator failing(system);
    failing.FailOnAttempt(1);
    auto failed = SharedRef<Counted>::TryMake(failing, 1);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(system.Stats().liveAllocations == 0);
    // 파생 → 기반 변환은 원래 타입의 소멸을 유지한다.
    DerivedFromPlain::destroyed = 0;
    {
        auto derived = SharedRef<DerivedFromPlain>::TryMake(system);
        REQUIRE(derived);
        SharedRef<PlainBase> base = derived.Value();
        CHECK(base->baseValue == 5 && base.UseCount() == 2);
        auto fromDerivedPointer = SharedRef<PlainBase>::TryFrom(derived.Value().AsSharedPtr());
        REQUIRE(fromDerivedPointer);
        CHECK(fromDerivedPointer.Value() == base);
    }
    CHECK(DerivedFromPlain::destroyed == 1);
    CHECK(system.Stats().liveAllocations == 0);
}

} // namespace rocks::test
