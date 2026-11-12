#include <Core/Math/RandomStream.h>
#include <TestSupport/Test.h>

using namespace rocks;

// 사전 조건 위반(NextUInt32Below(0), NextInt32InRange에서 minimum > maximum, NextFloatInRange의
// 비유한 입력·빈 구간)은 platform::Fatal로 프로세스를 종료한다. 같은 프로세스 안에서는 검증할 수
// 없으므로 여기서는 다루지 않는다. 경계 바로 안쪽 입력(bound 1, minimum == maximum, 최소 폭 실수
// 구간)이 정상 동작하는지는 아래 테스트가 확인한다.

namespace {

// 참조 출력: imneme/pcg-c test-high/expected/check-pcg32.out (pcg32-demo와 같은 seed 42, stream
// 54). 데모는 32bit 6개를 출력한 뒤 6단계 되돌려 같은 값을 다시 출력하므로, 동전·주사위 단계가
// 시작될 때의 상태는 6개를 한 번 뽑은 상태와 같다.
constexpr uint32 ReferenceRound1[] = {0xa15c02b7u, 0x7b47f409u, 0xba1d3330u,
                                      0x83d2f293u, 0xbfa4784bu, 0xcbed606eu};
constexpr char ReferenceCoins[] =
    "HHTTTHTHHHTHTTTHHHHHTTTHHHTHTHTHTTHTTTHHHHHHTTTTHHTTTTTHTTTTTTTHT";
constexpr uint32 ReferenceRolls[] = {3, 4, 1, 1, 2, 2, 3, 2, 4, 3, 2, 4, 3, 3, 5, 2, 3,
                                     1, 3, 1, 5, 1, 4, 1, 5, 6, 4, 6, 6, 2, 6, 3, 3};
constexpr uint32 ReferenceRound2[] = {0x74ab93adu, 0x1c1da000u, 0x494ff896u,
                                      0x34462f2fu, 0xd308a3e5u, 0x0fa83babu};

// stream 0(increment 1)에서 초기화 직후 상태가 0이 되는 seed다. 초기 상태는
// (increment + seed) * M + increment이므로 seed = -increment * M^-1 - increment (mod 2^64)로
// 구했다. 상태 0과 다음 상태 1은 모두 출력이 0이고, 그다음 상태 M + 1은 RandomStream(0, 0)의 첫
// 상태와 같다.
constexpr uint64 ZeroOutputSeed = 0x3f681078cd61d75aull;
constexpr uint32 ThirdOutputAfterZeroSeed = 0xe4c14788u;

// 연속 두 출력이 0xffffffff가 되도록 만든 상태다. 회전량(bit 59..63)을 0으로 두고 bit 27..58의
// xorshift 결과가 모두 1이 되게 bit를 위에서부터 정했다. increment는 다음 상태도 같은 조건을
// 만족하도록 (다음 상태 - state * M)으로 골랐다(홀수).
constexpr RandomStreamState AllOnesState{0, 0x657f2c0000000000ull, 0x07fffe0000000000ull};

constexpr int32 Int32Min = -2147483647 - 1;
constexpr int32 Int32Max = 2147483647;

float FloatFromBits(uint32 bits) noexcept {
    return __builtin_bit_cast(float, bits);
}

uint32 BitsFromFloat(float value) noexcept {
    return __builtin_bit_cast(uint32, value);
}

bool IsFiniteFloat(float value) noexcept {
    return (BitsFromFloat(value) & 0x7f800000u) != 0x7f800000u;
}

// value보다 큰 float 중 가장 작은 값이다(nextafter(value, +inf)). -0의 다음은 +최소 subnormal이다.
float NextFloatUp(float value) noexcept {
    const uint32 bits = BitsFromFloat(value);
    if (bits == 0x80000000u) {
        return FloatFromBits(0x00000001u);
    }
    if ((bits >> 31u) != 0u) {
        return FloatFromBits(bits - 1u);
    }
    return FloatFromBits(bits + 1u);
}

RandomStream AllOnesStream() noexcept {
    RandomStream random(0);
    random.RestoreState(AllOnesState);
    return random;
}

// 같은 bucket 수에서 관측 빈도와 균등 기대 빈도의 chi-square 통계량이다.
double ChiSquare(const uint32* counts, uint32 bucketCount, uint32 sampleCount) noexcept {
    const double expected = static_cast<double>(sampleCount) / static_cast<double>(bucketCount);
    double sum = 0.0;
    for (uint32 index = 0; index < bucketCount; ++index) {
        const double difference = static_cast<double>(counts[index]) - expected;
        sum += difference * difference / expected;
    }
    return sum;
}

} // namespace

