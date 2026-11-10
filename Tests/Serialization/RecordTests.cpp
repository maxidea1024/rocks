#include <Tests/Support/Test.h>
#include <Tests/Support/NFailAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <Serialization/Record.h>

using namespace rocks;

namespace {

constexpr Guid Schema{1, 2};
constexpr Guid First{3, 4};
constexpr Guid Second{5, 6};
constexpr uint8 OneFieldGolden[] = {
    'R', 'O', 'C', 'K', 'S', 'R', 'E', 'C', 1, 0, 0, 0, 1, 0, 0, 0,    0,    0,    0,
    0,   2,   0,   0,   0,   0,   0,   0,   0, 1, 0, 0, 0, 1, 0, 0,    0,    32,   0,
    0,   0,   0,   0,   0,   0,   3,   0,   0, 0, 0, 0, 0, 0, 4, 0,    0,    0,    0,
    0,   0,   0,   2,   0,   0,   0,   4,   0, 0, 0, 0, 0, 0, 0, 0x78, 0x56, 0x34, 0x12};
static_assert(sizeof(OneFieldGolden) == 76);

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

Result<void> RunRecordLifecycle(Allocator& allocator) noexcept {
    ArchiveWriter text(allocator);
    auto encoded = text.WriteString("serialization");
    if (!encoded) {
        return encoded;
    }
    RecordBuilder builder(allocator, Schema, 2);
    auto first = builder.AddField(First, WireType::String, text.View());
    if (!first) {
        return first;
    }
    auto second = builder.AddField(Second, 0xF00Du, text.View());
    if (!second) {
        return second;
    }
    auto bytes = builder.Build();
    if (!bytes) {
        return Result<void>::Failure(bytes.Error());
    }
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 2);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }
    auto field = reader.Value().Find(First);
    if (!field) {
        return Result<void>::Failure(field.Error());
    }
    auto archive = ArchiveReader::Create(field.Value().payload);
    if (!archive) {
        return Result<void>::Failure(archive.Error());
    }
    auto value = archive.Value().ReadString(allocator);
    if (!value) {
        return Result<void>::Failure(value.Error());
    }
    if (value.Value().View() != StringView("serialization") || !archive.Value().AtEnd()) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
    return Result<void>::Success();
}

} // namespace

TEST(RecordEnvelopeAndFieldEncodingMatchIndependentGolden) {
    SystemAllocator allocator;
    RecordBuilder builder(allocator, Schema, 1);
    const uint8 payload[] = {0x78, 0x56, 0x34, 0x12};
    REQUIRE(builder.AddField(First, WireType::U32, payload));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    CHECK(SameBytes(bytes.Value().View(), OneFieldGolden));
    auto reader = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1);
    REQUIRE(reader);
    CHECK(reader.Value().SchemaVersion() == 1);
    CHECK(reader.Value().FieldCount() == 1);
    auto field = reader.Value().Next();
    REQUIRE(field);
    CHECK(field.Value().id == First);
    CHECK(field.Value().wireType == static_cast<uint32>(WireType::U32));
    CHECK(SameBytes(field.Value().payload, payload));
    CHECK(field.Value().payload.Data() == OneFieldGolden + 72);
    auto valueReader = ArchiveReader::Create(field.Value().payload);
    REQUIRE(valueReader);
    auto value = valueReader.Value().ReadU32();
    REQUIRE(value);
    CHECK(value.Value() == 0x12345678);
    CHECK(valueReader.Value().AtEnd());
    auto end = reader.Value().Next();
    CHECK(!end);
    CHECK(end.Error() == ErrorCode::NotFound);
    CHECK(reader.Value().Position() == 1);
}

