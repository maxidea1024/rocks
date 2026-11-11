#include <Core/Containers/BitArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <Tests/Support/Test.h>

using namespace rocks;

namespace {

bool LeftBit(usize index) noexcept {
    return index % 3 == 0 || index % 11 == 2;
}

bool RightBit(usize index) noexcept {
    return index % 5 == 1 || index % 7 == 4;
}

} // namespace

TEST(BitArrayRemoveAtSwapUe56GoldenCrossesWordBoundary) {
    SystemAllocator allocator;
    {
        BitArray overlapping(allocator);
        REQUIRE(overlapping.TryAdd(false, 132));
        REQUIRE(overlapping.TrySet(0, true));
        REQUIRE(overlapping.TrySet(61, true));
        REQUIRE(overlapping.TrySet(63, true));
        REQUIRE(overlapping.TrySet(65, true));
        REQUIRE(overlapping.TrySet(67, true));
        // UE 5.6의 `NumBits - NumBitsToRemove` 위치에서 읽는다. tail이 count보다
        // 짧으므로 65, 66, 67이 유지되는 제거 위치 62, 63, 64에 들어간다.
        REQUIRE(overlapping.TryRemoveAtSwap(62, 67));
        REQUIRE(overlapping.Size() == 65);
        for (usize i = 0; i < 65; ++i) {
            const bool expected = i == 0 || i == 61 || i == 62 || i == 64;
            CHECK(overlapping[i] == expected);
        }
        CHECK(overlapping.CountSetBits() == 4);

        BitArray disjoint(allocator);
        REQUIRE(disjoint.TryAdd(false, 130));
        REQUIRE(disjoint.TrySet(126, true));
        REQUIRE(disjoint.TrySet(128, true));
        REQUIRE(disjoint.TryRemoveAtSwap(62, 4));
        REQUIRE(disjoint.Size() == 126);
        for (usize i = 0; i < 126; ++i) {
            CHECK(disjoint[i] == (i == 62 || i == 64));
        }
        CHECK(disjoint.CountSetBits() == 2);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArrayBitwisePoliciesPreserveMeaningAcrossWordBoundaries) {
    SystemAllocator allocator;
    {
        const usize lengths[] = {63, 64, 65, 127, 128, 129};
        const BitOperation operations[] = {BitOperation::And, BitOperation::Or, BitOperation::Xor};
        const BitSizePolicy policies[] = {BitSizePolicy::Maximum, BitSizePolicy::Minimum,
                                          BitSizePolicy::Maintain};
        for (usize leftSize : lengths) {
            for (usize rightSize : lengths) {
                BitArray left(allocator);
                BitArray right(allocator);
                REQUIRE(left.TryAdd(false, leftSize));
                REQUIRE(right.TryAdd(false, rightSize));
                for (usize i = 0; i < leftSize; ++i) {
                    REQUIRE(left.TrySet(i, LeftBit(i)));
                }
                for (usize i = 0; i < rightSize; ++i) {
                    REQUIRE(right.TrySet(i, RightBit(i)));
                }
                for (BitOperation operation : operations) {
                    for (BitSizePolicy policy : policies) {
                        for (uint32 fill = 0; fill < 2; ++fill) {
                            auto output = left.TryClone(allocator);
                            REQUIRE(output);
                            REQUIRE(output.Value().TryCombine(right, operation, policy, fill != 0));
                            const usize expectedSize =
                                policy == BitSizePolicy::Maintain ? leftSize
                                : policy == BitSizePolicy::Minimum
                                    ? (leftSize < rightSize ? leftSize : rightSize)
                                    : (leftSize > rightSize ? leftSize : rightSize);
                            REQUIRE(output.Value().Size() == expectedSize);
                            for (usize i = 0; i < expectedSize; ++i) {
                                const bool a = i < leftSize ? LeftBit(i) : fill != 0;
                                const bool b = i < rightSize ? RightBit(i) : fill != 0;
                                const bool expected = operation == BitOperation::And  ? a && b
                                                      : operation == BitOperation::Or ? a || b
                                                                                      : a != b;
                                CHECK(output.Value()[i] == expected);
                            }
                        }
                    }
                }
                // 입력을 수정하지 않는 것도 결합의 계약이다.
                for (usize i = 0; i < leftSize; ++i) {
                    CHECK(left[i] == LeftBit(i));
                }
                for (usize i = 0; i < rightSize; ++i) {
                    CHECK(right[i] == RightBit(i));
                }
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(BitArraySelfAndOrAcrossWordBoundariesPreservesBits) {
    SystemAllocator allocator;
    {
        const usize lengths[] = {0, 63, 64, 65, 127, 128, 129};
        for (usize length : lengths) {
            BitArray bits(allocator);
            REQUIRE(bits.TryAdd(false, length));
            for (usize i = 0; i < length; ++i) {
                REQUIRE(bits.TrySet(i, LeftBit(i)));
            }
            REQUIRE(bits.TryCombine(bits, BitOperation::And));
            REQUIRE(bits.Size() == length);
            for (usize i = 0; i < length; ++i) {
                CHECK(bits[i] == LeftBit(i));
            }
            REQUIRE(bits.TryCombine(bits, BitOperation::Or));
            REQUIRE(bits.Size() == length);
            for (usize i = 0; i < length; ++i) {
                CHECK(bits[i] == LeftBit(i));
            }
        }
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}
