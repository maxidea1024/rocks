#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Archive.h>
#include <string.h>

using namespace rocks;

namespace {

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }
    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

template <typename T>
void ExpectValue(Result<T> result, T expected) noexcept {
    REQUIRE(result);
    CHECK(result.Value() == expected);
}

constexpr uint8 PrimitiveGolden[] = {0,    1,    0x44, 0x33, 0x22, 0x11, 0x11, 0x22, 0x33, 0x44,
                                     0x55, 0x66, 0x77, 0x88, 0xfe, 0xff, 0xff, 0xff, 0xff, 0xff,
                                     0xff, 0xff, 0,    0,    0xc0, 0x3f, 0,    0,    0,    0,
                                     0,    0,    2,    0xc0, 8,    7,    6,    5,    4,    3,
                                     2,    1,    0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11};

} // namespace

TEST(ArchivePrimitiveEncodingMatchesIndependentLittleEndianGolden) {
    SystemAllocator allocator;
    ArchiveWriter writer(allocator);
    REQUIRE(writer.WriteBool(false));
    REQUIRE(writer.WriteBool(true));
    REQUIRE(writer.WriteU32(0x11223344));
    REQUIRE(writer.WriteU64(0x8877665544332211ULL));
    REQUIRE(writer.WriteI64(-2));
    REQUIRE(writer.WriteF32(1.5f));
    REQUIRE(writer.WriteF64(-2.25));
    REQUIRE(writer.WriteGuid({0x0102030405060708ULL, 0x1112131415161718ULL}));
    CHECK(SameBytes(writer.View(), PrimitiveGolden));

    // writer 출력이 아닌 고정 golden을 읽어 encoder/decoder가 같은 오류를 상쇄하지 못하게 한다.
    auto reader = ArchiveReader::Create(PrimitiveGolden);
    REQUIRE(reader);
    ExpectValue(reader.Value().ReadBool(), false);
    ExpectValue(reader.Value().ReadBool(), true);
    ExpectValue(reader.Value().ReadU32(), uint32{0x11223344});
    ExpectValue(reader.Value().ReadU64(), uint64{0x8877665544332211ULL});
    ExpectValue(reader.Value().ReadI64(), int64{-2});
    ExpectValue(reader.Value().ReadF32(), 1.5f);
    ExpectValue(reader.Value().ReadF64(), -2.25);
    ExpectValue(reader.Value().ReadGuid(), Guid{0x0102030405060708ULL, 0x1112131415161718ULL});
    CHECK(reader.Value().AtEnd());
}

TEST(ArchivePreservesFloatBitsAndSignedIntegerExtremes) {
    SystemAllocator allocator;
    constexpr uint32 floatBits[] = {0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc12345u};
    constexpr uint64 doubleBits[] = {0x8000000000000000ULL, 0x7ff0000000000000ULL,
                                     0xfff0000000000000ULL, 0x7ff8123456789abcULL};
    ArchiveWriter writer(allocator);
    for (uint32 bits : floatBits) {
        float value = 0;
        memcpy(&value, &bits, sizeof(value));
        REQUIRE(writer.WriteF32(value));
    }
    for (uint64 bits : doubleBits) {
        double value = 0;
        memcpy(&value, &bits, sizeof(value));
        REQUIRE(writer.WriteF64(value));
    }
    constexpr int64 maximum = 0x7fffffffffffffffLL;
    constexpr int64 minimum = -maximum - 1;
    REQUIRE(writer.WriteI64(minimum));
    REQUIRE(writer.WriteI64(maximum));
    auto reader = ArchiveReader::Create(writer.View());
    REQUIRE(reader);
    for (uint32 expected : floatBits) {
        auto decoded = reader.Value().ReadF32();
        REQUIRE(decoded);
        uint32 bits = 0;
        const float value = decoded.Value();
        memcpy(&bits, &value, sizeof(bits));
        CHECK(bits == expected);
    }
    for (uint64 expected : doubleBits) {
        auto decoded = reader.Value().ReadF64();
        REQUIRE(decoded);
        uint64 bits = 0;
        const double value = decoded.Value();
        memcpy(&bits, &value, sizeof(bits));
        CHECK(bits == expected);
    }
    ExpectValue(reader.Value().ReadI64(), minimum);
    ExpectValue(reader.Value().ReadI64(), maximum);
    CHECK(reader.Value().AtEnd());
}