TEST(RecordUnknownTypesAndInsertionOrderArePreserved) {
    SystemAllocator allocator;
    RecordBuilder builder(allocator, Schema, 7);
    {
        uint8 temporary[] = {9, 8, 7};
        REQUIRE(builder.AddField(Second, 0xF00Du, temporary));
        temporary[0] = 0; // AddField가 caller scratch를 보관하지 않는지 검증한다.
    }
    REQUIRE(builder.AddField(First, WireType::Bytes, {}));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto repeated = builder.Build();
    REQUIRE(repeated);
    CHECK(SameBytes(bytes.Value().View(), repeated.Value().View()));
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 7, 7);
    REQUIRE(reader);
    auto unknown = reader.Value().Next();
    REQUIRE(unknown);
    CHECK(unknown.Value().id == Second);
    CHECK(unknown.Value().wireType == 0xF00Du);
    const uint8 original[] = {9, 8, 7};
    CHECK(SameBytes(unknown.Value().payload, original));
    auto known = reader.Value().Next();
    REQUIRE(known);
    CHECK(known.Value().id == First);
    // TLV 경계와 known 값 검증은 다르다. 빈 Bytes payload는 태그만으로 자동 성공하지 않는다.
    auto archive = ArchiveReader::Create(known.Value().payload);
    REQUIRE(archive);
    CHECK(!archive.Value().ReadBytes());
    auto found = reader.Value().Find(Second);
    REQUIRE(found);
    CHECK(reader.Value().Position() == 2);
    CHECK(found.Value().wireType == 0xF00Du);
}

TEST(RecordRejectsInvalidIdentityAndDuplicateWithoutPartialField) {
    SystemAllocator allocator;
    RecordBuilder invalid(allocator, {}, 1);
    CHECK(!invalid.AddField(First, WireType::Bytes, {}));
    CHECK(!invalid.Build());
    RecordBuilder zeroVersion(allocator, Schema, 0);
    CHECK(!zeroVersion.Build());
    RecordBuilder builder(allocator, Schema, 1);
    CHECK(!builder.AddField({}, WireType::Bytes, {}));
    CHECK(!builder.AddField(First, 0, {}));
    REQUIRE(builder.AddField(First, WireType::Bytes, {}));
    auto duplicate = builder.AddField(First, WireType::U32, {});
    CHECK(!duplicate);
    CHECK(duplicate.Error() == ErrorCode::AlreadyExists);
    CHECK(builder.FieldCount() == 1);
    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 1);
    REQUIRE(reader);
    CHECK(reader.Value().FieldCount() == 1);
    CHECK(!reader.Value().Find({}));
    auto absent = reader.Value().Find(Second);
    CHECK(!absent);
    CHECK(absent.Error() == ErrorCode::NotFound);
    CHECK(reader.Value().Position() == 0);
}

TEST(RecordEveryTruncatedPrefixAndTrailingBytesAreRejected) {
    SystemAllocator allocator;
    for (usize length = 0; length < sizeof(OneFieldGolden); ++length) {
        auto reader = RecordReader::Open(allocator, {OneFieldGolden, length}, Schema, 1, 1);
        CHECK(!reader);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    uint8 trailing[sizeof(OneFieldGolden) + 1] = {};
    for (usize index = 0; index < sizeof(OneFieldGolden); ++index) {
        trailing[index] = OneFieldGolden[index];
    }
    CHECK(!RecordReader::Open(allocator, trailing, Schema, 1, 1));
    CHECK(allocator.Stats().liveAllocations == 0);
}

TEST(RecordMalformedEnvelopeVersionCountLengthAndTagHaveExplicitErrors) {
    SystemAllocator allocator;

    struct Mutation {
        usize offset;
        uint8 value;
        ErrorCode error;
    };

    const Mutation mutations[] = {
        {0, 'X', ErrorCode::InvalidArgument}, {8, 2, ErrorCode::Unsupported},
        {12, 9, ErrorCode::TypeMismatch},     {28, 0, ErrorCode::Unsupported},
        {28, 2, ErrorCode::Unsupported},      {32, 0, ErrorCode::InvalidArgument},
        {32, 2, ErrorCode::InvalidArgument},  {36, 31, ErrorCode::InvalidArgument},
        {60, 0, ErrorCode::InvalidArgument},  {64, 5, ErrorCode::InvalidArgument}};
    for (const Mutation& mutation : mutations) {
        uint8 bytes[sizeof(OneFieldGolden)];
        for (usize index = 0; index < sizeof(bytes); ++index) {
            bytes[index] = OneFieldGolden[index];
        }
        bytes[mutation.offset] = mutation.value;
        auto reader = RecordReader::Open(allocator, bytes, Schema, 1, 1);
        CHECK(!reader);
        CHECK(reader.Error() == mutation.error);
        CHECK(allocator.Stats().liveAllocations == 0);
    }
    CHECK(!RecordReader::Open(allocator, OneFieldGolden, {}, 1, 1));
    CHECK(!RecordReader::Open(allocator, OneFieldGolden, Schema, 0, 1));
    CHECK(!RecordReader::Open(allocator, OneFieldGolden, Schema, 2, 1));
}

TEST(RecordDuplicateWireIdsAreRejectedBeforeReaderPublication) {
    SystemAllocator allocator;
    RecordBuilder builder(allocator, Schema, 1);
    REQUIRE(builder.AddField(First, WireType::U32, {}));
    REQUIRE(builder.AddField(Second, 200, {}));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    for (usize index = 0; index < 16; ++index) {
        bytes.Value()[44 + 28 + index] = bytes.Value()[44 + index];
    }
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 1);
    CHECK(!reader);
    CHECK(reader.Error() == ErrorCode::AlreadyExists);
}

