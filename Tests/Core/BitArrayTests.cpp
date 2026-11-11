#include <Core/Containers/BitArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/StackAllocator.h>
#include <Tests/Support/NFailAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

uint32 Next(uint32& seed) noexcept {
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// 각 연산을 독립 bool 배열과 비교해 word 경계와 삭제 순서를 검증한다.
void CheckBits(const BitArray& bits, const bool* model, usize count) noexcept {
    if (bits.Size() != count) {
        CHECK(false);
        return;
    }
    usize set = 0;
    usize firstTrue = BitArray::NotFound;
    usize firstFalse = BitArray::NotFound;
    for (usize i = 0; i < count; ++i) {
        CHECK(bits[i] == model[i]);
        if (model[i]) {
            ++set;
            if (firstTrue == BitArray::NotFound) {
                firstTrue = i;
            }
        } else if (firstFalse == BitArray::NotFound) {
            firstFalse = i;
        }
    }
    CHECK(bits.CountSetBits() == set);
    CHECK(bits.Find(true) == firstTrue);
    CHECK(bits.Find(false) == firstFalse);
}

} // namespace

TEST(BitArrayWordBoundariesReferencesAndGrowth) {
    SystemAllocator allocator;
    {
        BitArray bits(allocator);
        CHECK(bits.IsEmpty());
        CHECK(bits.Find(false) == BitArray::NotFound);
        auto start = bits.TryAdd(false, 130);
        REQUIRE(start);
        CHECK(start.Value() == 0);
        CHECK(bits.Capacity() >= 130);
        REQUIRE(bits.TrySet(0, true));
        REQUIRE(bits.TrySet(63, true));
        REQUIRE(bits.TrySet(64, true));
        REQUIRE(bits.TrySet(129, true));
        CHECK(bits.CountSetBits() == 4);
        bits[64] = bits[0];
        bits[0] = false;
        CHECK(!bits[0] && bits[64]);
        CHECK(bits.Find(false, 63) == 65);
        CHECK(bits.FindAndSetFirstZeroBit(63) == 65);
        CHECK(bits[65]);
        CHECK(bits.Find(false, bits.Size()) == BitArray::NotFound);
        CHECK(!bits.TryGet(130));
        CHECK(!bits.TrySet(130, true));
        CHECK(!bits.TrySetRange(129, 2, true));
        CHECK(bits.TrySetRange(130, 0, true));
        REQUIRE(bits.TryResize(65));
        REQUIRE(bits.TryResize(130, false));
        CHECK(bits[64] && !bits[65] && !bits[129]);
        const usize capacity = bits.Capacity();
        bits.Clear();
        CHECK(bits.IsEmpty() && bits.Capacity() == capacity);
        REQUIRE(bits.Trim());
        CHECK(bits.Capacity() == 0);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayInsertionDeletionAndMovedOwnership) {
    SystemAllocator first;
    SystemAllocator second;
    {
        BitArray bits(first);
        REQUIRE(bits.TryAdd(true, 5));
        REQUIRE(bits.TryInsert(2, 3, false));
        const bool inserted[] = {true, true, false, false, false, true, true, true};
        CheckBits(bits, inserted, 8);
        REQUIRE(bits.TryRemoveAt(1, 4));
        const bool removed[] = {true, true, true, true};
        CheckBits(bits, removed, 4);
        REQUIRE(bits.TrySet(1, false));
        REQUIRE(bits.TryRemoveAtSwap(0, 2));
        const bool swapped[] = {true, true};
        CheckBits(bits, swapped, 2);
        CHECK(!bits.TryInsert(3, 1, false));
        CHECK(!bits.TryRemoveAt(3));
        CHECK(bits.TryRemoveAt(2, 0));
        CHECK(bits.Trim().Error() == ErrorCode::Busy);
        BitArray moved(Move(bits));
        CHECK(bits.IsEmpty() && bits.Capacity() == 0);
        CHECK(&moved.GetAllocator() == &first);
        BitArray target(second);
        REQUIRE(target.TryAdd(false, 1));
        target = Move(moved);
        CHECK(moved.IsEmpty());
        CHECK(&target.GetAllocator() == &first);
        CheckBits(target, swapped, 2);
    }
    CHECK(first.Stats().liveAllocations == 0);
    CHECK(second.Stats().liveAllocations == 0);
}

TEST(BitArrayRemoveAtSwapMatchesUnrealOverlappingRange) {
    SystemAllocator allocator;
    {
        BitArray bits(allocator);
        REQUIRE(bits.TryAdd(false, 8));
        REQUIRE(bits.TrySet(5, true));
        REQUIRE(bits.TryRemoveAtSwap(4, 3));
        const bool expected[] = {false, false, false, false, true};
        CheckBits(bits, expected, 5);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
TEST(BitArrayAllocationFailurePreservesContentsAndCapacity) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BitArray bits(failing);
        REQUIRE(bits.TryAdd(false, 64));
        REQUIRE(bits.TrySetRange(31, 34, true).Error() == ErrorCode::InvalidArgument);
        REQUIRE(bits.TrySetRange(31, 33, true));
        const usize capacity = bits.Capacity();
        failing.FailOnAttempt(1);
        auto failInsert = bits.TryInsert(32, 2, false);
        CHECK(!failInsert && failInsert.Error() == ErrorCode::OutOfMemory);
        CHECK(bits.Size() == 64 && bits.Capacity() == capacity);
        CHECK(bits.CountSetBits() == 33 && bits[31] && bits[63]);
        failing.FailOnAttempt(1);
        auto failReserve = bits.TryReserve(capacity + 1);
        CHECK(!failReserve && failReserve.Error() == ErrorCode::OutOfMemory);
        CHECK(bits.Size() == 64 && bits.CountSetBits() == 33);
        failing.DisableFailure();
        CHECK(bits.TryReserve(MaxSize).Error() == ErrorCode::Overflow);
        CHECK(bits.TryAdd(true, MaxSize).Error() == ErrorCode::Overflow);
        CHECK(bits.Size() == 64 && bits.CountSetBits() == 33);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(BitArrayBitwiseSizePoliciesAndClonePreserveInputs) {
    SystemAllocator allocator;
    {
        BitArray first(allocator);
        BitArray second(allocator);
        REQUIRE(first.TryAdd(false, 3));
        REQUIRE(second.TryAdd(false, 5));
        REQUIRE(first.TrySet(0, true));
        REQUIRE(first.TrySet(2, true));
        REQUIRE(second.TrySet(1, true));
        REQUIRE(second.TrySet(2, true));
        REQUIRE(second.TrySet(4, true));

        auto copy = first.TryClone(allocator);
        REQUIRE(copy);
        REQUIRE(copy.Value().TryCombine(second, BitOperation::Or));
        const bool unionExpected[] = {true, true, true, false, true};
        CheckBits(copy.Value(), unionExpected, 5);
        CHECK(first.Size() == 3 && first.CountSetBits() == 2);
        REQUIRE(copy.Value().TryCombine(second, BitOperation::And, BitSizePolicy::Minimum));
        const bool andExpected[] = {false, true, true, false, true};
        CheckBits(copy.Value(), andExpected, 5);
        REQUIRE(copy.Value().TryCombine(first, BitOperation::Xor, BitSizePolicy::Maintain, true));
        const bool xorExpected[] = {true, true, false, true, false};
        CheckBits(copy.Value(), xorExpected, 5);
        REQUIRE(copy.Value().TryCombine(copy.Value(), BitOperation::Xor));
        CHECK(copy.Value().Size() == 5 && copy.Value().CountSetBits() == 0);
        copy.Value().Invert();
        CHECK(copy.Value().CountSetBits() == 5);
        auto range = copy.Value().TryCountSetBits(1, 4);
        REQUIRE(range);
        CHECK(range.Value() == 3);
        CHECK(copy.Value().TryCountSetBits(4, 3).Error() == ErrorCode::InvalidArgument);
        CHECK(copy.Value().TryCountSetBits(0, 6).Error() == ErrorCode::InvalidArgument);
        CHECK(copy.Value().TryCombine(first, static_cast<BitOperation>(99)).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(copy.Value()
                  .TryCombine(first, BitOperation::And, static_cast<BitSizePolicy>(99))
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(copy.Value().CountSetBits() == 5);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayBitwiseAndCloneFailuresKeepBothInputs) {
    SystemAllocator system;
    test::NFailAllocator failing(system);
    {
        BitArray first(failing);
        BitArray second(system);
        REQUIRE(first.TryAdd(true, 64));
        REQUIRE(second.TryAdd(false, 129));
        REQUIRE(second.TrySet(128, true));
        const usize originalCapacity = first.Capacity();
        failing.FailOnAttempt(1);
        auto combined = first.TryCombine(second, BitOperation::Or);
        CHECK(!combined && combined.Error() == ErrorCode::OutOfMemory);
        CHECK(first.Size() == 64 && first.Capacity() == originalCapacity &&
              first.CountSetBits() == 64);
        CHECK(second.Size() == 129 && second.CountSetBits() == 1);
        failing.FailOnAttempt(1);
        auto copy = second.TryClone(failing);
        CHECK(!copy && copy.Error() == ErrorCode::OutOfMemory);
        CHECK(second.Size() == 129 && second[128]);
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(BitArrayUsesReservedStackStorageWithoutGrowth) {
    SystemAllocator system;
    {
        StackAllocator stack(system);
        {
            BitArray bits(stack);
            REQUIRE(bits.TryReserve(128));
            REQUIRE(bits.TryAdd(true, 96));
            REQUIRE(bits.TryInsert(64, 16, false));
            REQUIRE(bits.TryRemoveAt(0, 8));
            CHECK(bits.Size() == 104 && bits.CountSetBits() == 88);
        }
        REQUIRE(stack.Trim());
    }
    CHECK(system.Stats().liveAllocations == 0);
}

TEST(BitArrayBitwiseExhaustiveSmallInputsMatchReference) {
    SystemAllocator allocator;
    {
        const BitOperation operations[] = {BitOperation::And, BitOperation::Or, BitOperation::Xor};
        const BitSizePolicy policies[] = {BitSizePolicy::Maximum, BitSizePolicy::Minimum,
                                          BitSizePolicy::Maintain};
        for (usize leftSize = 0; leftSize <= 4; ++leftSize) {
            for (uint32 leftMask = 0; leftMask < (1u << leftSize); ++leftMask) {
                for (usize rightSize = 0; rightSize <= 4; ++rightSize) {
                    for (uint32 rightMask = 0; rightMask < (1u << rightSize); ++rightMask) {
                        BitArray left(allocator);
                        BitArray right(allocator);
                        REQUIRE(left.TryAdd(false, leftSize));
                        REQUIRE(right.TryAdd(false, rightSize));
                        for (usize i = 0; i < leftSize; ++i) {
                            REQUIRE(left.TrySet(i, (leftMask & (1u << i)) != 0));
                        }
                        for (usize i = 0; i < rightSize; ++i) {
                            REQUIRE(right.TrySet(i, (rightMask & (1u << i)) != 0));
                        }
                        for (BitOperation operation : operations) {
                            for (BitSizePolicy policy : policies) {
                                for (int fill = 0; fill != 2; ++fill) {
                                    auto output = left.TryClone(allocator);
                                    REQUIRE(output);
                                    REQUIRE(output.Value().TryCombine(right, operation, policy,
                                                                      fill != 0));
                                    const usize length =
                                        policy == BitSizePolicy::Maintain ? leftSize
                                        : policy == BitSizePolicy::Minimum
                                            ? (leftSize < rightSize ? leftSize : rightSize)
                                            : (leftSize > rightSize ? leftSize : rightSize);
                                    REQUIRE(output.Value().Size() == length);
                                    for (usize i = 0; i < length; ++i) {
                                        const bool a =
                                            i < leftSize ? (leftMask & (1u << i)) != 0 : fill != 0;
                                        const bool b = i < rightSize ? (rightMask & (1u << i)) != 0
                                                                     : fill != 0;
                                        const bool expected =
                                            operation == BitOperation::And  ? a && b
                                            : operation == BitOperation::Or ? a || b
                                                                            : a != b;
                                        CHECK(output.Value()[i] == expected);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayDeterministicMixedOperationsMatchReferenceModel) {
    SystemAllocator allocator;
    {
        BitArray bits(allocator);
        bool model[384]{};
        usize count = 0;
        uint32 seed = 0x739ab821u;
        for (usize iteration = 0; iteration < 4000; ++iteration) {
            const uint32 operation = Next(seed) % 6;
            if (operation == 0 && count < 380) {
                const bool value = (Next(seed) & 1) != 0;
                const usize amount = Next(seed) % 5;
                auto result = bits.TryAdd(value, amount);
                REQUIRE(result);
                CHECK(result.Value() == count);
                for (usize i = 0; i < amount; ++i) {
                    model[count++] = value;
                }
            } else if (operation == 1 && count < 380) {
                const usize index = Next(seed) % (count + 1);
                const usize amount = Next(seed) % 5;
                const bool value = (Next(seed) & 1) != 0;
                REQUIRE(bits.TryInsert(index, amount, value));
                for (usize i = count; i > index; --i) {
                    model[i + amount - 1] = model[i - 1];
                }
                for (usize i = 0; i < amount; ++i) {
                    model[index + i] = value;
                }
                count += amount;
            } else if ((operation == 2 || operation == 3) && count != 0) {
                const usize index = Next(seed) % count;
                const usize amount = Next(seed) % (count - index + 1);
                if (operation == 2) {
                    REQUIRE(bits.TryRemoveAt(index, amount));
                    for (usize i = index; i + amount < count; ++i) {
                        model[i] = model[i + amount];
                    }
                } else {
                    REQUIRE(bits.TryRemoveAtSwap(index, amount));
                    // UE5.6은 제거 전 끝의 count비트를 앞의 제거 영역으로 복사한다.
                    // 원본 snapshot으로 기대값을 구성해 구현과 독립적으로 비교한다.
                    bool before[384]{};
                    for (usize i = 0; i < count; ++i) {
                        before[i] = model[i];
                    }
                    for (usize i = 0; i < amount; ++i) {
                        model[index + i] = before[count - amount + i];
                    }
                }
                count -= amount;
            } else if (operation == 4 && count != 0) {
                const usize index = Next(seed) % count;
                const bool value = (Next(seed) & 1) != 0;
                REQUIRE(bits.TrySet(index, value));
                model[index] = value;
            } else if (operation == 5) {
                const usize next = Next(seed) % 160;
                const bool value = (Next(seed) & 1) != 0;
                REQUIRE(bits.TryResize(next, value));
                for (usize i = count; i < next; ++i) {
                    model[i] = value;
                }
                count = next;
            }
            CheckBits(bits, model, count);
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
