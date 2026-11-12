#include <Base/Checked.h>
#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Core/Math/RandomStream.h>
#include <TestSupport/Test.h>
#include <stdio.h>

using namespace rocks;

// 컴파일 시점에도 같은 규칙이 성립해야 한다.
static_assert([] {
    int8 small = 0;
    return TryAdd<int8>(100, 27, small) && small == 127 && !TryAdd<int8>(100, 28, small) &&
           small == 127;
}());
static_assert(ByteSwap<uint32>(0x11223344u) == 0x44332211u);
static_assert(IsX64 != IsArm64 && IsLittleEndian);

TEST(Base_CheckedArithmeticBoundaries) {
    // 실패하면 output을 바꾸지 않는다.
    uint8 byte = 7;
    CHECK(!TryAdd<uint8>(255, 1, byte) && byte == 7);
    CHECK(TryAdd<uint8>(254, 1, byte) && byte == 255);
    int8 signedByte = 5;
    CHECK(!TryAdd<int8>(-128, -1, signedByte) && signedByte == 5);
    CHECK(TrySubtract<int8>(-127, 1, signedByte) && signedByte == -128);
    uint32 unsignedValue = 9;
    CHECK(!TrySubtract<uint32>(0, 1, unsignedValue) && unsignedValue == 9);
    int64 wide = 0;
    CHECK(!TryMultiply<int64>(0x7fffffffffffffffll, 2, wide));
    CHECK(TryMultiply<int64>(-0x4000000000000000ll, 2, wide) && wide == -0x7fffffffffffffffll - 1);
    CHECK(!TryMultiply<int64>(-0x7fffffffffffffffll - 1, -1, wide));

    // 인자 타입이 output과 달라도 된다. 결과 타입은 output이다.
    usize size = 0;
    CHECK(TryAdd(MaxSize - 1, 1, size) && size == MaxSize);
    CHECK(!TryAdd(MaxSize, 1, size) && size == MaxSize);
    CHECK(!TryMultiply(MaxSize / 2 + 1, 2, size));
    CHECK(TrySubtract(usize{10}, 3, size) && size == 7 && !TrySubtract(usize{2}, 3, size));

    // output 타입으로 표현되지 않는 인자(음수 count 등)는 큰 부호 없는 값이 되지 않고 false다.
    size = 11;
    const int negativeCount = -1;
    CHECK(!TryMultiply(negativeCount, sizeof(char), size) && size == 11);
    CHECK(!TryAdd(usize{0}, -1, size) && !TryAdd(usize{16}, int64{-16}, size) && size == 11);
    int8 small = 3;
    CHECK(!TryAdd(int32{200}, int32{-100}, small) && small == 3); // 200은 int8이 아니다
    CHECK(TryAdd(int32{100}, int32{27}, small) && small == 127);

    // usize·uint64는 플랫폼마다 다른 typedef일 수 있다. 섞어도 모든 플랫폼에서 컴파일된다.
    uint64 wideOutput = 0;
    const uint64 wideInput = 4000;
    CHECK(TryAdd(wideInput, 1, wideOutput) && wideOutput == 4001);
    CHECK(TryMultiply(wideInput, sizeof(int32), wideOutput) && wideOutput == 16000);
    CHECK(TryAdd(wideInput, size, wideOutput) && TryAdd(size, wideInput, size));

    // 정렬.
    usize aligned = 0;
    CHECK(TryAlignUp(13, 8, aligned) && aligned == 16 && TryAlignUp(16, 8, aligned) &&
          aligned == 16);
    CHECK(TryAlignUp(0, 64, aligned) && aligned == 0);
    CHECK(!TryAlignUp(MaxSize, 2, aligned) && !TryAlignUp(1, 0, aligned) &&
          !TryAlignUp(1, 12, aligned));
    CHECK(!TryAlignUp(-1, 8, aligned) && !TryAlignUp(8, -8, aligned) && aligned == 0);
    CHECK(TryAlignUp(wideInput + 1, 4096, wideOutput) && wideOutput == 4096);
    uint8 alignedByte = 0;
    CHECK(TryAlignUp(240, 16, alignedByte) && alignedByte == 240 &&
          !TryAlignUp(241, 16, alignedByte) && alignedByte == 240);
    CHECK(IsAligned(64, 16) && !IsAligned(65, 16) && !IsAligned(64, 0) && !IsAligned(64, 3));
    CHECK(!IsAligned(-64, 16) && IsAligned(wideInput, uint64{32}) && IsAligned(usize{0}, 8));

    // 범위 검사 변환.
    int16 narrow = 1;
    CHECK(!TryNarrow(70000ll, narrow) && narrow == 1);
    CHECK(TryNarrow(-32768ll, narrow) && narrow == -32768);
    uint32 positive = 0;
    CHECK(!TryNarrow(-1, positive) && TryNarrow(4294967295ll, positive) && positive == 4294967295u);
    int32 fromUnsigned = 0;
    CHECK(!TryNarrow(0x80000000u, fromUnsigned) && TryNarrow(0x7fffffffu, fromUnsigned));
    int8 tiny = 0;
    CHECK(TryNarrow(uint64{5}, tiny) && tiny == 5 && !TryNarrow(uint64{200}, tiny));

    // 시프트.
    uint32 shifted = 0;
    CHECK(TryShiftLeft<uint32>(1, 31, shifted) && shifted == 0x80000000u);
    CHECK(!TryShiftLeft<uint32>(1, 32, shifted) && !TryShiftLeft<uint32>(3, 31, shifted));
    int32 signedShift = 0;
    CHECK(TryShiftLeft<int32>(1, 30, signedShift) && signedShift == 0x40000000);
    CHECK(!TryShiftLeft<int32>(1, 31, signedShift)); // 부호 bit로 넘어간다
    CHECK(!TryShiftLeft<int32>(-1, 1, signedShift));
}