// 컴파일 시점 평가도 실행 시점과 같은 값을 만들어야 한다. 정수 경로와 내림 보정 실수 경로를
// 고정한다.
static_assert([] {
    RandomStream random(42, 54);
    const bool first = random.NextUInt32() == 0xa15c02b7u;
    const bool second = random.NextUInt32() == 0x7b47f409u;
    return first && second;
}());
static_assert(RandomStream(42, 54).GetState() == RandomStreamState{42, 54, 0x185706b82c2e03f8ull});
static_assert([] {
    RandomStream random(0);
    random.RestoreState(AllOnesState);
    return random.NextFloatInRange(1.0f, 2.0f) == 0x1.fffffep+0f;
}());

TEST(RandomStreamMatchesPcgReferenceDemo) {
    RandomStream random(42, 54);
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(random.NextUInt32() == ReferenceRound1[index]);
    }

    // 동전·주사위는 참조 구현의 bounded 함수 결과다. 거절 규칙이나 나머지 사상이 다르면 여기서
    // 어긋나고, 소비 횟수가 다르면 아래 round 2 값이 어긋난다.
    for (uint32 index = 0; index < 65; ++index) {
        const char side = random.NextUInt32Below(2) != 0u ? 'H' : 'T';
        CHECK(side == ReferenceCoins[index]);
    }
    for (uint32 index = 0; index < 33; ++index) {
        CHECK(random.NextUInt32Below(6) + 1u == ReferenceRolls[index]);
    }

    // 데모의 52장 카드 섞기는 count = 52..2에 대해 bounded(count)를 한 번씩 호출한다.
    for (uint32 count = 52; count > 1; --count) {
        CHECK(random.NextUInt32Below(count) < count);
    }
    for (uint32 index = 0; index < 6; ++index) {
        CHECK(random.NextUInt32() == ReferenceRound2[index]);
    }
}

TEST(RandomStreamGoldenSequencesFromIndependentOracle) {
    // 아래 값은 PCG32 공개 설명으로 따로 작성한 Python 구현에서 얻었다. 이 구현은 위 참조 데모
    // 출력(round 1·2, 동전, 주사위)을 모두 재현한다.
    RandomStream defaultZero(0);
    const uint32 expectedDefaultZero[] = {0xe823a24eu, 0x7a7ecbd9u, 0x89fd6c06u, 0xae646aa8u};
    for (uint32 expected : expectedDefaultZero) {
        CHECK(defaultZero.NextUInt32() == expected);
    }

    RandomStream defaultFortyTwo(42);
    const uint32 expectedDefaultFortyTwo[] = {0xc2f57bd6u, 0x6b07c4a9u, 0x72b7b29bu, 0x44215383u};
    for (uint32 expected : expectedDefaultFortyTwo) {
        CHECK(defaultFortyTwo.NextUInt32() == expected);
    }

    RandomStream wide(0x0123456789abcdefull, 0xfedcba9876543210ull);
    const uint32 expectedWide[] = {0xc7d1489eu, 0x038cf20cu, 0xa6447e87u, 0x3d5a66c3u};
    for (uint32 expected : expectedWide) {
        CHECK(wide.NextUInt32() == expected);
    }

    RandomStream zeroStream(0, 0);
    const uint32 expectedZeroStream[] = {0xe4c14788u, 0x379c6516u, 0x5c4ab3bbu, 0x601d23e0u};
    for (uint32 expected : expectedZeroStream) {
        CHECK(zeroStream.NextUInt32() == expected);
    }

    CHECK(RandomStream(42, 54).GetState() == (RandomStreamState{42, 54, 0x185706b82c2e03f8ull}));
}

