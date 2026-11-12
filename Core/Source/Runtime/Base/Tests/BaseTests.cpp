#include <Base/Checked.h>
#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Core/Math/RandomStream.h>
#include <TestSupport/Test.h>

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

    // usize 전용 버전은 인자 타입이 섞여도 된다.
    usize size = 0;
    CHECK(TryAdd(MaxSize - 1, 1, size) && size == MaxSize);
    CHECK(!TryAdd(MaxSize, 1, size) && size == MaxSize);
    CHECK(!TryMultiply(MaxSize / 2 + 1, 2, size));

    // 정렬.
    usize aligned = 0;
    CHECK(TryAlignUp(13, 8, aligned) && aligned == 16 && TryAlignUp(16, 8, aligned) &&
          aligned == 16);
    CHECK(TryAlignUp(0, 64, aligned) && aligned == 0);
    CHECK(!TryAlignUp(MaxSize, 2, aligned) && !TryAlignUp(1, 0, aligned) &&
          !TryAlignUp(1, 12, aligned));
    CHECK(IsAligned(64, 16) && !IsAligned(65, 16) && !IsAligned(64, 0) && !IsAligned(64, 3));

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
    // int32 연산을 int64로 계산한 값과 대조한다(오버플로 판정과 결과 모두).
    RandomStream random(test::CurrentSeed() ^ 0xb15eull);
    bool matches = true;
    for (int iteration = 0; iteration < 20000 && matches; ++iteration) {
        const auto left = static_cast<int32>(random.NextUInt32());
        const auto right = static_cast<int32>(iteration % 4 == 0 ? random.NextUInt32() % 65536
                                                                 : random.NextUInt32());
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
    }

    CHECK(matches);
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
