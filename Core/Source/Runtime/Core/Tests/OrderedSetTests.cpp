#include <Base/Types.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/String.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

struct SetCollisionHash {
    usize operator()(int32) const noexcept {
        return 0;
    }
};

// Set의 원소 순서(ForEach와 반복자 둘 다)가 expected와 같은지 본다.
template <typename Set, usize N>
bool HasElements(const Set& set, const int32 (&expected)[N]) {
    if (set.Count() != N) {
        return false;
    }

    bool same = true;
    usize visited = 0;
    set.ForEach([&](const int32& key) noexcept {
        if (visited >= N || key != expected[visited]) {
            same = false;
        }

        ++visited;
    });
    usize iterated = 0;
    for (const int32& key : set) {
        if (iterated >= N || key != expected[iterated]) {
            same = false;
        }

        ++iterated;
    }

    return same && visited == N && iterated == N;
}

} // namespace

TEST(OrderedSet_AddKeepsInsertionOrder) {
    SystemAllocator allocator;
    {
        OrderedSet<int32, SetCollisionHash> set(allocator);
        CHECK(set.IsEmpty());
        CHECK(set.begin() == set.end());
        const int32 elements[] = {30, 10, 20, -4};
        for (int32 element : elements) {
            auto added = set.Add(element);
            REQUIRE(added);
            CHECK(added.Value() == InsertOutcome::Inserted);
        }

        CHECK(HasElements(set, elements));
        // 이미 있는 원소의 Add는 순서를 바꾸지 않는다.
        auto duplicate = set.Add(int32{10});
        REQUIRE(duplicate);
        CHECK(duplicate.Value() == InsertOutcome::AlreadyPresent);
        CHECK(HasElements(set, elements));
        REQUIRE(set.Find(20) != nullptr);
        CHECK(*set.Find(20) == 20);
        CHECK(set.Find(21) == nullptr);

        // 지운 뒤 다시 넣은 원소는 끝에 붙는다.
        CHECK(set.Erase(10));
        CHECK(!set.Erase(10));
        REQUIRE(set.Add(int32{10}));
        const int32 reinserted[] = {30, 20, -4, 10};
        CHECK(HasElements(set, reinserted));
        auto cursor = set.cbegin();
        CHECK(*cursor == 30);
        ++cursor;
        CHECK(*cursor.operator->() == 20);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(OrderedSet_EraseIfCompactAndClear) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedSet<int32> set(allocator);
        REQUIRE(set.Reserve(16));
        CHECK(set.Capacity() == 16);
        for (int32 element = 15; element >= 0; --element) {
            REQUIRE(set.Add(element));
        }

        const uint64 attempts = allocator.Attempts();
        auto removed = set.EraseIf([](const int32& element) noexcept { return element % 3 != 0; });
        REQUIRE(removed);
        CHECK(removed.Value() == 10);
        CHECK(allocator.Attempts() == attempts);
        const int32 survivors[] = {15, 12, 9, 6, 3, 0};
        CHECK(HasElements(set, survivors));
        REQUIRE(set.Compact());
        CHECK(set.Capacity() == 8);
        CHECK(HasElements(set, survivors));

        set.ForEach([&](const int32& element) noexcept {
            CHECK(set.Contains(element));
            CHECK(set.Add(int32{100}).Error() == ErrorCode::Busy);
            CHECK(set.Reserve(64).Error() == ErrorCode::Busy);
        });
        set.Clear();
        CHECK(set.IsEmpty());
        CHECK(set.Capacity() == 8);
        set.Reset();
        CHECK(set.Capacity() == 0);
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedSet_AllocationFailureLeavesSetUnchanged) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        OrderedSet<int32> set(allocator);
        allocator.FailOnAttempt(1);
        auto first = set.Add(int32{1});
        CHECK(!first);
        CHECK(first.Error() == ErrorCode::OutOfMemory);
        CHECK(set.IsEmpty());

        allocator.DisableFailure();
        for (int32 element = 0; element != 4; ++element) {
            REQUIRE(set.Add(element));
        }

        const int32* pointer = set.Find(3);
        allocator.FailOnAttempt(1);
        auto growth = set.Add(int32{4});
        CHECK(!growth);
        CHECK(growth.Error() == ErrorCode::OutOfMemory);
        CHECK(set.Find(3) == pointer);
        CHECK(set.Capacity() == 4);
        const int32 unchanged[] = {0, 1, 2, 3};
        CHECK(HasElements(set, unchanged));
        auto overflow = set.Reserve(MaxSize);
        CHECK(!overflow);
        CHECK(overflow.Error() == ErrorCode::Overflow);
        CHECK(HasElements(set, unchanged));
        allocator.DisableFailure();
        REQUIRE(set.Add(int32{4}));
        const int32 grown[] = {0, 1, 2, 3, 4};
        CHECK(HasElements(set, grown));
    }
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(OrderedSet_EqualityIncludesOrderAndMove) {
    SystemAllocator first;
    SystemAllocator second;
    {
        OrderedSet<String> left(first);
        OrderedSet<String> right(second);
        const char* names[] = {"b", "a", "c"};
        for (const char* name : names) {
            String element(first);
            REQUIRE(element.TryAssign(name));
            REQUIRE(left.Add(Move(element)));
        }

        for (usize index = 3; index != 0; --index) {
            String element(second);
            REQUIRE(element.TryAssign(names[index - 1]));
            REQUIRE(right.Add(Move(element)));
        }

        // 지우고 다시 넣어 끝으로 보낸다.
        auto moveToBack = [&](const char* name) noexcept {
            if (!right.Erase(StringView(name))) {
                return false;
            }

            String element(second);
            return element.TryAssign(name) && right.Add(Move(element));
        };

        // 같은 원소라도 순서가 다르면 다르다(left b, a, c / right c, a, b).
        CHECK(left != right);
        CHECK(right.Contains(StringView("a")));
        REQUIRE(moveToBack("c"));
        CHECK(left != right); // right a, b, c
        REQUIRE(moveToBack("a"));
        REQUIRE(moveToBack("c"));
        CHECK(left == right); // right b, a, c

        OrderedSet<String> moved(Move(left));
        CHECK(left.IsEmpty());
        CHECK(moved == right);
        CHECK(moved.begin()->View() == StringView("b"));
    }
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

namespace rocks::test {

// 별도 child에서만 실행한다. 원소를 지운 뒤 기존 반복자를 쓰면 Fatal이어야 한다.
int RunOrderedSetNegativeControl() noexcept {
    PrepareFatalTestProcess();
    SystemAllocator allocator;
    OrderedSet<int32> set(allocator);
    if (!set.Add(int32{1}) || !set.Add(int32{2})) {
        return 2;
    }

    auto cursor = set.begin();
    (void)set.Erase(2);
    return *cursor == 1 ? 0 : 2;
}

} // namespace rocks::test
