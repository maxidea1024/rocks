#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Base/Checked.h>
#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>

using namespace rocks;

namespace {

struct LifetimeValue {
    static inline int32 live = 0;
    static inline int32 constructed = 0;
    static inline int32 destroyed = 0;
    static inline int32 moved = 0;
    static inline int32 copied = 0;

    int32 value;

    explicit LifetimeValue(int32 initial = 0) noexcept : value(initial) {
        ++live;
        ++constructed;
    }

    LifetimeValue(const LifetimeValue& other) noexcept : value(other.value) {
        ++live;
        ++constructed;
        ++copied;
    }

    LifetimeValue(LifetimeValue&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
        ++constructed;
        ++moved;
    }

    ~LifetimeValue() noexcept {
        --live;
        ++destroyed;
    }

    static void Reset() noexcept {
        live = 0;
        constructed = 0;
        destroyed = 0;
        moved = 0;
        copied = 0;
    }
};

struct MoveOnlyValue {
    int32 value;

    explicit MoveOnlyValue(int32 initial) noexcept : value(initial) {
    }

    MoveOnlyValue(MoveOnlyValue&& other) noexcept : value(other.value) {
        other.value = -1;
    }

    MoveOnlyValue(const MoveOnlyValue&) = delete;
};

struct alignas(64) AlignedValue {
    uint64 value = 7;
};

} // namespace

TEST(CoreTypesAndCheckedArithmetic) {
    static_assert(sizeof(usize) == sizeof(void*));
    static_assert(IsSameV<RemoveReferenceT<int32&&>, int32>);
    static_assert(IsSameV<RemoveConstT<const int32>, int32>);
    constexpr Guid first{1, 2};
    constexpr Guid same{1, 2};
    constexpr Guid different{2, 1};
    static_assert(first == same);
    static_assert(first != different);

    usize output = 19;
    CHECK(!TryAdd(MaxSize, 1, output));
    CHECK(output == 19);
    CHECK(!TryMultiply(MaxSize, 2, output));
    CHECK(output == 19);
    CHECK(TryMultiply(0, MaxSize, output));
    CHECK(output == 0);
    CHECK(TryAdd(17, 25, output));
    CHECK(output == 42);
    CHECK(IsPowerOfTwo(4096));
    CHECK(!IsPowerOfTwo(0));
    CHECK(!IsPowerOfTwo(3));
}

TEST(CoreResultMoveOnlyAndErrorValues) {
    auto value = Result<MoveOnlyValue>::Success(MoveOnlyValue(42));
    REQUIRE(value);
    auto moved = Move(value);
    CHECK(moved.Value().value == 42);
    CHECK(value.Value().value == -1);

    auto failure = Result<MoveOnlyValue>::Failure(ErrorCode::Cancelled);
    CHECK(!failure);
    CHECK(failure.Error() == ErrorCode::Cancelled);
    failure = Move(moved);
    REQUIRE(failure);
    CHECK(failure.Value().value == 42);

    auto errorAsValue = Result<ErrorCode>::Success(ErrorCode::TypeMismatch);
    REQUIRE(errorAsValue);
    CHECK(errorAsValue.Value() == ErrorCode::TypeMismatch);
    CHECK(errorAsValue.Error() == ErrorCode::None);

    auto invalidFailure = Result<void>::Failure(ErrorCode::None);
    CHECK(!invalidFailure);
    CHECK(invalidFailure.Error() == ErrorCode::InvalidArgument);
}

TEST(CoreResultDestroysEveryConstructedValue) {
    LifetimeValue::Reset();
    {
        auto first = Result<LifetimeValue>::Success(LifetimeValue(13));
        auto second = Result<LifetimeValue>::Failure(ErrorCode::NotFound);
        second = Move(first);
        CHECK(second.Value().value == 13);
        auto third = Move(second);
        CHECK(third.Value().value == 13);
        third = Result<LifetimeValue>::Failure(ErrorCode::Cancelled);
    }

    CHECK(LifetimeValue::live == 0);
    CHECK(LifetimeValue::constructed == LifetimeValue::destroyed);
}