TEST(RecordBuilderAllocationFailuresPreservePriorFieldsAndBuildIsReusable) {
    SystemAllocator allocator;
    for (uint64 failedAttempt = 1; failedAttempt <= 2; ++failedAttempt) {
        {
            test::NFailAllocator failure(allocator);
            RecordBuilder builder(failure, Schema, 1);
            REQUIRE(builder.AddField(First, WireType::Bytes, {}));
            failure.FailOnAttempt(failedAttempt);
            const uint8 large[128] = {};
            auto failed = builder.AddField(Second, WireType::Bytes, large);
            CHECK(!failed);
            CHECK(failed.Error() == ErrorCode::OutOfMemory);
            CHECK(builder.FieldCount() == 1);
            failure.DisableFailure();
            auto previous = builder.Build();
            REQUIRE(previous);
            auto reader = RecordReader::Open(allocator, previous.Value().View(), Schema, 1, 1);
            REQUIRE(reader);
            CHECK(reader.Value().FieldCount() == 1);
            REQUIRE(builder.AddField(Second, WireType::Bytes, large));
            failure.FailOnAttempt(1);
            auto failedBuild = builder.Build();
            CHECK(!failedBuild);
            CHECK(failedBuild.Error() == ErrorCode::OutOfMemory);
            CHECK(builder.FieldCount() == 2);
            failure.DisableFailure();
            REQUIRE(builder.Build());
        }
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(RecordEveryAllocationFailureUnwindsAndRetryPipelineLeaksNothing) {
    SystemAllocator allocator;
    test::NFailAllocator failure(allocator);
    REQUIRE(RunRecordLifecycle(failure));
    const uint64 attempts = failure.Attempts();
    CHECK(attempts >= 6);
    CHECK(allocator.Stats().liveAllocations == 0);
    for (uint64 index = 1; index <= attempts; ++index) {
        failure.FailOnAttempt(index);
        auto result = RunRecordLifecycle(failure);
        CHECK(!result);
        CHECK(result.Error() == ErrorCode::OutOfMemory);
        CHECK(allocator.Stats().liveAllocations == 0);
        failure.DisableFailure();
        REQUIRE(RunRecordLifecycle(failure));
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}

TEST(RecordEmptyHeaderAndCountValueWorkLimitsAreEnforced) {
    SystemAllocator allocator;
    RecordLimits limits;
    limits.archive.maxTotalBytes = 44;
    limits.archive.maxValueBytes = 0;
    limits.maxFields = 0;
    RecordBuilder empty(allocator, Schema, 1, limits);
    auto bytes = empty.Build();
    REQUIRE(bytes);
    CHECK(bytes.Value().Size() == 44);
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 1, limits);
    REQUIRE(reader);
    CHECK(reader.Value().FieldCount() == 0);
    CHECK(!reader.Value().Next());
    CHECK(!empty.AddField(First, WireType::Bytes, {}));
    limits.archive.maxTotalBytes = 43;
    RecordBuilder tooSmall(allocator, Schema, 1, limits);
    auto failed = tooSmall.Build();
    CHECK(!failed);
    CHECK(failed.Error() == ErrorCode::CapacityExceeded);

    limits = {};
    limits.maxFields = 0;
    auto count = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1, limits);
    CHECK(!count);
    CHECK(count.Error() == ErrorCode::CapacityExceeded);
    limits = {};
    limits.archive.maxValueBytes = 3;
    auto value = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1, limits);
    CHECK(!value);
    CHECK(value.Error() == ErrorCode::CapacityExceeded);

    limits = {};
    limits.archive.maxWork = 1;
    auto work = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1, limits);
    CHECK(!work);
    CHECK(work.Error() == ErrorCode::CapacityExceeded);
    CHECK(allocator.Stats().liveAllocations == 1); // 빈 header 소유 Array만 남는다.
}

