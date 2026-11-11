#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

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
