#include <Core/Containers/BitArray.h>
#include <Core/Memory/SystemAllocator.h>
#include <TestSupport/Test.h>

// BitArray의 word 단위 경로를 독립 bool 배열 모델과 비교한다. 이 파일은 BitArray.h와
// SystemAllocator만 쓰므로 ASan·UBSan 하네스가 같은 소스를 그대로 컴파일해 재사용한다.

using namespace rocks;

namespace {

bool Pattern(usize index) noexcept {
    return ((index * 17 + 3) % 11) < 5;
}

usize WordsToBits(usize words) noexcept {
    return words * 64;
}

usize RoundUpToWord(usize bits) noexcept {
    return (bits + 63) / 64 * 64;
}

} // namespace

TEST(BitArrayWordMovesAgainstIndependentBits) {
    SystemAllocator allocator;
    const usize starts[] = {0, 1, 62, 63, 64, 65, 127, 128, 129, 257};
    const usize amounts[] = {0, 1, 2, 63, 64, 65, 127, 128, 129};
    for (usize start : starts) {
        for (usize amount : amounts) {
            BitArray bits(allocator);
            bool model[512]{};
            REQUIRE(bits.TryResize(257));
            for (usize i = 0; i < 257; ++i) {
                model[i] = Pattern(i);
                bits[i] = model[i];
            }

            for (usize i = 257; i > start; --i) {
                model[i + amount - 1] = model[i - 1];
            }

            for (usize i = 0; i < amount; ++i) {
                model[start + i] = true;
            }

            REQUIRE(bits.TryInsert(start, amount, true));
            for (usize i = 0; i < bits.Size(); ++i) {
                CHECK(bits[i] == model[i]);
            }

            REQUIRE(bits.TryRemoveAt(start, amount));
            for (usize i = 0; i < 257; ++i) {
                CHECK(bits[i] == Pattern(i));
            }

            // UE BitArray의 swap source는 Size()-count이며 Array의 Size()-moved와 다르다.
            const usize removeAt = start < 257 - amount ? start : 257 - amount;
            const usize tail = 257 - removeAt - amount;
            const usize moved = amount < tail ? amount : tail;
            for (usize i = 0; i < 257; ++i) {
                model[i] = Pattern(i);
            }

            for (usize i = 0; i < moved; ++i) {
                model[removeAt + i] = Pattern(257 - amount + i);
            }

            REQUIRE(bits.TryRemoveAtSwap(removeAt, amount));
            for (usize i = 0; i < bits.Size(); ++i) {
                CHECK(bits[i] == model[i]);
            }

            // 축소 뒤 재확장해도 크기 밖으로 밀려난 bit가 되살아나지 않는다.
            REQUIRE(bits.TryResize(512, false));
            for (usize i = 257 - amount; i < 512; ++i) {
                CHECK(!bits[i]);
            }
        }
    }
}

TEST(BitArrayWordRangeMasksAndSearch) {
    SystemAllocator allocator;
    BitArray bits(allocator);
    REQUIRE(bits.TryResize(130));
    for (usize begin = 0; begin <= 130; ++begin) {
        for (usize end = begin; end <= 130; ++end) {
            REQUIRE(bits.TrySetRange(0, 130, false));
            REQUIRE(bits.TrySetRange(begin, end - begin, true));
            CHECK(bits.CountSetBits() == end - begin);
            CHECK(bits.Find(true) == (begin == end ? BitArray::NotFound : begin));
            CHECK(bits.Find(true, end) == BitArray::NotFound);
            auto count = bits.TryCountSetBits(begin, end);
            REQUIRE(count);
            CHECK(count.Value() == end - begin);
            bits.Invert();
            CHECK(bits.CountSetBits() == 130 - (end - begin));
            CHECK(bits.Find(false, end) == BitArray::NotFound);
        }
    }
}

TEST(BitArrayWordCombineTailAndMissingValues) {
    SystemAllocator allocator;
    const usize sizes[] = {0, 1, 63, 64, 65, 127, 128, 129};
    const BitOperation operations[] = {BitOperation::And, BitOperation::Or, BitOperation::Xor};
    const BitSizePolicy policies[] = {BitSizePolicy::Minimum, BitSizePolicy::Maximum,
                                      BitSizePolicy::Maintain};
    const bool missingValues[] = {false, true};
    for (usize a : sizes) {
        for (usize b : sizes) {
            for (BitOperation operation : operations) {
                for (BitSizePolicy policy : policies) {
                    for (bool missing : missingValues) {
                        BitArray left(allocator);
                        BitArray right(allocator);
                        REQUIRE(left.TryResize(a));
                        REQUIRE(right.TryResize(b));
                        for (usize i = 0; i < a; ++i) {
                            left[i] = Pattern(i);
                        }

                        for (usize i = 0; i < b; ++i) {
                            right[i] = !Pattern(i);
                        }

                        REQUIRE(left.TryCombine(right, operation, policy, missing));
                        const usize expectedSize = policy == BitSizePolicy::Maintain ? a
                                                   : policy == BitSizePolicy::Minimum
                                                       ? (a < b ? a : b)
                                                       : (a > b ? a : b);
                        CHECK(left.Size() == expectedSize);
                        usize count = 0;
                        for (usize i = 0; i < expectedSize; ++i) {
                            const bool l = i < a ? Pattern(i) : missing;
                            const bool r = i < b ? !Pattern(i) : missing;
                            const bool expected = operation == BitOperation::And  ? l && r
                                                  : operation == BitOperation::Or ? l || r
                                                                                  : l != r;
                            CHECK(left[i] == expected);
                            count += expected ? 1 : 0;
                        }

                        CHECK(left.CountSetBits() == count);
                        REQUIRE(left.TryCombine(left, BitOperation::Xor));
                        CHECK(left.CountSetBits() == 0);
                        REQUIRE(left.TryResize(192, false));
                        CHECK(left.CountSetBits() == 0);
                    }
                }
            }
        }
    }
}