TEST(RandomStreamSameSeedCopyAndRestoreContinueIdentically) {
    RandomStream first(1234, 5);
    RandomStream second(1234, 5);
    for (uint32 index = 0; index < 1000; ++index) {
        CHECK(first.NextUInt32() == second.NextUInt32());
    }

    // 임의 위치에서 복사·상태 복원한 사본은 이후 모든 API 호출에서 원본과 같은 값을 만든다.
    for (uint32 index = 0; index < 37; ++index) {
        first.NextUInt32();
    }
    RandomStream copy = first;
    RandomStream restored(999, 999);
    restored.RestoreState(first.GetState());
    CHECK(restored.GetState() == first.GetState());
    for (uint32 index = 0; index < 200; ++index) {
        const uint32 bound = index + 1u;
        const uint32 value = first.NextUInt32Below(bound);
        CHECK(copy.NextUInt32Below(bound) == value);
        CHECK(restored.NextUInt32Below(bound) == value);

        const double unit = first.NextDouble01();
        CHECK(copy.NextDouble01() == unit);
        CHECK(restored.NextDouble01() == unit);

        const float ranged = first.NextFloatInRange(-3.0f, 7.5f);
        CHECK(copy.NextFloatInRange(-3.0f, 7.5f) == ranged);
        CHECK(restored.NextFloatInRange(-3.0f, 7.5f) == ranged);
    }
    CHECK(copy.GetState() == first.GetState());

    // Reset은 현재 seed·stream의 첫 위치로 돌아간다. 복원한 사본은 복원한 seed를 기준으로 한다.
    RandomStream fresh(1234, 5);
    first.Reset();
    restored.Reset();
    CHECK(first.GetState() == fresh.GetState());
    CHECK(restored.GetState() == fresh.GetState());
    for (uint32 index = 0; index < 16; ++index) {
        const uint32 value = fresh.NextUInt32();
        CHECK(first.NextUInt32() == value);
        CHECK(restored.NextUInt32() == value);
    }

    // Seed(a, b)는 RandomStream(a, b)와 같다. stream을 생략하면 이전 stream이 아니라 기본값이다.
    RandomStream reseeded(1, 99);
    reseeded.Seed(77, 3);
    CHECK(reseeded.GetState() == RandomStream(77, 3).GetState());
    reseeded.Seed(7);
    CHECK(reseeded.GetState() == RandomStream(7).GetState());
    CHECK(reseeded.GetState().stream == RandomStream::DefaultStream);
}

TEST(RandomStreamDistinctStreamsAndSeedsProduceDistinctSequences) {
    constexpr uint32 Count = 16;
    uint32 byStream[Count][4] = {};
    uint32 bySeed[Count][4] = {};
    for (uint32 index = 0; index < Count; ++index) {
        RandomStream streamVariant(99, index);
        RandomStream seedVariant(index);
        for (uint32 draw = 0; draw < 4; ++draw) {
            byStream[index][draw] = streamVariant.NextUInt32();
            bySeed[index][draw] = seedVariant.NextUInt32();
        }
    }
    for (uint32 left = 0; left < Count; ++left) {
        for (uint32 right = left + 1; right < Count; ++right) {
            bool streamDiffers = false;
            bool seedDiffers = false;
            for (uint32 draw = 0; draw < 4; ++draw) {
                streamDiffers = streamDiffers || byStream[left][draw] != byStream[right][draw];
                seedDiffers = seedDiffers || bySeed[left][draw] != bySeed[right][draw];
            }
            CHECK(streamDiffers);
            CHECK(seedDiffers);
        }
    }

    // increment = (stream << 1) | 1이므로 stream 최상위 bit는 버려진다. 문서화한 동치와 정규화를
    // 고정한다.
    RandomStream low(5, 3);
    RandomStream high(5, 3ull | (1ull << 63u));
    CHECK(high.GetState().stream == 3u);
    CHECK(low.GetState() == high.GetState());
    for (uint32 index = 0; index < 64; ++index) {
        CHECK(low.NextUInt32() == high.NextUInt32());
    }
}

