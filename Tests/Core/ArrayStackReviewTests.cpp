#include <Core/Containers/Array.h>
#include <Core/Memory/StackAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

struct OnlyMove {
    static inline int live = 0;
    int value = 0;

    explicit OnlyMove(int input) noexcept : value(input) {
        ++live;
    }
    OnlyMove(const OnlyMove&) = delete;
    OnlyMove& operator=(const OnlyMove&) = delete;
    OnlyMove& operator=(OnlyMove&&) = delete;
    OnlyMove(OnlyMove&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }
    ~OnlyMove() noexcept {
        --live;
    }
};

} // namespace

TEST(ArrayStackReservedMoveOnlyAliasAndRangeRemoval) {
    SystemAllocator system;
    {
        StackAllocator stack(system);
        {
            Array<OnlyMove> values(stack);
            REQUIRE(values.TryReserve(12));
            REQUIRE(values.TryEmplaceBack(10));
            REQUIRE(values.TryEmplaceBack(20));
            REQUIRE(values.TryEmplaceBack(30));
            auto* address = values.Data();
            REQUIRE(values.TryInsert(Move(values[2]), 1));
            CHECK(values.Data() == address && values.Size() == 4);
            CHECK(values[0].value == 10 && values[1].value == 30 && values[2].value == 20 &&
                  values[3].value == -1);
            REQUIRE(values.TryInsert(Move(values[0]), 3));
            CHECK(values.Data() == address && values.Size() == 5);
            CHECK(values[0].value == -1 && values[1].value == 30 && values[2].value == 20 &&
                  values[3].value == 10 && values[4].value == -1);
            REQUIRE(values.TryRemoveAt(1, 2));
            CHECK(values.Size() == 3 && values[0].value == -1 && values[1].value == 10 &&
                  values[2].value == -1);
            REQUIRE(values.TryRemoveAtSwap(0, 1));
            CHECK(values.Size() == 2 && values[0].value == -1 && values[1].value == 10);
            CHECK(OnlyMove::live == 2);
        }
        CHECK(OnlyMove::live == 0);
        REQUIRE(stack.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(ArrayStackReservedSelfAppendDoesNotReallocate) {
    SystemAllocator system;
    {
        StackAllocator stack(system);
        {
            Array<int32> values(stack);
            REQUIRE(values.TryReserve(8));
            REQUIRE(values.TryPushBack(1));
            REQUIRE(values.TryPushBack(2));
            REQUIRE(values.TryPushBack(3));
            auto* address = values.Data();
            REQUIRE(values.TryAppend(values));
            CHECK(values.Data() == address && values.Size() == 6);
            CHECK(values[0] == 1 && values[1] == 2 && values[2] == 3 && values[3] == 1 &&
                  values[4] == 2 && values[5] == 3);
        }
        REQUIRE(stack.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}
