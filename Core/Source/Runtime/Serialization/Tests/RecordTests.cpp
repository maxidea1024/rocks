#include <TestSupport/Test.h>
#include <TestSupport/NFailAllocator.h>
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
        {0, 'X', ErrorCode::InvalidArgument},
        {8, 2, ErrorCode::Unsupported},
        // schema version 0은 쓰지 않는 잘못된 값이라 범위 밖(Unsupported)과 구분한다.
        {12, 9, ErrorCode::TypeMismatch},
        {28, 0, ErrorCode::InvalidArgument},
        {28, 2, ErrorCode::Unsupported},
        {32, 0, ErrorCode::InvalidArgument},
        {32, 2, ErrorCode::InvalidArgument},
        {36, 31, ErrorCode::InvalidArgument},
        {60, 0, ErrorCode::InvalidArgument},
        {64, 5, ErrorCode::InvalidArgument}};
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

TEST(RecordManyFieldsUseSortedIndexForDuplicatesAndFind) {
    // 최대 필드 수(4096)의 Record: 중복 검사는 정렬 index(O(n log n))라 work가 n²에 비례하지 않고,
    // Find는 이진 탐색이며 Next는 기록 순서를 지킨다. id는 기록 순서와 정렬 순서가 다르고 high가
    // 겹치게(low로 갈리게) 섞는다.
    SystemAllocator allocator;
    constexpr uint32 Count = 4096;
    auto idAt = [](uint32 index) noexcept {
        const uint32 mixed = (index * 1237u) % Count; // 1237은 홀수라 순열이다
        return Guid{mixed % 5u + 1u, static_cast<uint64>(mixed) + 1u};
    };

    RecordBuilder builder(allocator, Schema, 1);
    for (uint32 index = 0; index < Count; ++index) {
        REQUIRE(builder.AddField(idAt(index), WireType::Bytes, {}));
    }

    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto reader = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 1);
    REQUIRE(reader);
    // n² 비교였다면 약 840만 work다. 정렬 index는 파싱 byte와 2·n·13 + n 정도다.
    CHECK(reader.Value().WorkUsed() < 400000);
    for (uint32 index = 0; index < 3; ++index) {
        auto next = reader.Value().Next();
        CHECK(next && next.Value().id == idAt(index));
    }

    bool allFound = true;
    uint64 maxFindWork = 0;
    for (uint32 index = 0; index < Count; ++index) {
        const uint64 before = reader.Value().WorkUsed();
        auto found = reader.Value().Find(idAt(index));
        allFound = allFound && found && found.Value().id == idAt(index);
        const uint64 spent = reader.Value().WorkUsed() - before;
        maxFindWork = spent > maxFindWork ? spent : maxFindWork;
    }

    CHECK(allFound);
    CHECK(maxFindWork <= 16); // 이진 탐색 상한(13 + 1) + 호출 비용
    CHECK(reader.Value().Find(Guid{0, 1}).Error() == ErrorCode::NotFound);      // 가장 작은 쪽 밖
    CHECK(reader.Value().Find(Guid{3, 999999}).Error() == ErrorCode::NotFound); // 중간의 빈 자리
    CHECK(reader.Value().Find(Guid{9, 1}).Error() == ErrorCode::NotFound);      // 가장 큰 쪽 밖

    // 첫 필드와 마지막 필드(이웃하지 않은 기록 위치)의 중복도 찾는다.
    const usize last = 44 + static_cast<usize>(Count - 1) * 28;
    for (usize index = 0; index < 16; ++index) {
        bytes.Value()[last + index] = bytes.Value()[44 + index];
    }

    auto duplicate = RecordReader::Open(allocator, bytes.Value().View(), Schema, 1, 1);
    CHECK(!duplicate && duplicate.Error() == ErrorCode::AlreadyExists);
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
    // 빈 header 소유 Array와 위에서 연 빈 Record reader의 트리 예산(SharedPtr)만 남는다. 실패한
    // Open은 아무것도 남기지 않는다.
    CHECK(allocator.Stats().liveAllocations == 2);
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