TEST(RandomStreamUInt32BelowStaysInRangeAndConsumesDocumentedDraws) {
    const uint32 bounds[] = {1u, 2u, 3u, 0x80000001u, 0xffffffffu};
    RandomStream random(0xb0b, 1);
    for (uint32 bound : bounds) {
        for (uint32 index = 0; index < 4096; ++index) {
            const uint32 value = random.NextUInt32Below(bound);
            CHECK(value < bound);
            if (bound == 1u) {
                CHECK(value == 0u);
            }
        }
    }

    // bound 1과 2의 거듭제곱은 거절이 없으므로 정확히 1회 소비하고 출력 하위 bit를 그대로 쓴다.
    RandomStream subject(0xfeed, 4);
    RandomStream twin(0xfeed, 4);
    for (uint32 shift = 0; shift < 32; ++shift) {
        const uint32 bound = 1u << shift;
        CHECK(subject.NextUInt32Below(bound) == (twin.NextUInt32() & (bound - 1u)));
    }
    CHECK(subject.GetState() == twin.GetState());

    // 거절 경로: bound 3의 기준값은 2^32 mod 3 = 1이라 출력 0만 버린다. ZeroOutputSeed는 0을 두 번
    // 연속 출력하므로 두 번 거절하고 세 번째 출력의 나머지를 반환해야 한다.
    RandomStream rejecting(ZeroOutputSeed, 0);
    CHECK(rejecting.NextUInt32Below(3) == ThirdOutputAfterZeroSeed % 3u);
    RandomStream rejectingTwin(ZeroOutputSeed, 0);
    CHECK(rejectingTwin.NextUInt32() == 0u);
    CHECK(rejectingTwin.NextUInt32() == 0u);
    CHECK(rejectingTwin.NextUInt32() == ThirdOutputAfterZeroSeed);
    CHECK(rejecting.GetState() == rejectingTwin.GetState());

    RandomStream rejectingMax(ZeroOutputSeed, 0);
    CHECK(rejectingMax.NextUInt32Below(0xffffffffu) == ThirdOutputAfterZeroSeed);

    // 출력 0xffffffff는 bound 0xffffffff에서 bound가 아니라 0으로 사상되어야 한다.
    RandomStream allOnes = AllOnesStream();
    CHECK(allOnes.NextUInt32Below(0xffffffffu) == 0u);
}

TEST(RandomStreamUInt32BelowIsUnbiased) {
    // 고정 seed라 결과가 매번 같다. 임계값은 독립 oracle로 계산한 실제 값(최대 10.6)보다 넉넉하다.
    RandomStream random(0x5eed, 7);
    const uint32 bounds[] = {2u, 3u, 6u, 7u, 10u};
    for (uint32 bound : bounds) {
        uint32 counts[10] = {};
        const uint32 samples = bound * 6000u;
        for (uint32 index = 0; index < samples; ++index) {
            const uint32 value = random.NextUInt32Below(bound);
            REQUIRE(value < bound);
            ++counts[value];
        }
        const double degreesOfFreedom = static_cast<double>(bound - 1u);
        CHECK(ChiSquare(counts, bound, samples) < degreesOfFreedom * 4.0 + 20.0);
    }

    // bound = 3 * 2^30에서 나머지 연산만 쓰면 [0, 2^30) 값이 두 번 사상되어 확률이 1/3이 아니라
    // 1/2이 된다(약 15000/30000). 거절 표본은 약 10000이어야 한다. oracle 값은 10162다.
    RandomStream large(0x5eed, 8);
    uint32 lowThird = 0;
    for (uint32 index = 0; index < 30000; ++index) {
        if (large.NextUInt32Below(0xc0000000u) < 0x40000000u) {
            ++lowThird;
        }
    }
    CHECK(lowThird > 9400u && lowThird < 10600u);

    // 2^31 + 1은 출력의 약 절반을 거절한다. 위쪽 절반([2^30, 2^31])도 고르게 나와야 한다. oracle
    // 값은 2022다.
    RandomStream half(0x5eed, 10);
    uint32 upperHalf = 0;
    for (uint32 index = 0; index < 4000; ++index) {
        if (half.NextUInt32Below(0x80000001u) >= 0x40000000u) {
            ++upperHalf;
        }
    }
    CHECK(upperHalf > 1700u && upperHalf < 2300u);
}