TEST(Base_CheckedArithmeticMatchesWideOracle) {
    // int32 연산을 int64로 계산한 값과 대조한다(오버플로 판정과 결과 모두). 두 피연산자의 bit 폭과
    // 부호를 무작위로 골라 성공·실패 경로가 모두 자주 나오게 한다.
    RandomStream random(test::CurrentSeed() ^ 0xb15eull);
    auto operand = [&random]() {
        const uint32 bits = random.NextUInt32() >> (random.NextUInt32() % 32);
        const auto magnitude = static_cast<int32>(bits >> 1);
        return random.NextUInt32() % 2 == 0 ? magnitude : -magnitude - 1;
    };

    bool matches = true;
    int multiplySuccesses = 0;
    int multiplyFailures = 0;
    for (int iteration = 0; iteration < 20000 && matches; ++iteration) {
        const int32 left = operand();
        const int32 right = operand();
        const int64 sum = static_cast<int64>(left) + right;
        const int64 difference = static_cast<int64>(left) - right;
        const int64 product = static_cast<int64>(left) * right;
        int32 output = 0;
        auto fits = [](int64 value) { return value >= -2147483647ll - 1 && value <= 2147483647ll; };
        matches = TryAdd(left, right, output) == fits(sum) && (!fits(sum) || output == sum);
        matches = matches && TrySubtract(left, right, output) == fits(difference) &&
                  (!fits(difference) || output == difference);
        matches = matches && TryMultiply(left, right, output) == fits(product) &&
                  (!fits(product) || output == product);
        ++(fits(product) ? multiplySuccesses : multiplyFailures);
    }

    CHECK(matches);
    // 곱셈 판정의 양쪽 경로를 충분히 시험했는지 확인한다.
    CHECK(multiplySuccesses > 2000 && multiplyFailures > 2000);
}

TEST(Base_EndianLoadStore) {
    uint8 bytes[8] = {};
    StoreLittleEndian<uint32>(bytes, 0x11223344u);
    CHECK(bytes[0] == 0x44 && bytes[1] == 0x33 && bytes[2] == 0x22 && bytes[3] == 0x11);
    CHECK(LoadLittleEndian<uint32>(bytes) == 0x11223344u);
    StoreBigEndian<uint16>(bytes, 0xabcd);
    CHECK(bytes[0] == 0xab && bytes[1] == 0xcd && LoadBigEndian<uint16>(bytes) == 0xabcd);
    StoreLittleEndian<int64>(bytes, -2);
    CHECK(bytes[0] == 0xfe && bytes[7] == 0xff && LoadLittleEndian<int64>(bytes) == -2);
    StoreBigEndian<uint64>(bytes, 0x0102030405060708ull);
    CHECK(bytes[0] == 1 && bytes[7] == 8 && LoadBigEndian<uint64>(bytes) == 0x0102030405060708ull);
    CHECK(ByteSwap<uint16>(0x1234) == 0x3412 &&
          ByteSwap<uint64>(0x0102030405060708ull) == 0x0807060504030201ull);
    CHECK(ByteSwap<int32>(-2) == static_cast<int32>(0xfeffffff));

    // 정렬되지 않은 위치에서도 읽고 쓴다.
    uint8 unaligned[11] = {};
    StoreLittleEndian<uint64>(unaligned + 3, 0x8877665544332211ull);
    CHECK(LoadLittleEndian<uint64>(unaligned + 3) == 0x8877665544332211ull && unaligned[3] == 0x11);
}

#if !ROCKS_BUILD_RELEASE
// Release가 아닌 구성에서는 최적화 끄기 구간이 컴파일되고 정상 동작해야 한다.
ROCKS_DISABLE_OPTIMIZATION

static int SumWithoutOptimization(int count) {
    int total = 0;
    for (int index = 1; index <= count; ++index) {
        total += index;
    }

    return total;
}

ROCKS_ENABLE_OPTIMIZATION
#endif

TEST(Base_BuildConfigurationMacros) {
    // 빌드 시스템은 정확히 하나의 구성 매크로를 정의한다.
    CHECK(int(IsDebugBuild) + int(IsDevelopmentBuild) + int(IsReleaseBuild) == 1);
    CHECK(IsDebugBuild == (ROCKS_BUILD_DEBUG == 1));
    CHECK(IsDevelopmentBuild == (ROCKS_BUILD_DEVELOPMENT == 1));
    CHECK(IsReleaseBuild == (ROCKS_BUILD_RELEASE == 1));

    // 최적화 여부: Debug(Asan 포함)는 끄고 Development·Release는 켠다(테스트 모듈은 재정의가 없다).
#if defined(__OPTIMIZE__)
    constexpr bool optimized = true;
#else
    constexpr bool optimized = false;
#endif
    CHECK(optimized == !IsDebugBuild);

    // NDEBUG는 Release에서만 정의한다.
#if defined(NDEBUG)
    constexpr bool ndebug = true;
#else
    constexpr bool ndebug = false;
#endif
    CHECK(ndebug == IsReleaseBuild);

#if !ROCKS_BUILD_RELEASE
    CHECK(SumWithoutOptimization(100) == 5050);
#endif

    printf("  build: debug=%d development=%d release=%d optimized=%d\n", int(IsDebugBuild),
           int(IsDevelopmentBuild), int(IsReleaseBuild), int(optimized));
}
