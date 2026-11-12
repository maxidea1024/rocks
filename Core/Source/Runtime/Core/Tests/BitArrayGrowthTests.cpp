#include <Core/Containers/BitArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

using namespace rocks;

TEST(BitArrayRepeatedSmallAddsUseGeometricGrowth) {
    SystemAllocator allocator;
    {
        BitArray bits(allocator);
        constexpr usize count = 65536;
        for (usize i = 0; i < count; ++i) {
            auto added = bits.TryAdd(i % 97 == 0);
            REQUIRE(added);
            CHECK(added.Value() == i);
        }

        CHECK(bits.Size() == count);
        CHECK(bits.Capacity() >= count);
        for (usize i = 0; i < count; ++i) {
            CHECK(bits[i] == (i % 97 == 0));
        }
        // 64bit 단위로만 확장하면 1024번이다. 임의의 특정 성장 계수는 강요하지 않는다.
        CHECK(allocator.Stats().totalAllocations <= 32);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayExplicitReserveRetainsWordRoundedContract) {
    SystemAllocator allocator;
    {
        BitArray bits(allocator);
        REQUIRE(bits.TryReserve(65));
        CHECK(bits.Capacity() == 128);
        REQUIRE(bits.TryReserve(129));
        CHECK(bits.Capacity() == 192);
        REQUIRE(bits.TryAdd(true, 129));
        CHECK(bits.Capacity() == 192 && bits.CountSetBits() == 129);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayAutomaticGrowthFailurePreservesBorrowedBitAndState) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BitArray bits(failing);
        REQUIRE(bits.TryReserve(64));
        REQUIRE(bits.TryAdd(true, 64));
        BitReference borrowed = bits[0];
        const usize capacity = bits.Capacity();
        failing.FailOnAttempt(1);
        auto failure = bits.TryAdd(false);
        CHECK(!failure && failure.Error() == ErrorCode::OutOfMemory);
        CHECK(bits.Size() == 64 && bits.Capacity() == capacity && bits.CountSetBits() == 64);
        CHECK(failing.Attempts() == 1);
        borrowed = false;
        CHECK(!bits[0] && bits.CountSetBits() == 63);
        failing.DisableFailure();
        REQUIRE(bits.TryAdd(false));
        CHECK(bits.Size() == 65 && !bits[0] && !bits[64]);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(BitArrayMaximumRepresentableCapacityFailsBeforeArithmeticOverflow) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BitArray bits(failing);
        constexpr usize maximum = (MaxSize / 64) * 64;
        failing.FailOnAttempt(1);
        auto withinRange = bits.TryResize(maximum);
        CHECK(!withinRange && withinRange.Error() == ErrorCode::OutOfMemory);
        CHECK(failing.Attempts() == 1 && bits.IsEmpty() && bits.Capacity() == 0);
        failing.FailOnAttempt(1);
        auto tooLarge = bits.TryResize(maximum + 1);
        CHECK(!tooLarge && tooLarge.Error() == ErrorCode::Overflow);
        CHECK(failing.Attempts() == 0 && bits.IsEmpty() && bits.Capacity() == 0);
        failing.FailOnAttempt(1);
        auto explicitWithinRange = bits.TryReserve(maximum);
        CHECK(!explicitWithinRange && explicitWithinRange.Error() == ErrorCode::OutOfMemory);
        CHECK(failing.Attempts() == 1);
        failing.FailOnAttempt(1);
        auto explicitTooLarge = bits.TryReserve(maximum + 1);
        CHECK(!explicitTooLarge && explicitTooLarge.Error() == ErrorCode::Overflow);
        CHECK(failing.Attempts() == 0);
    }
    CHECK(system.Stats().liveAllocations == 0);
}