TEST(RandomStreamInt32InRangeHandlesFullAndDegenerateRanges) {
    // minimum == maximum은 값을 그대로 반환하되 출력 1회를 소비한다.
    const int32 singletons[] = {Int32Min, -1, 0, 7, Int32Max};
    RandomStream subject(31, 41);
    RandomStream twin(31, 41);
    for (int32 value : singletons) {
        CHECK(subject.NextInt32InRange(value, value) == value);
        twin.NextUInt32();
        CHECK(subject.GetState() == twin.GetState());
    }

    // 전체 범위는 Max - Min + 1이 int32로 표현되지 않는 반례다. 출력 1회를 거절 없이 offset으로
    // 쓰며, 다른 범위와 같은 minimum + offset 사상이므로 결과는 출력의 최상위 bit를 뒤집은 값이다.
    bool sawNegative = false;
    bool sawPositive = false;
    for (uint32 index = 0; index < 1000; ++index) {
        const int32 value = subject.NextInt32InRange(Int32Min, Int32Max);
        CHECK(value == static_cast<int32>(twin.NextUInt32() ^ 0x80000000u));
        sawNegative = sawNegative || value < 0;
        sawPositive = sawPositive || value > 0;
    }
    CHECK(sawNegative && sawPositive);

    for (uint32 index = 0; index < 1000; ++index) {
        CHECK(subject.NextInt32InRange(Int32Min, Int32Max - 1) <= Int32Max - 1);
        CHECK(subject.NextInt32InRange(Int32Min + 1, Int32Max) >= Int32Min + 1);
    }

    bool seen[3] = {};
    for (uint32 index = 0; index < 300; ++index) {
        const int32 value = subject.NextInt32InRange(-1, 1);
        REQUIRE(value >= -1 && value <= 1);
        seen[value + 1] = true;
    }
    CHECK(seen[0] && seen[1] && seen[2]);

    // 독립 oracle 값이다. 양 끝(10, -10 쪽)을 포함하는 사상을 고정한다.
    RandomStream small(42, 54);
    const int32 expectedSmall[] = {8, 8, 10, -3, 0, 6, -5, 5};
    for (int32 expected : expectedSmall) {
        CHECK(small.NextInt32InRange(-10, 10) == expected);
    }
    RandomStream nearFull(42, 54);
    const int32 expectedNearFull[] = {559678135, -79170551, 974992176, 64156307};
    for (int32 expected : expectedNearFull) {
        CHECK(nearFull.NextInt32InRange(Int32Min, Int32Max - 1) == expected);
    }

    // 실행 seed에 따라 달라지는 임의 구간에서도 결과는 항상 양 끝 사이에 있어야 한다.
    RandomStream property(test::CurrentSeed(), 11);
    for (uint32 index = 0; index < 20000; ++index) {
        int32 low = static_cast<int32>(property.NextUInt32());
        int32 high = static_cast<int32>(property.NextUInt32());
        if (low > high) {
            const int32 swap = low;
            low = high;
            high = swap;
        }
        const int32 value = property.NextInt32InRange(low, high);
        CHECK(value >= low && value <= high);
    }
}