// Find·Contains·FindAndSetFirstZeroBit·TryClone·TryReserve와 축소 후 재확장을 모델과 비교한다.
TEST(BitArrayWordSearchCloneAndReserveAgainstModel) {
    SystemAllocator allocator;
    const usize sizes[] = {0, 1, 2, 63, 64, 65, 127, 128, 129, 191, 192, 193, 200};
    for (usize size : sizes) {
        BitArray bits(allocator);
        bool model[256]{};
        REQUIRE(bits.TryResize(size));
        bool anyTrue = false;
        bool anyFalse = false;
        for (usize i = 0; i < size; ++i) {
            model[i] = Pattern(i);
            bits[i] = model[i];
            anyTrue = anyTrue || model[i];
            anyFalse = anyFalse || !model[i];
        }

        CHECK(bits.Contains(true) == anyTrue);
        CHECK(bits.Contains(false) == anyFalse);
        for (usize start = 0; start <= size + 1; ++start) {
            usize expectedTrue = BitArray::NotFound;
            usize expectedFalse = BitArray::NotFound;
            for (usize i = start; i < size; ++i) {
                if (model[i] && expectedTrue == BitArray::NotFound) {
                    expectedTrue = i;
                }

                if (!model[i] && expectedFalse == BitArray::NotFound) {
                    expectedFalse = i;
                }
            }

            CHECK(bits.Find(true, start) == expectedTrue);
            CHECK(bits.Find(false, start) == expectedFalse);
        }

        auto clone = bits.TryClone(allocator);
        REQUIRE(clone);
        CHECK(clone.Value().Size() == size);
        CHECK(clone.Value().Capacity() == RoundUpToWord(size));
        for (usize i = 0; i < size; ++i) {
            CHECK(clone.Value()[i] == model[i]);
        }

        // 마지막 word의 padding bit는 false 검색에 잡히지 않는다.
        for (;;) {
            usize expected = BitArray::NotFound;
            for (usize i = 0; i < size; ++i) {
                if (!model[i]) {
                    expected = i;
                    break;
                }
            }

            const usize found = bits.FindAndSetFirstZeroBit();
            CHECK(found == expected);
            if (found == BitArray::NotFound || found != expected) {
                break;
            }

            model[found] = true;
        }

        CHECK(bits.CountSetBits() == size);
        CHECK(!bits.Contains(false));
        const usize before = bits.Capacity();
        REQUIRE(bits.TryReserve(size + 130));
        const usize expectedCapacity = before >= size + 130 ? before : RoundUpToWord(size + 130);
        CHECK(bits.Capacity() == expectedCapacity);
        CHECK(bits.CountSetBits() == size);
        REQUIRE(bits.TryResize(size / 2));
        REQUIRE(bits.TryResize(size + 70, false));
        CHECK(bits.CountSetBits() == size / 2);
        CHECK(bits.Find(true, size / 2) == BitArray::NotFound);
        bits.Clear();
        REQUIRE(bits.TryResize(size + 70, false));
        CHECK(bits.CountSetBits() == 0);
    }
}

// 자동 성장은 UE slack을 64-bit word 수에 적용한다: 첫 할당 max(4, words), 이후
// words + floor(words * 3/8) + 16. 명시적 TryReserve는 요청 bit를 word로 올린 크기만 잡는다.
TEST(BitArrayWordGrowthUsesWordSlack) {
    SystemAllocator allocator;
    BitArray bits(allocator);
    REQUIRE(bits.TryResize(1));
    CHECK(bits.Capacity() == WordsToBits(4));
    REQUIRE(bits.TryResize(WordsToBits(4)));
    CHECK(bits.Capacity() == WordsToBits(4));
    REQUIRE(bits.TryAdd(true));
    CHECK(bits.Capacity() == WordsToBits(22));
    REQUIRE(bits.TryResize(WordsToBits(22) + 1));
    CHECK(bits.Capacity() == WordsToBits(23 + 8 + 16));

    BitArray exact(allocator);
    REQUIRE(exact.TryReserve(65));
    CHECK(exact.Capacity() == WordsToBits(2));
    REQUIRE(exact.TryReserve(129));
    CHECK(exact.Capacity() == WordsToBits(3));
}
