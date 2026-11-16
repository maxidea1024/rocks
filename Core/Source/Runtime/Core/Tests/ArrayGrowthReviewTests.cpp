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

// "하나 추가할 자리 확보 -> 추가"를 반복할 때 TryReserve(Size() + 1)은 매번 정확히 한 칸만 늘려
// 할당이 추가 횟수만큼 일어나고, TryEnsureCapacity(Size() + 1)은 성장 규칙으로 늘려 할당이 기하급수
// 단계 수로 줄어든다. 할당 횟수(SystemAllocator 누적 할당 수)로 결정적으로 본다.
TEST(ArrayEnsureCapacityGrowsGeometricallyUnlikeExactReserve) {
    constexpr int32 Count = 100;
    SystemAllocator linearAllocator;
    {
        Array<int32> values(linearAllocator);
        for (int32 index = 0; index < Count; ++index) {
            REQUIRE(values.TryReserve(values.Size() + 1));
            CHECK(values.Capacity() == values.Size() + 1);
            REQUIRE(values.TryPushBack(index));
        }
    }

    CHECK(linearAllocator.Stats().totalAllocations == static_cast<uint64>(Count));

    SystemAllocator geometricAllocator;
    uint64 expectedAllocations = 0;
    {
        Array<int32> values(geometricAllocator);
        usize expectedCapacity = 0;
        for (int32 index = 0; index < Count; ++index) {
            if (values.Size() + 1 > expectedCapacity) {
                expectedCapacity =
                    ArrayGrowthCapacity(expectedCapacity, values.Size() + 1, sizeof(int32));
                ++expectedAllocations;
            }

            REQUIRE(values.TryEnsureCapacity(values.Size() + 1));
            CHECK(values.Capacity() == expectedCapacity);
            REQUIRE(values.TryPushBack(index));
        }

        for (int32 index = 0; index < Count; ++index) {
            CHECK(values[static_cast<usize>(index)] == index);
        }
    }

    // 4 -> 7 -> 11 -> 17 -> 26 -> 40 -> 61 -> 92 -> 139로 9번이다.
    CHECK(expectedAllocations == 9);
    CHECK(geometricAllocator.Stats().totalAllocations == expectedAllocations);
    CHECK(geometricAllocator.Stats().liveAllocations == 0);
}

// 기준은 절대 원소 수다. 이미 충분하면 할당·이동하지 않고, 모자라면 성장 규칙의 용량(요청이 더
// 크면 요청 그대로)을 잡는다. 할당 실패와 overflow는 데이터·size·capacity·주소를 보존한다.
TEST(ArrayEnsureCapacityIsAbsoluteAndPreservesStateOnFailure) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<int32> values(allocator);
        REQUIRE(values.TryEnsureCapacity(0));
        CHECK(values.Capacity() == 0 && values.Data() == nullptr);
        CHECK(allocator.Attempts() == 0);

        REQUIRE(values.TryEnsureCapacity(10));
        CHECK(values.Capacity() == ArrayGrowthCapacity(0, 10, sizeof(int32)));
        CHECK(values.Capacity() == 10);
        for (int32 index = 0; index < 3; ++index) {
            REQUIRE(values.TryPushBack(index + 1));
        }

        const int32* data = values.Data();
        allocator.DisableFailure();
        REQUIRE(values.TryEnsureCapacity(10));
        REQUIRE(values.TryEnsureCapacity(4));
        CHECK(allocator.Attempts() == 0);
        CHECK(values.Data() == data && values.Capacity() == 10);

        allocator.FailOnAttempt(1);
        auto failed = values.TryEnsureCapacity(11);
        CHECK(!failed && failed.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == data && values.Capacity() == 10 && values.Size() == 3);
        CHECK(values[0] == 1 && values[1] == 2 && values[2] == 3);

        allocator.DisableFailure();
        auto overflow = values.TryEnsureCapacity(MaxSize / sizeof(int32) + 1);
        CHECK(!overflow && overflow.Error() == ErrorCode::Overflow);
        CHECK(allocator.Attempts() == 0);
        CHECK(values.Data() == data && values.Capacity() == 10 && values.Size() == 3);

        // 10의 1.5배 + 1 = 16, 요청이 더 크면 요청 그대로다.
        REQUIRE(values.TryEnsureCapacity(11));
        CHECK(values.Capacity() == 16);
        REQUIRE(values.TryEnsureCapacity(40));
        CHECK(values.Capacity() == 40);
        CHECK(values.Size() == 3);
        CHECK(values[0] == 1 && values[1] == 2 && values[2] == 3);
    }

    CHECK(backing.Stats().liveAllocations == 0);
}