TEST(RandomStreamNextBoolUsesTopBit) {
    RandomStream subject(0x5eed, 9);
    RandomStream twin(0x5eed, 9);
    uint32 trueCount = 0;
    for (uint32 index = 0; index < 10000; ++index) {
        const bool value = subject.NextBool();
        CHECK(value == ((twin.NextUInt32() >> 31u) != 0u));
        trueCount += value ? 1u : 0u;
    }
    // oracle 값은 4998이다.
    CHECK(trueCount > 4700u && trueCount < 5300u);
}

TEST(RandomStreamFloat01CoversZeroButNeverOne) {
    CHECK(RandomStream(42, 54).NextFloat01() == 0x1.42b804p-1f);

    RandomStream zero(ZeroOutputSeed, 0);
    const float zeroValue = zero.NextFloat01();
    CHECK(BitsFromFloat(zeroValue) == 0u);

    // 상위 24bit가 모두 1인 최댓값은 1 - 2^-24다. 32bit 전체를 2^-32로 곱하면 1.0f로 반올림되는
    // 반례다.
    RandomStream allOnes = AllOnesStream();
    const float maximum = allOnes.NextFloat01();
    CHECK(maximum == 0x1.fffffep-1f);
    CHECK(maximum < 1.0f);

    RandomStream random(2024, 3);
    for (uint32 index = 0; index < 100000; ++index) {
        const float value = random.NextFloat01();
        REQUIRE(value >= 0.0f && value < 1.0f);
        // 결과는 항상 2^-24의 정수배다.
        const float scaled = value * 16777216.0f;
        CHECK(static_cast<float>(static_cast<uint32>(scaled)) == scaled);
    }
}

TEST(RandomStreamDouble01CoversZeroButNeverOne) {
    CHECK(RandomStream(42, 54).NextDouble01() == 0x1.42b8056ef68fep-1);

    RandomStream zero(ZeroOutputSeed, 0);
    CHECK(zero.NextDouble01() == 0.0);

    RandomStream allOnes = AllOnesStream();
    const double maximum = allOnes.NextDouble01();
    CHECK(maximum == 0x1.fffffffffffffp-1);
    CHECK(maximum < 1.0);

    // 첫 출력이 상위 32bit이고 정확히 2회를 소비한다.
    RandomStream subject(77, 8);
    RandomStream twin(77, 8);
    for (uint32 index = 0; index < 1000; ++index) {
        const uint64 high = twin.NextUInt32();
        const uint64 low = twin.NextUInt32();
        const double expected = static_cast<double>(((high << 32u) | low) >> 11u) * 0x1.0p-53;
        const double value = subject.NextDouble01();
        CHECK(value == expected);
        CHECK(value >= 0.0 && value < 1.0);
    }
    CHECK(subject.GetState() == twin.GetState());
}

