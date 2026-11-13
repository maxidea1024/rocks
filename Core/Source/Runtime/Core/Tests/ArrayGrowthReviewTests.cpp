#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

namespace {

// noexcept const 비교와 별도의 변경 가능한 비교를 구분해 읽기 전용 질의의 호출을 검증한다.
struct EqualityProbe {
    int value = 0;
    mutable uint32 constCalls = 0;
    uint32 mutableCalls = 0;

    explicit EqualityProbe(int input) noexcept : value(input) {
    }

    EqualityProbe(const EqualityProbe&) noexcept = default;
    EqualityProbe(EqualityProbe&&) noexcept = default;
    ~EqualityProbe() noexcept = default;

    bool operator==(const EqualityProbe& other) noexcept {
        ++mutableCalls;
        return value == other.value;
    }

    bool operator==(const EqualityProbe& other) const noexcept {
        ++constCalls;
        return value == other.value;
    }
};

} // namespace

TEST(ArrayInitialBulkResizeHonorsRequestedCapacity) {
    SystemAllocator allocator;
    const usize counts[] = {4u, 5u, 64u};
    for (usize count : counts) {
        Array<int32> values(allocator);
        REQUIRE(values.TryResize(count));
        CHECK(values.Size() == count);
        CHECK(values.Capacity() >= count);
        for (usize i = 0; i < count; ++i) {
            CHECK(values[i] == 0);
            values[i] = static_cast<int32>(i + 11);
        }

        for (usize i = 0; i < count; ++i) {
            CHECK(values[i] == static_cast<int32>(i + 11));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArrayInitialBulkAppendHonorsRequestedCapacity) {
    SystemAllocator allocator;
    int32 input[64]{};
    for (usize i = 0; i < 64; ++i) {
        input[i] = static_cast<int32>(i * 7 + 3);
    }

    const usize counts[] = {4u, 5u, 64u};
    for (usize count : counts) {
        Array<int32> values(allocator);
        REQUIRE(values.TryAppend(Span<const int32>(input, count)));
        CHECK(values.Size() == count);
        CHECK(values.Capacity() >= count);
        for (usize i = 0; i < count; ++i) {
            CHECK(values[i] == input[i]);
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArrayInitialBulkAllocationFailureLeavesEmptyAndSourcesUnchanged) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    int32 input[64]{};
    for (usize i = 0; i < 64; ++i) {
        input[i] = static_cast<int32>(i + 1);
    }

    {
        Array<int32> resized(allocator);
        allocator.FailOnAttempt(1);
        auto failed = resized.TryResize(64);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(resized.IsEmpty() && resized.Data() == nullptr && resized.Capacity() == 0);
        CHECK(backing.Stats().liveAllocations == 0);
        allocator.DisableFailure();
        REQUIRE(resized.TryResize(64));
        CHECK(resized.Size() == 64 && resized.Capacity() >= 64);
    }

    {
        Array<int32> appended(allocator);
        allocator.FailOnAttempt(1);
        auto failed = appended.TryAppend(Span<const int32>(input, 64));
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(appended.IsEmpty() && appended.Data() == nullptr && appended.Capacity() == 0);
        CHECK(backing.Stats().liveAllocations == 0);
        for (usize i = 0; i < 64; ++i) {
            CHECK(input[i] == static_cast<int32>(i + 1));
        }

        allocator.DisableFailure();
        REQUIRE(appended.TryAppend(Span<const int32>(input, 64)));
        CHECK(appended.Size() == 64 && appended.Capacity() >= 64);
    }

    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(ArrayFindOnConstArrayUsesConstComparisonOverload) {
    SystemAllocator allocator;
    {
        Array<EqualityProbe> values(allocator);
        REQUIRE(values.TryEmplaceBack(1));
        REQUIRE(values.TryEmplaceBack(2));
        REQUIRE(values.TryEmplaceBack(1));
        EqualityProbe target(1);
        const Array<EqualityProbe>& readable = values;
        CHECK(readable.Find(target) == 0);
        CHECK(readable.FindLast(target) == 2);
        CHECK(readable.Contains(target));
        CHECK(values[0].mutableCalls == 0 && values[1].mutableCalls == 0 &&
              values[2].mutableCalls == 0);
        CHECK(values[0].constCalls == 2 && values[2].constCalls == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 공개 성장 규칙(ArrayGrowthCapacity)은 Array가 실제로 고르는 용량과 같다(경로 삽입이 임시 배열에
// 같은 용량을 예약한다 — Docs/Design/ReflectionFoundations.ko.md 7.1절).
TEST(ArrayGrowthCapacityMatchesArrayGrowth) {
    static_assert(ArrayGrowthCapacity(0, 1, 4) == 4);
    static_assert(ArrayGrowthCapacity(0, 9, 4) == 9);
    static_assert(ArrayGrowthCapacity(4, 5, 4) == 7);
    static_assert(ArrayGrowthCapacity(4, 20, 4) == 20);
    static_assert(ArrayGrowthCapacity(MaxSize / 8 - 1, MaxSize / 8, 8) == MaxSize / 8);
    SystemAllocator allocator;
    Array<int32> values(allocator);
    for (int32 index = 0; index < 100; ++index) {
        const usize before = values.Capacity();
        const usize expected = values.Size() < before
                                   ? before
                                   : ArrayGrowthCapacity(before, values.Size() + 1, sizeof(int32));
        REQUIRE(values.TryPushBack(index));
        CHECK(values.Capacity() == expected);
    }
}