TEST(CoreAllocatorZeroAlignmentAndLedger) {
    SystemAllocator allocator;
    auto empty = allocator.Allocate(0, 8);
    REQUIRE(empty);
    CHECK(empty.Value() == nullptr);
    CHECK(allocator.Deallocate(nullptr));
    CHECK(allocator.Stats().totalAllocations == 0);

    auto invalid = allocator.Allocate(17, 3);
    CHECK(!invalid);
    CHECK(invalid.Error() == ErrorCode::InvalidArgument);
    auto zeroAlignment = allocator.Allocate(17, 0);
    CHECK(!zeroAlignment);
    auto overflow = allocator.Allocate(MaxSize, 64);
    CHECK(!overflow);
    CHECK(overflow.Error() == ErrorCode::Overflow);

    uint64 allocations = 0;
    for (usize alignment = 1; alignment <= 4096; alignment *= 2) {
        auto allocation = allocator.Allocate(37, alignment, 123);
        REQUIRE(allocation);
        CHECK(reinterpret_cast<uptr>(allocation.Value()) % alignment == 0);
        auto* bytes = static_cast<uint8*>(allocation.Value());
        for (usize index = 0; index < 37; ++index) {
            bytes[index] = static_cast<uint8>(index);
        }

        const auto active = allocator.Stats();
        CHECK(active.liveAllocations == 1);
        CHECK(active.liveBytes == 37);
        CHECK(allocator.Deallocate(allocation.Value()));
        ++allocations;
    }

    const auto stats = allocator.Stats();
    CHECK(stats.liveAllocations == 0);
    CHECK(stats.liveBytes == 0);
    CHECK(stats.totalAllocations == allocations);
}