TEST(RandomStreamFloatInRangeNeverReturnsMaximum) {
    // 폭이 1 ulp인 구간에는 minimum 하나만 속한다. 반올림이 maximum으로 올라가도 내림 보정해야
    // 한다.
    const float one = 1.0f;
    const float aboveOne = NextFloatUp(one);
    const float belowOne = FloatFromBits(BitsFromFloat(one) - 1u);
    const float minSubnormal = FloatFromBits(0x00000001u);
    const float negativeMinSubnormal = FloatFromBits(0x80000001u);
    RandomStream random(55, 66);
    bool sawNegativeSubnormal = false;
    bool sawZero = false;
    for (uint32 index = 0; index < 10000; ++index) {
        CHECK(random.NextFloatInRange(one, aboveOne) == one);
        CHECK(random.NextFloatInRange(belowOne, one) == belowOne);
        CHECK(random.NextFloatInRange(-one, -belowOne) == -one);

        const float tiny = random.NextFloatInRange(negativeMinSubnormal, minSubnormal);
        CHECK(tiny == negativeMinSubnormal || tiny == 0.0f);
        sawNegativeSubnormal = sawNegativeSubnormal || tiny == negativeMinSubnormal;
        sawZero = sawZero || tiny == 0.0f;
    }
    CHECK(sawNegativeSubnormal && sawZero);

    // 반례: u = 1 - 2^-24에서 [1, 2)의 순진한 float 식은 2 - 2^-24를 짝수 반올림해 2.0f를 반환한다.
    const float naive = 1.0f + (2.0f - 1.0f) * 0x1.fffffep-1f;
    CHECK(naive == 2.0f);
    RandomStream positive = AllOnesStream();
    CHECK(positive.NextFloatInRange(1.0f, 2.0f) == 0x1.fffffep+0f);
    RandomStream negative = AllOnesStream();
    CHECK(negative.NextFloatInRange(-2.0f, -1.0f) == -0x1.000002p+0f);

    // float 최대 폭에서도 차이가 double로 유한하며, 양 끝 출력이 각각 -FLT_MAX와 FLT_MAX 미만이
    // 된다.
    const float floatMax = FloatFromBits(0x7f7fffffu);
    RandomStream widest = AllOnesStream();
    CHECK(BitsFromFloat(widest.NextFloatInRange(-floatMax, floatMax)) == 0x7f7ffffdu);
    RandomStream widestZero(ZeroOutputSeed, 0);
    CHECK(widestZero.NextFloatInRange(-floatMax, floatMax) == -floatMax);

    // [0, 1)은 NextFloat01과 같은 값이다.
    RandomStream unitSubject(8, 9);
    RandomStream unitTwin(8, 9);
    for (uint32 index = 0; index < 10000; ++index) {
        CHECK(unitSubject.NextFloatInRange(0.0f, 1.0f) == unitTwin.NextFloat01());
    }

    // 독립 oracle 값이다(double 계산 후 float 내림).
    RandomStream golden(42, 54);
    CHECK(golden.NextFloatInRange(-5.0f, 3.0f) == 0x1.5c02p-5f);
    CHECK(golden.NextFloatInRange(-5.0f, 3.0f) == -0x1.25c06p+0f);
    CHECK(golden.NextFloatInRange(-5.0f, 3.0f) == 0x1.a1d33p-1f);
    CHECK(golden.NextFloatInRange(-5.0f, 3.0f) == -0x1.c2d0ep-1f);
    RandomStream goldenFraction(42, 54);
    CHECK(goldenFraction.NextFloatInRange(0.1f, 1000.5f) == 0x1.3b54c6p+9f);
    CHECK(goldenFraction.NextFloatInRange(0.1f, 1000.5f) == 0x1.e1dbfap+8f);
    CHECK(goldenFraction.NextFloatInRange(0.1f, 1000.5f) == 0x1.6bb30cp+9f);
    CHECK(goldenFraction.NextFloatInRange(0.1f, 1000.5f) == 0x1.019f2ap+9f);
}

TEST(RandomStreamFloatInRangeRandomIntervalsStayHalfOpen) {
    // 실행 seed마다 다른 유한 float bit 패턴으로 구간을 만든다. 크기 차이가 큰 구간, 부호가 다른
    // 구간, subnormal 구간을 모두 포함한다. 결과는 항상 [low, high)이고, 1 ulp 구간에서는 low
    // 하나뿐이다.
    RandomStream bits(test::CurrentSeed(), 12);
    RandomStream random(test::CurrentSeed(), 13);
    uint32 checkedIntervals = 0;
    for (uint32 index = 0; index < 50000; ++index) {
        float low = FloatFromBits(bits.NextUInt32());
        float high = FloatFromBits(bits.NextUInt32());
        if (!IsFiniteFloat(low) || !IsFiniteFloat(high) || low == high) {
            continue;
        }
        if (high < low) {
            const float swap = low;
            low = high;
            high = swap;
        }
        const float value = random.NextFloatInRange(low, high);
        CHECK(value >= low && value < high);

        const float next = NextFloatUp(low);
        if (IsFiniteFloat(next)) {
            CHECK(random.NextFloatInRange(low, next) == low);
        }
        ++checkedIntervals;
    }
    CHECK(checkedIntervals > 40000u);
}
