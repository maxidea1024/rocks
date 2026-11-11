#include "Tests/Support/Test.h"
#include "Tests/Support/NFailAllocator.h"
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>

namespace rocks::test {
namespace {

struct Base {
    static inline int destroyed = 0;
    int baseValue = 1;

    virtual ~Base() noexcept {
        ++destroyed;
    }
};

// 첫 기반이 아닌 Base를 만들어 Base 포인터 주소가 할당 주소와 다르게 한다.
struct Padding {
    double padding[4] = {};
    virtual ~Padding() noexcept = default;
};

struct Derived final : Padding, Base {
    static inline int derivedDestroyed = 0;
    int derivedValue;

    explicit Derived(int value) noexcept : derivedValue(value) {
    }

    ~Derived() noexcept override {
        ++derivedDestroyed;
    }
};

struct alignas(64) OverAligned {
    int value = 3;
};

} // namespace

TEST(UniquePtr_OwnsAndReturnsStorage) {
    SystemAllocator allocator;
    {
        auto created = UniquePtr<int>::TryMake(allocator, 42);
        REQUIRE(created);
        UniquePtr<int> pointer = Move(created.Value());
        CHECK(pointer && *pointer == 42 && pointer.GetAllocator() == &allocator);
        CHECK(allocator.Stats().liveAllocations == 1);
        UniquePtr<int> moved = Move(pointer);
        CHECK(!pointer && pointer == nullptr && *moved == 42);
        moved.Reset();
        CHECK(!moved && allocator.Stats().liveAllocations == 0);
        moved.Reset(); // 빈 포인터 Reset은 무해하다.
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(UniquePtr_AllocationFailurePropagates) {
    SystemAllocator system;
    NFailAllocator allocator(system);
    allocator.FailOnAttempt(1);
    auto failed = UniquePtr<int>::TryMake(allocator, 1);
    CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(UniquePtr_DerivedToBaseUsesOriginalAllocation) {
    SystemAllocator allocator;
    Base::destroyed = 0;
    Derived::derivedDestroyed = 0;
    {
        auto created = UniquePtr<Derived>::TryMake(allocator, 7);
        REQUIRE(created);
        void* derivedAddress = created.Value().Get();
        UniquePtr<Base> base = Move(created.Value());
        // Base는 두 번째 기반이므로 주소가 달라진다. 해제는 원래 할당 주소로 해야 한다.
        CHECK(static_cast<void*>(base.Get()) != derivedAddress);
        CHECK(base->baseValue == 1);
        CHECK(allocator.Stats().liveAllocations == 1);
    }
    CHECK(Derived::derivedDestroyed == 1 && Base::destroyed == 1);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(UniquePtr_MoveAssignmentReleasesPrevious) {
    SystemAllocator allocator;
    auto first = UniquePtr<OverAligned>::TryMake(allocator);
    auto second = UniquePtr<OverAligned>::TryMake(allocator);
    REQUIRE(first && second);
    CHECK(reinterpret_cast<uptr>(first.Value().Get()) % 64 == 0);
    UniquePtr<OverAligned> target = Move(first.Value());
    target = Move(second.Value());
    CHECK(allocator.Stats().liveAllocations == 1);
    UniquePtr<OverAligned>& alias = target;
    target = Move(alias); // 자기 이동은 소유권을 유지한다.
    CHECK(target && target->value == 3);
    target = nullptr;
    CHECK(allocator.Stats().liveAllocations == 0);
}

namespace {

struct ListNode {
    UniquePtr<ListNode> next;
    int value;

    explicit ListNode(int number) noexcept : value(number) {
    }
};

} // namespace

TEST(UniquePtr_AssignFromOwnedMemberKeepsSourceAlive) {
    SystemAllocator allocator;
    auto first = UniquePtr<ListNode>::TryMake(allocator, 1);
    auto second = UniquePtr<ListNode>::TryMake(allocator, 2);
    auto third = UniquePtr<ListNode>::TryMake(allocator, 3);
    REQUIRE(first && second && third);
    UniquePtr<ListNode> head = Move(first.Value());
    head->next = Move(second.Value());
    head->next->next = Move(third.Value());
    // 대입 원본이 기존 head 객체의 멤버다. 기존 객체를 먼저 소멸하면 원본을 읽기 전에 해제된다.
    head = Move(head->next);
    CHECK(head->value == 2 && head->next->value == 3);
    head = Move(head->next);
    CHECK(head->value == 3 && !head->next);
    CHECK(allocator.Stats().liveAllocations == 1);
    head.Reset();
    CHECK(allocator.Stats().liveAllocations == 0);
}

} // namespace rocks::test