namespace {

constexpr Guid Inner{7, 8};
constexpr Guid Leaf{9, 10};

// depth단 중첩 Record를 만든다: 가장 안쪽은 Leaf 필드(bytes 길이의 Bytes) 하나, 바깥 단은 First
// 필드에 안쪽 Record를 담는다.
Result<Array<uint8>> NestedRecord(Allocator& allocator, usize depth, usize bytes) noexcept {
    Array<uint8> current(allocator);
    {
        Array<uint8> filler(allocator);
        if (auto reserved = filler.TryReserve(bytes); !reserved) {
            return Result<Array<uint8>>::Failure(reserved.Error());
        }

        for (usize index = 0; index < bytes; ++index) {
            if (!filler.TryPushBack(static_cast<uint8>(index))) {
                return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
            }
        }

        ArchiveWriter value(allocator);
        if (auto written = value.WriteBytes(filler.View()); !written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        RecordBuilder leaf(allocator, Inner, 1);
        if (auto added = leaf.AddField(Leaf, WireType::Bytes, value.View()); !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }

        auto built = leaf.Build();
        if (!built) {
            return built;
        }

        current = Move(built.Value());
    }

    for (usize level = 1; level < depth; ++level) {
        RecordBuilder outer(allocator, Inner, 1);
        if (auto added = outer.AddField(First, WireType::Record, current.View()); !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }

        auto built = outer.Build();
        if (!built) {
            return built;
        }

        current = Move(built.Value());
    }

    return Result<Array<uint8>>::Success(Move(current));
}

} // namespace

// RecordWriter는 바깥 writer 뒤에 제자리로 써서 RecordBuilder::Build와 같은 byte를 만든다(중첩
// Record도 값 byte를 한 번만 쓴다).
TEST(RecordWriterMatchesBuilderBytesInPlace) {
    SystemAllocator allocator;
    const uint8 payload[] = {0x78, 0x56, 0x34, 0x12};
    ArchiveWriter out(allocator);
    REQUIRE(out.WriteU32(0xaabbccdd));
    auto record = RecordWriter::Begin(out, Schema, 1);
    REQUIRE(record);
    REQUIRE(record.Value().BeginField(First));
    REQUIRE(out.WriteRaw(payload));
    REQUIRE(record.Value().EndField(WireType::U32));
    REQUIRE(record.Value().End());
    CHECK(record.Value().FieldCount() == 1);
    CHECK(out.ByteCount() == 4 + sizeof(OneFieldGolden));
    CHECK(SameBytes(Span<const uint8>(out.View().Data() + 4, out.ByteCount() - 4), OneFieldGolden));

    // 중첩: 바깥 Record의 First 필드에 안쪽 Record를 제자리로 쓴다 = 빌더로 따로 만든 것과 같다.
    RecordBuilder inner(allocator, Inner, 2);
    REQUIRE(inner.AddField(Second, WireType::U32, payload));
    auto innerBytes = inner.Build();
    REQUIRE(innerBytes);
    RecordBuilder outer(allocator, Schema, 1);
    REQUIRE(outer.AddField(First, WireType::Record, innerBytes.Value().View()));
    auto expected = outer.Build();
    REQUIRE(expected);
    ArchiveWriter nested(allocator);
    auto outerWriter = RecordWriter::Begin(nested, Schema, 1);
    REQUIRE(outerWriter);
    REQUIRE(outerWriter.Value().BeginField(First));
    auto innerWriter = RecordWriter::Begin(nested, Inner, 2);
    REQUIRE(innerWriter);
    REQUIRE(innerWriter.Value().BeginField(Second));
    REQUIRE(nested.WriteRaw(payload));
    REQUIRE(innerWriter.Value().EndField(WireType::U32));
    REQUIRE(innerWriter.Value().End());
    REQUIRE(outerWriter.Value().EndField(WireType::Record));
    REQUIRE(outerWriter.Value().End());
    CHECK(SameBytes(nested.View(), expected.Value().View()));
}

TEST(RecordWriterRejectsMisuseAndLimits) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    CHECK(RecordWriter::Begin(out, {}, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(RecordWriter::Begin(out, Schema, 0).Error() == ErrorCode::InvalidArgument);
    RecordLimits tiny;
    tiny.archive.maxTotalBytes = 43;
    CHECK(RecordWriter::Begin(out, Schema, 1, tiny).Error() == ErrorCode::CapacityExceeded);

    RecordLimits limits;
    limits.maxFields = 1;
    limits.archive.maxValueBytes = 4;
    limits.archive.maxTotalBytes = 44 + 28 + 4;
    auto record = RecordWriter::Begin(out, Schema, 1, limits);
    REQUIRE(record);
    CHECK(record.Value().EndField(WireType::U32).Error() == ErrorCode::InvalidArgument);
    CHECK(record.Value().BeginField({}).Error() == ErrorCode::InvalidArgument);
    REQUIRE(record.Value().BeginField(First));
    CHECK(record.Value().BeginField(Second).Error() == ErrorCode::InvalidArgument);
    CHECK(record.Value().End().Error() == ErrorCode::InvalidArgument);
    REQUIRE(out.WriteU64(1));
    // 값 8 byte > maxValueBytes 4.
    CHECK(record.Value().EndField(WireType::U64).Error() == ErrorCode::CapacityExceeded);

    ArchiveWriter second(allocator);
    auto fitting = RecordWriter::Begin(second, Schema, 1, limits);
    REQUIRE(fitting);
    REQUIRE(fitting.Value().BeginField(First));
    REQUIRE(second.WriteU32(7));
    CHECK(fitting.Value().EndField(0).Error() == ErrorCode::InvalidArgument);
    REQUIRE(fitting.Value().EndField(WireType::U32));
    // 필드 수 한도 1.
    CHECK(fitting.Value().BeginField(Second).Error() == ErrorCode::CapacityExceeded);
    REQUIRE(fitting.Value().End());

    // 전체 크기 한도.
    limits.maxFields = 4;
    ArchiveWriter third(allocator);
    auto large = RecordWriter::Begin(third, Schema, 1, limits);
    REQUIRE(large);
    for (uint32 index = 0; index < 2; ++index) {
        REQUIRE(large.Value().BeginField(Guid{20, index + 1}));
        REQUIRE(third.WriteU32(index));
        REQUIRE(large.Value().EndField(WireType::U32));
    }

    CHECK(large.Value().End().Error() == ErrorCode::CapacityExceeded);
}

// OpenRecordField의 색인 전용 해석도 안쪽 envelope·TLV 경계·중복 ID·버전 0을 검사한다.
TEST(RecordOpenRecordFieldValidatesInnerEnvelope) {
    SystemAllocator allocator;
    const uint8 payload[] = {1, 2, 3, 4};
    RecordBuilder inner(allocator, Inner, 1);
    REQUIRE(inner.AddField(First, WireType::U32, payload));
    // 둘째 ID는 low byte 하나만 바꾸면 첫 ID({3, 4})와 같아진다.
    REQUIRE(inner.AddField(Guid{3, 5}, WireType::U32, payload));
    auto valid = inner.Build();
    REQUIRE(valid);
    struct Mutation {
        usize offset;
        uint8 value;
        ErrorCode expected;
    };

    // 0..43 header, 44.. 첫 TLV(28 + 4), 76.. 둘째 TLV.
    const Mutation mutations[] = {
        {28, 0, ErrorCode::InvalidArgument},        // schema version 0
        {44 + 20, 200, ErrorCode::InvalidArgument}, // 첫 TLV 길이가 남은 byte보다 크다
        {76 + 8, 4, ErrorCode::AlreadyExists},      // 둘째 ID의 low를 첫 ID와 같게
        {36, 70, ErrorCode::InvalidArgument}};      // payloadBytes 불일치
    for (const Mutation& mutation : mutations) {
        Array<uint8> bytes(allocator);
        REQUIRE(bytes.TryReserve(valid.Value().Size()));
        for (uint8 byte : valid.Value()) {
            REQUIRE(bytes.TryPushBack(byte));
        }

        bytes[mutation.offset] = mutation.value;
        RecordBuilder outer(allocator, Schema, 1);
        REQUIRE(outer.AddField(First, WireType::Record, bytes.View()));
        auto outerBytes = outer.Build();
        REQUIRE(outerBytes);
        auto reader = RecordReader::Open(allocator, outerBytes.Value().View(), Schema, 1, 1);
        REQUIRE(reader);
        auto field = reader.Value().Next();
        REQUIRE(field);
        CHECK(reader.Value().OpenRecordField(allocator, field.Value(), Inner, 1, 1, 16).Error() ==
              mutation.expected);
    }

    // trailing byte가 있는 안쪽 Record.
    Array<uint8> trailing(allocator);
    REQUIRE(trailing.TryReserve(valid.Value().Size() + 1));
    for (uint8 byte : valid.Value()) {
        REQUIRE(trailing.TryPushBack(byte));
    }

    REQUIRE(trailing.TryPushBack(9));
    RecordBuilder outer(allocator, Schema, 1);
    REQUIRE(outer.AddField(First, WireType::Record, trailing.View()));
    auto outerBytes = outer.Build();
    REQUIRE(outerBytes);
    auto reader = RecordReader::Open(allocator, outerBytes.Value().View(), Schema, 1, 1);
    REQUIRE(reader);
    auto field = reader.Value().Next();
    REQUIRE(field);
    CHECK(reader.Value().OpenRecordField(allocator, field.Value(), Inner, 1, 1, 16).Error() ==
          ErrorCode::InvalidArgument);

    // 이동된 reader는 InvalidArgument다.
    RecordReader moved = Move(reader.Value());
    CHECK(reader.Value().OpenRecordField(allocator, field.Value(), Inner, 1, 1, 16).Error() ==
          ErrorCode::InvalidArgument);
    (void)moved;
}

// OpenRecordField는 바깥 reader의 필드 범위·wire를 검사하고, 같은 예산으로 열되 payload 바이트를
// 다시 청구하지 않는다(깊은 중첩도 바깥 한 번의 바이트 청구 + 단계마다 색인 비용).
TEST(RecordOpenRecordFieldSharesBudgetWithoutRechargingPayload) {
    SystemAllocator allocator;
    constexpr usize Depth = 16;
    constexpr usize Bytes = 64 * 1024;
    auto bytes = NestedRecord(allocator, Depth, Bytes);
    REQUIRE(bytes);
    auto outer = RecordReader::Open(allocator, bytes.Value().View(), Inner, 1, 1);
    REQUIRE(outer);
    const uint64 afterOpen = outer.Value().WorkUsed();
    CHECK(afterOpen >= Bytes);

    RecordReader* current = &outer.Value();
    Array<RecordReader> chain(allocator);
    REQUIRE(chain.TryReserve(Depth));
    for (usize level = 1; level < Depth; ++level) {
        auto field = current->Next();
        REQUIRE(field);
        auto inner = current->OpenRecordField(allocator, field.Value(), Inner, 1, 1, 16);
        REQUIRE(inner);
        REQUIRE(chain.TryPushBack(Move(inner.Value())));
        current = &chain[chain.Size() - 1];
    }

    // 15단을 더 열어도 payload(64 KiB)를 다시 청구하지 않는다.
    CHECK(current->WorkUsed() - afterOpen < 4096);
    auto leaf = current->Next();
    REQUIRE(leaf);
    CHECK(leaf.Value().id == Leaf);
    auto value = current->OpenValue(leaf.Value());
    REQUIRE(value);
    auto read = value.Value().ReadBytes();
    REQUIRE(read);
    CHECK(read.Value().Size() == Bytes);

    // 오류: wire가 Record가 아니면 TypeMismatch, 다른 schema는 TypeMismatch, 범위 밖 버전은
    // Unsupported, 다른 Record의 필드는 InvalidArgument, 필드 수 한도 초과는 CapacityExceeded.
    CHECK(current->OpenRecordField(allocator, leaf.Value(), Inner, 1, 1, 16).Error() ==
          ErrorCode::TypeMismatch);
    auto first = RecordReader::Open(allocator, bytes.Value().View(), Inner, 1, 1);
    REQUIRE(first);
    auto nested = first.Value().Next();
    REQUIRE(nested);
    CHECK(first.Value().OpenRecordField(allocator, nested.Value(), Schema, 1, 1, 16).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(first.Value().OpenRecordField(allocator, nested.Value(), Inner, 2, 3, 16).Error() ==
          ErrorCode::Unsupported);
    CHECK(first.Value().OpenRecordField(allocator, nested.Value(), Inner, 1, 1, 0).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(current->OpenRecordField(allocator, nested.Value(), Inner, 1, 1, 16).Error() ==
          ErrorCode::InvalidArgument);
}