TEST(CoreAllocatorRejectsUnownedAndRepeatedFree) {
    SystemAllocator first;
    SystemAllocator second;
    int32 local = 0;
    auto invalid = first.Deallocate(&local);
    CHECK(!invalid);
    CHECK(invalid.Error() == ErrorCode::NotFound);
    CHECK(!first.Deallocate(reinterpret_cast<void*>(static_cast<uptr>(1))));

    auto allocation = first.Allocate(32, 16);
    REQUIRE(allocation);
    void* address = allocation.Value();
    CHECK(!second.Deallocate(address));
    CHECK(first.Stats().liveAllocations == 1);
    CHECK(first.Deallocate(address));
    auto repeated = first.Deallocate(address);
    CHECK(!repeated);
    CHECK(repeated.Error() == ErrorCode::NotFound);
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(CoreSpanConstAndEmptyView) {
    struct BaseElement {
        int32 value;
    };

    struct DerivedElement : BaseElement {
        int32 extra;
    };

    static_assert(!__is_constructible(Span<BaseElement>, Span<DerivedElement>));
    static_assert(!__is_constructible(Span<BaseElement>, DerivedElement*, usize));
    static_assert(__is_constructible(Span<const int32>, Span<int32>));
    Span<int32> nullView(nullptr, 0);
    CHECK(nullView.Size() == 0);
    int32 values[] = {4, 8, 15, 16};
    Span<int32> mutableView(values);
    Span<const int32> view(mutableView);
    CHECK(view.Size() == 4);
    CHECK(view[2] == 15);
    mutableView[1] = 42;
    CHECK(view[1] == 42);
    Span<const int32> empty;
    CHECK(empty.Data() == nullptr);
    CHECK(empty.Size() == 0);
}

TEST(CoreArraySelfAliasCopyAcrossGrowth) {
    LifetimeValue::Reset();
    SystemAllocator allocator;
    {
        Array<LifetimeValue> values(allocator);
        REQUIRE(values.TryReserve(2));
        REQUIRE(values.TryEmplaceBack(11));
        REQUIRE(values.TryEmplaceBack(22));
        REQUIRE(values.TryPushBack(values[0]));
        CHECK(values.Size() == 3);
        CHECK(values[0].value == 11);
        CHECK(values[1].value == 22);
        CHECK(values[2].value == 11);
        CHECK(LifetimeValue::live == 3);
        CHECK(LifetimeValue::copied == 1);
    }

    CHECK(LifetimeValue::live == 0);
    CHECK(LifetimeValue::constructed == LifetimeValue::destroyed);
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(CoreArrayFailedGrowthPreservesAliasedMoveSource) {
    LifetimeValue::Reset();
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    {
        Array<LifetimeValue> values(allocator);
        REQUIRE(values.TryReserve(2));
        REQUIRE(values.TryEmplaceBack(11));
        REQUIRE(values.TryEmplaceBack(22));
        auto* previous = values.Data();
        const auto previousCapacity = values.Capacity();
        const auto previousMoves = LifetimeValue::moved;

        allocator.FailOnAttempt(1);
        auto failed = values.TryPushBack(Move(values[0]));
        CHECK(!failed);
        CHECK(failed.Error() == ErrorCode::OutOfMemory);
        CHECK(values.Data() == previous);
        CHECK(values.Capacity() == previousCapacity);
        CHECK(values.Size() == 2);
        CHECK(values[0].value == 11);
        CHECK(LifetimeValue::moved == previousMoves);

        allocator.DisableFailure();
        REQUIRE(values.TryPushBack(Move(values[0])));
        CHECK(values.Size() == 3);
        CHECK(values[0].value == -1);
        CHECK(values[1].value == 22);
        CHECK(values[2].value == 11);
    }

    CHECK(LifetimeValue::live == 0);
    CHECK(LifetimeValue::constructed == LifetimeValue::destroyed);
    CHECK(backing.Stats().liveAllocations == 0);
}

TEST(CoreArrayReserveFailureAndOverflowPreserveState) {
    SystemAllocator backing;
    test::NFailAllocator allocator(backing);
    Array<uint64> values(allocator);
    REQUIRE(values.TryPushBack(71));
    auto* previous = values.Data();
    const auto capacity = values.Capacity();

    allocator.FailOnAttempt(1);
    auto failed = values.TryReserve(128);
    CHECK(!failed);
    CHECK(failed.Error() == ErrorCode::OutOfMemory);
    CHECK(values.Data() == previous);
    CHECK(values.Capacity() == capacity);
    CHECK(values[0] == 71);
    const auto attempts = allocator.Attempts();
    auto overflow = values.TryReserve(MaxSize);
    CHECK(!overflow);
    CHECK(overflow.Error() == ErrorCode::Overflow);
    CHECK(allocator.Attempts() == attempts);
    CHECK(values.Data() == previous);
    CHECK(values[0] == 71);
}

TEST(CoreArrayAllocationFailureSweepClosesLifetimeLedger) {
    for (uint64 failAt = 1; failAt <= 6; ++failAt) {
        LifetimeValue::Reset();
        SystemAllocator backing;
        test::NFailAllocator allocator(backing);
        allocator.FailOnAttempt(failAt);
        bool observedFailure = false;
        {
            Array<LifetimeValue> values(allocator);
            for (int32 value = 0; value < 32; ++value) {
                const usize before = values.Size();
                auto* address = values.Data();
                auto result = values.TryEmplaceBack(value);
                if (!result) {
                    observedFailure = true;
                    CHECK(result.Error() == ErrorCode::OutOfMemory);
                    CHECK(values.Size() == before);
                    CHECK(values.Data() == address);
                    for (usize index = 0; index < before; ++index) {
                        CHECK(values[index].value == static_cast<int32>(index));
                    }

                    break;
                }
            }
        }

        CHECK(observedFailure);
        CHECK(LifetimeValue::live == 0);
        CHECK(LifetimeValue::constructed == LifetimeValue::destroyed);
        CHECK(backing.Stats().liveAllocations == 0);
        CHECK(backing.Stats().liveBytes == 0);
    }
}

TEST(CoreArrayMoveAssignmentKeepsAllocatorOwnership) {
    SystemAllocator first;
    SystemAllocator second;
    {
        Array<MoveOnlyValue> source(first);
        Array<MoveOnlyValue> destination(second);
        REQUIRE(source.TryEmplaceBack(42));
        REQUIRE(destination.TryEmplaceBack(8));
        destination = Move(source);
        CHECK(source.Size() == 0);
        CHECK(source.Data() == nullptr);
        CHECK(destination.Size() == 1);
        CHECK(destination[0].value == 42);
        CHECK(second.Stats().liveAllocations == 0);
        CHECK(first.Stats().liveAllocations == 1);
        REQUIRE(source.TryEmplaceBack(99));
        CHECK(source[0].value == 99);
        CHECK(first.Stats().liveAllocations == 2);
    }

    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(CoreArrayOverAlignmentAndClearKeepsCapacity) {
    SystemAllocator allocator;
    Array<AlignedValue> values(allocator);
    for (usize index = 0; index < 25; ++index) {
        REQUIRE(values.TryEmplaceBack());
    }

    CHECK(reinterpret_cast<uptr>(values.Data()) % alignof(AlignedValue) == 0);
    const auto capacity = values.Capacity();
    auto* address = values.Data();
    values.Clear();
    CHECK(values.Size() == 0);
    CHECK(values.Capacity() == capacity);
    CHECK(values.Data() == address);
    REQUIRE(values.TryEmplaceBack());
    CHECK(values[0].value == 7);
}