TEST(ArchiveUtf8EmbeddedNullAndOwnedStringOutliveSource) {
    SystemAllocator allocator;
    const char utf8[] = {'A', 0, '\xED', '\x95', '\x9C', '\xF0', '\x9F', '\x8E', '\xAE'};
    String owned(allocator);
    {
        ArchiveWriter writer(allocator);
        REQUIRE(writer.WriteString({utf8, sizeof(utf8)}));
        REQUIRE(writer.WriteBytes({}));
        CHECK(writer.View()[0] == sizeof(utf8));
        for (usize index = 1; index < 8; ++index) {
            CHECK(writer.View()[index] == 0);
        }
        auto reader = ArchiveReader::Create(writer.View());
        REQUIRE(reader);
        auto value = reader.Value().ReadString(allocator);
        REQUIRE(value);
        owned = Move(value.Value());
        auto empty = reader.Value().ReadBytes();
        REQUIRE(empty);
        CHECK(empty.Value().Size() == 0);
        CHECK(reader.Value().AtEnd());
    }
    CHECK(owned.View() == StringView(utf8, sizeof(utf8)));
    CHECK(owned.CStr()[owned.ByteCount()] == 0);
}

TEST(ArchiveStringValidationAndTruncationNeverAdvanceCursor) {
    // overlong, surrogate, 범위 초과, continuation 단독, 잘린 scalar 각각을 거부한다.
    const uint8 malformed[][4] = {{0xc0, 0x80, 0, 0},
                                  {0xed, 0xa0, 0x80, 0},
                                  {0xf4, 0x90, 0x80, 0x80},
                                  {0x80, 0, 0, 0},
                                  {0xf0, 0x9f, 0, 0}};
    const usize lengths[] = {2, 3, 4, 1, 2};
    SystemAllocator allocator;
    for (usize index = 0; index < 5; ++index) {
        ArchiveWriter writer(allocator);
        REQUIRE(writer.WriteBytes({malformed[index], lengths[index]}));
        auto reader = ArchiveReader::Create(writer.View());
        REQUIRE(reader);
        auto invalid = reader.Value().ReadStringView();
        CHECK(!invalid);
        CHECK(invalid.Error() == ErrorCode::InvalidArgument);
        CHECK(reader.Value().Position() == 0);
        CHECK(reader.Value().WorkUsed() != 0);
        auto bytes = reader.Value().ReadBytes();
        REQUIRE(bytes);
        CHECK(SameBytes(bytes.Value(), {malformed[index], lengths[index]}));

        const usize previousSize = writer.ByteCount();
        auto rejected =
            writer.WriteString({reinterpret_cast<const char*>(malformed[index]), lengths[index]});
        CHECK(!rejected);
        CHECK(rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(writer.ByteCount() == previousSize);
    }
    for (usize length = 0; length < sizeof(PrimitiveGolden); ++length) {
        auto reader = ArchiveReader::Create({PrimitiveGolden, length});
        REQUIRE(reader);
        auto raw = reader.Value().ReadRaw(sizeof(PrimitiveGolden));
        CHECK(!raw);
        CHECK(reader.Value().Position() == 0);
    }
    const uint8 badBool[] = {2};
    auto boolReader = ArchiveReader::Create(badBool);
    REQUIRE(boolReader);
    auto boolean = boolReader.Value().ReadBool();
    CHECK(!boolean);
    CHECK(boolReader.Value().Position() == 0);
}

TEST(ArchiveCompoundGuidAndLengthPrefixFailuresAreAtomic) {
    uint8 bytes[16] = {};
    for (usize length = 0; length < 16; ++length) {
        auto reader = ArchiveReader::Create({bytes, length});
        REQUIRE(reader);
        auto guid = reader.Value().ReadGuid();
        CHECK(!guid);
        CHECK(reader.Value().Position() == 0);
    }
    // 길이 prefix는 읽을 수 있어도 실제 payload가 부족하면 전체 연산을 commit하지 않는다.
    bytes[0] = 9;
    auto reader = ArchiveReader::Create(bytes);
    REQUIRE(reader);
    auto value = reader.Value().ReadBytes();
    CHECK(!value);
    CHECK(value.Error() == ErrorCode::InvalidArgument);
    CHECK(reader.Value().Position() == 0);
    for (usize index = 0; index < 8; ++index) {
        bytes[index] = 0xff;
    }
    auto hugeReader = ArchiveReader::Create(bytes);
    REQUIRE(hugeReader);
    auto huge = hugeReader.Value().ReadBytes();
    CHECK(!huge);
    CHECK(huge.Error() == ErrorCode::CapacityExceeded);
    CHECK(hugeReader.Value().Position() == 0);
}

TEST(ArchiveSelfAppendSurvivesGrowthAndTakeBytesRetainsOwnership) {
    SystemAllocator allocator;
    {
        ArchiveWriter writer(allocator);
        const uint8 seed[] = {1, 2, 3, 4};
        REQUIRE(writer.WriteRaw(seed));
        REQUIRE(writer.WriteBytes(writer.View()));
        REQUIRE(writer.WriteRaw({writer.View().Data() + 1, 3}));
        const uint8 expected[] = {1, 2, 3, 4, 4, 0, 0, 0, 0, 0, 0, 0, 1, 2, 3, 4, 2, 3, 4};
        CHECK(SameBytes(writer.View(), expected));
        const uint64 operations = writer.OperationsUsed();
        auto owned = writer.TakeBytes();
        CHECK(writer.ByteCount() == 0);
        CHECK(writer.OperationsUsed() == operations);
        CHECK(SameBytes(owned.View(), expected));
        REQUIRE(writer.WriteBool(true));
        CHECK(SameBytes(owned.View(), expected));
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArchiveWriterOutOfMemoryPreservesBytesAndReaderOutOfMemoryPreservesCursor) {
    SystemAllocator allocator;
    {
        test::NFailAllocator failure(allocator);
        ArchiveWriter writer(failure);
        REQUIRE(writer.WriteU32(0x11223344));
        const uint8* oldAddress = writer.View().Data();
        const uint8 seed[] = {0x44, 0x33, 0x22, 0x11};
        uint8 large[128] = {};
        failure.FailOnAttempt(1);
        auto appended = writer.WriteBytes(large);
        CHECK(!appended);
        CHECK(appended.Error() == ErrorCode::OutOfMemory);
        CHECK(writer.View().Data() == oldAddress);
        CHECK(SameBytes(writer.View(), seed));
        failure.DisableFailure();
        REQUIRE(writer.WriteBytes(large));

        ArchiveWriter stringWriter(allocator);
        REQUIRE(stringWriter.WriteString("owned"));
        auto reader = ArchiveReader::Create(stringWriter.View());
        REQUIRE(reader);
        failure.FailOnAttempt(1);
        auto failedString = reader.Value().ReadString(failure);
        CHECK(!failedString);
        CHECK(failedString.Error() == ErrorCode::OutOfMemory);
        CHECK(reader.Value().Position() == 0);
        const uint64 used = reader.Value().WorkUsed();
        failure.DisableFailure();
        auto retry = reader.Value().ReadString(failure);
        REQUIRE(retry);
        CHECK(retry.Value().View() == StringView("owned"));
        CHECK(reader.Value().WorkUsed() > used);
    }
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(ArchiveLimitsCountFailedAttemptsAndDoNotRefundWork) {
    SystemAllocator allocator;
    ArchiveLimits limits;
    limits.maxWork = 4;
    ArchiveWriter writer(allocator, limits);
    auto failed = writer.WriteU32(1); // operation 1 + 고정 값 4 byte = 5
    CHECK(!failed);
    CHECK(failed.Error() == ErrorCode::CapacityExceeded);
    CHECK(writer.ByteCount() == 0);
    CHECK(writer.WorkUsed() == 4);
    CHECK(!writer.WriteBool(true));
    limits.maxWork = 5;
    ArchiveWriter exact(allocator, limits);
    REQUIRE(exact.WriteU32(1));
    CHECK(exact.WorkUsed() == 5);

    const uint8 invalidBool[] = {7};
    limits.maxWork = 100;
    limits.maxOperations = 2;
    auto reader = ArchiveReader::Create(invalidBool, limits);
    REQUIRE(reader);
    CHECK(!reader.Value().ReadBool());
    CHECK(!reader.Value().ReadBool());
    auto exhausted = reader.Value().ReadBool();
    CHECK(!exhausted);
    CHECK(exhausted.Error() == ErrorCode::CapacityExceeded);
    CHECK(reader.Value().OperationsUsed() == 2);
    CHECK(reader.Value().Position() == 0);

    limits.maxOperations = 100;
    limits.maxTotalBytes = 3;
    auto oversized = ArchiveReader::Create(PrimitiveGolden, limits);
    CHECK(!oversized);
    ArchiveWriter bounded(allocator, limits);
    CHECK(!bounded.WriteU32(1));
    CHECK(bounded.ByteCount() == 0);
}

TEST(ArchiveZeroDynamicLimitAllowsFixedPrimitivesAndEmptyValues) {
    SystemAllocator allocator;
    ArchiveLimits limits;
    limits.maxValueBytes = 0;
    ArchiveWriter writer(allocator, limits);
    REQUIRE(writer.WriteGuid({1, 2}));
    REQUIRE(writer.WriteBytes({}));
    REQUIRE(writer.WriteString({}));
    const uint8 one[] = {0};
    CHECK(!writer.WriteBytes(one));
    auto reader = ArchiveReader::Create(writer.View(), limits);
    REQUIRE(reader);
    ExpectValue(reader.Value().ReadGuid(), Guid{1, 2});
    auto bytes = reader.Value().ReadBytes();
    REQUIRE(bytes);
    CHECK(bytes.Value().Size() == 0);
    auto string = reader.Value().ReadStringView();
    REQUIRE(string);
    CHECK(string.Value().IsEmpty());
    CHECK(reader.Value().AtEnd());
}