TEST(RecordIterationBudgetFailureKeepsPositionAndMoveTransfersIndex) {
    SystemAllocator allocator;
    uint64 openWork = 0;
    {
        auto baseline = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1);
        REQUIRE(baseline);
        openWork = baseline.Value().WorkUsed();
    }
    RecordLimits limits;
    limits.archive.maxWork = openWork;
    auto reader = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1, limits);
    REQUIRE(reader);
    auto next = reader.Value().Next();
    CHECK(!next);
    CHECK(next.Error() == ErrorCode::CapacityExceeded);
    CHECK(reader.Value().Position() == 0);

    auto moving = RecordReader::Open(allocator, OneFieldGolden, Schema, 1, 1);
    REQUIRE(moving);
    REQUIRE(moving.Value().Next());
    RecordReader moved(Move(moving.Value()));
    CHECK(moved.Position() == 1);
    CHECK(moved.FieldCount() == 1);
    CHECK(moving.Value().FieldCount() == 0);
    CHECK(moving.Value().Position() == 0);
    CHECK(!moving.Value().Next());
    CHECK(!moved.Next());
}

TEST(RecordDeterministicByteMutationHarnessStaysBoundedAndReencodesAcceptedInput) {
    SystemAllocator allocator;
    uint32 random = 0x6d2b79f5u;
    for (uint32 iteration = 0; iteration < 2048; ++iteration) {
        {
            uint8 bytes[sizeof(OneFieldGolden)];
            for (usize index = 0; index < sizeof(bytes); ++index) {
                bytes[index] = OneFieldGolden[index];
            }
            random = random * 1664525u + 1013904223u;
            const usize offset = (random >> 16) % sizeof(bytes);
            random = random * 1664525u + 1013904223u;
            bytes[offset] ^= static_cast<uint8>((random >> 24) | 1u);
            RecordLimits limits;
            limits.archive.maxTotalBytes = sizeof(bytes);
            limits.archive.maxValueBytes = sizeof(bytes);
            limits.archive.maxOperations = 32;
            limits.archive.maxWork = 512;
            limits.maxFields = 4;
            auto reader = RecordReader::Open(allocator, bytes, Schema, 1, 1, limits);
            if (reader) {
                RecordBuilder rebuilt(allocator, Schema, reader.Value().SchemaVersion(), limits);
                for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
                    auto field = reader.Value().Next();
                    REQUIRE(field);
                    REQUIRE(rebuilt.AddField(field.Value().id, field.Value().wireType,
                                             field.Value().payload));
                }
                auto encoded = rebuilt.Build();
                REQUIRE(encoded);
                CHECK(SameBytes(encoded.Value().View(), bytes));
                CHECK(reader.Value().WorkUsed() <= limits.archive.maxWork);
            } else {
                CHECK(reader.Error() != ErrorCode::None);
            }
        }
        CHECK(allocator.Stats().liveAllocations == 0);
    }
}
