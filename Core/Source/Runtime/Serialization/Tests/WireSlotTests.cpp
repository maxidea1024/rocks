#include <Core/Memory/SystemAllocator.h>
#include <Serialization/WireSlot.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace wire_slot_tests_file {

// 컨테이너 안 자리의 wire 규칙(WireSlot)과 원소 자리 길이 prefix Record 시험이다
// (Docs/Design/ReflectionExtensions.ko.md 1.2절 — 리플렉션 B9a3).
namespace {

constexpr Guid ParentSchema{17, 1};
constexpr Guid ValueField{17, 2};
constexpr Guid ContentStruct{17, 3};
constexpr Guid ContentMember{17, 4};

constexpr uint32 Wire(WireType type) noexcept {
    return static_cast<uint32>(type);
}

void AppendU64(Array<uint8>& out, uint64 value) noexcept {
    for (int index = 0; index < 8; ++index) {
        static_cast<void>(out.TryPushBack(static_cast<uint8>(value >> (8 * index))));
    }
}

void AppendBytes(Array<uint8>& out, Span<const uint8> bytes) noexcept {
    static_cast<void>(out.TryAppend(bytes));
}

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

// U32 멤버 하나(전체 76 byte)의 Record다.
Result<Array<uint8>> BuildContent(Allocator& allocator, uint32 value, uint32 version = 2) noexcept {
    ArchiveWriter payload(allocator);
    if (auto written = payload.WriteU32(value); !written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    RecordBuilder builder(allocator, ContentStruct, version);
    if (auto added = builder.AddField(ContentMember, WireType::U32, payload.View()); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return builder.Build();
}

// 자리 표의 기대값이다: 0이면 받지 않는다.
struct SlotRow {
    uint32 wire;
    usize array;    // ArrayElement 최소 크기
    usize key;      // Key
    usize mapValue; // MapValue
    usize optional; // OptionalInner
    bool exact;
};

} // namespace

TEST(WireSlot_ShapesFollowSlotTable) {
    // 1.2절: ValueArray·Map 값은 단일 값 tag + InstancedStruct·Text(Map 값은 Record도), 키는 실수·
    // InstancedStruct·Text를 빼고, Optional 내부는 참조·InstancedStruct·Text를 빼고 Record를
    // 받는다.
    const SlotRow rows[] = {{Wire(WireType::Bool), 1, 1, 1, 1, true},
                            {Wire(WireType::U32), 4, 4, 4, 4, true},
                            {Wire(WireType::U64), 8, 8, 8, 8, true},
                            {Wire(WireType::I64), 8, 8, 8, 8, true},
                            {Wire(WireType::F32), 4, 0, 4, 4, true},
                            {Wire(WireType::F64), 8, 0, 8, 8, true},
                            {Wire(WireType::Guid), 16, 16, 16, 16, true},
                            {Wire(WireType::String), 8, 8, 8, 8, false},
                            {Wire(WireType::Bytes), 8, 8, 8, 8, false},
                            {Wire(WireType::Record), 0, 0, 52, 44, false},
                            {Wire(WireType::RecordList), 0, 0, 0, 0, false},
                            {Wire(WireType::SoftObjectReference), 24, 24, 24, 0, false},
                            {Wire(WireType::ValueArray), 0, 0, 0, 0, false},
                            {Wire(WireType::Optional), 0, 0, 0, 0, false},
                            {Wire(WireType::Set), 0, 0, 0, 0, false},
                            {Wire(WireType::Map), 0, 0, 0, 0, false},
                            {Wire(WireType::InstancedStruct), 8, 0, 8, 0, false},
                            {Wire(WireType::Text), 1, 0, 1, 0, false},
                            {0u, 0, 0, 0, 0, false},
                            {19u, 0, 0, 0, 0, false},
                            {0xF00Du, 0, 0, 0, 0, false}};
    for (const SlotRow& row : rows) {
        const WireSlot slots[] = {WireSlot::ArrayElement, WireSlot::Key, WireSlot::MapValue,
                                  WireSlot::OptionalInner};
        const usize sizes[] = {row.array, row.key, row.mapValue, row.optional};
        for (usize index = 0; index < 4; ++index) {
            auto shape = WireSlotShape(slots[index], row.wire);
            if (sizes[index] == 0) {
                CHECK(shape.Error() == ErrorCode::InvalidArgument);
                continue;
            }

            REQUIRE(shape);
            CHECK(shape.Value().minElementBytes == sizes[index]);
            CHECK(shape.Value().exact == row.exact);
        }
    }

    // 열거자 밖의 자리는 어떤 tag도 받지 않는다.
    CHECK(WireSlotShape(static_cast<WireSlot>(4), Wire(WireType::Bool)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(WireSlotShape(static_cast<WireSlot>(200), Wire(WireType::U32)).Error() ==
          ErrorCode::InvalidArgument);
}

TEST(WireSlot_PrefixedRecordRoundTripAndWriterMisuse) {
    SystemAllocator allocator;
    auto content = BuildContent(allocator, 0x11223344u);
    REQUIRE(content);
    ArchiveWriter out(allocator);
    REQUIRE(out.WriteU32(0x5a5a5a5au)); // 바깥 writer 뒤에 제자리로 쓴다
    auto record = PrefixedRecordWriter::Begin(out, Wire(WireType::Record));
    REQUIRE(record);
    REQUIRE(out.WriteRaw(content.Value().View()));
    REQUIRE(record.Value().End());
    CHECK(record.Value().End().Error() == ErrorCode::InvalidArgument); // 두 번 끝낼 수 없다

    // 손으로 조립한 byte: u64 길이 76 + Record.
    Array<uint8> expected(allocator);
    AppendU64(expected, 76);
    AppendBytes(expected, content.Value().View());
    CHECK(
        SameBytes(Span<const uint8>(out.View().Data() + 4, out.ByteCount() - 4), expected.View()));

    auto reader =
        ArchiveReader::Create(Span<const uint8>(out.View().Data() + 4, out.ByteCount() - 4));
    REQUIRE(reader);
    auto read = ReadPrefixedRecord(reader.Value());
    REQUIRE(read);
    CHECK(SameBytes(read.Value(), content.Value().View()));
    CHECK(reader.Value().AtEnd());

    // writer 오류: Record·InstancedStruct 밖의 wire는 쓰기 전에 InvalidArgument, Record wire에 빈
    // 내용·Record가 아닌 내용은 End가 InvalidArgument다.
    ArchiveWriter bad(allocator);
    CHECK(PrefixedRecordWriter::Begin(bad, Wire(WireType::Bytes)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(PrefixedRecordWriter::Begin(bad, Wire(WireType::RecordList)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(bad.ByteCount() == 0);
    auto empty = PrefixedRecordWriter::Begin(bad, Wire(WireType::Record));
    REQUIRE(empty);
    CHECK(empty.Value().End().Error() == ErrorCode::InvalidArgument);
    ArchiveWriter garbage(allocator);
    auto notRecord = PrefixedRecordWriter::Begin(garbage, Wire(WireType::Record));
    REQUIRE(notRecord && garbage.WriteU64(7));
    CHECK(notRecord.Value().End().Error() == ErrorCode::InvalidArgument);

    // InstancedStruct wire는 0 byte(비어 있음)도 받는다.
    ArchiveWriter instanced(allocator);
    auto none = PrefixedRecordWriter::Begin(instanced, WireType::InstancedStruct);
    REQUIRE(none && none.Value().End());
    const uint8 zero[] = {0, 0, 0, 0, 0, 0, 0, 0};
    CHECK(SameBytes(instanced.View(), zero));

    // 호출자가 End 전에 out을 길이 자리보다 짧게 줄이면 버퍼를 읽지 않고 InvalidArgument다.
    ArchiveWriter cleared(allocator);
    REQUIRE(cleared.WriteU32(1));
    auto shrunk = PrefixedRecordWriter::Begin(cleared, WireType::Record);
    REQUIRE(shrunk);
    cleared.Clear();
    CHECK(shrunk.Value().End().Error() == ErrorCode::InvalidArgument);
    CHECK(cleared.ByteCount() == 0);
}

TEST(WireSlot_ReadPrefixedRecordRejectsMalformedContentWithoutMovingCursor) {
    SystemAllocator allocator;
    auto content = BuildContent(allocator, 5);
    REQUIRE(content);
    struct Case {
        uint64 length;
        usize appended; // 길이 prefix 뒤에 붙일 Record byte 수(앞부분)
        usize patchOffset;
        uint8 patchValue;
        ErrorCode expected;
    };

    constexpr usize NoPatch = IndexNone;
    const Case cases[] = {{76, 75, NoPatch, 0, ErrorCode::CorruptData}, // 길이가 남은 byte보다 크다
                          {0, 0, NoPatch, 0, ErrorCode::CorruptData},   // 0 byte는 Record가 아니다
                          {40, 40, NoPatch, 0, ErrorCode::CorruptData}, // header보다 짧다
                          {76, 76, 0, 'X', ErrorCode::CorruptData},     // magic
                          {76, 76, 8, 2, ErrorCode::Unsupported},       // envelope version
                          {76, 76, 28, 0, ErrorCode::CorruptData},      // schema version 0
                          {75, 76, NoPatch, 0, ErrorCode::CorruptData}}; // 길이가 Record보다 1 짧다
    for (const Case& item : cases) {
        Array<uint8> bytes(allocator);
        AppendU64(bytes, item.length);
        AppendBytes(bytes, Span<const uint8>(content.Value().Data(), item.appended));
        if (item.patchOffset != NoPatch) {
            bytes[8 + item.patchOffset] = item.patchValue;
        }

        auto reader = ArchiveReader::Create(bytes.View());
        REQUIRE(reader);
        auto read = ReadPrefixedRecord(reader.Value());
        CHECK(read.Error() == item.expected);
        CHECK(reader.Value().Position() == 0);
    }

    // 길이 prefix 잘림.
    const uint8 truncated[] = {76, 0, 0, 0, 0, 0, 0};
    auto truncatedReader = ArchiveReader::Create(truncated);
    REQUIRE(truncatedReader);
    CHECK(ReadPrefixedRecord(truncatedReader.Value()).Error() == ErrorCode::CorruptData);
    CHECK(truncatedReader.Value().Position() == 0);

    // 길이가 maxValueBytes를 넘으면 CapacityExceeded다.
    Array<uint8> large(allocator);
    AppendU64(large, 76);
    AppendBytes(large, content.Value().View());
    ArchiveLimits small;
    small.maxValueBytes = 75;
    auto limited = ArchiveReader::Create(large.View(), small);
    REQUIRE(limited);
    CHECK(ReadPrefixedRecord(limited.Value()).Error() == ErrorCode::CapacityExceeded);
    CHECK(limited.Value().Position() == 0);
}

TEST(WireSlot_PrefixedContentIsChargedOnceInRecordTree) {
    // Record 트리 안의 값 reader(공유 예산)는 내용 byte를 다시 청구하지 않고, 단독 reader는
    // 청구한다.
    SystemAllocator allocator;
    Array<uint8> filler(allocator);
    REQUIRE(filler.TryAppendUninitialized(64 * 1024));
    platform::MemorySet(filler.Data(), 0x33, filler.Size());
    ArchiveWriter member(allocator);
    REQUIRE(member.WriteBytes(filler.View()));
    RecordBuilder inner(allocator, ContentStruct, 1);
    REQUIRE(inner.AddField(ContentMember, WireType::Bytes, member.View()));
    auto innerBytes = inner.Build();
    REQUIRE(innerBytes);
    ArchiveWriter value(allocator);
    auto prefixed = PrefixedRecordWriter::Begin(value, Wire(WireType::Record));
    REQUIRE(prefixed && value.WriteRaw(innerBytes.Value().View()) && prefixed.Value().End());
    RecordBuilder parentBuilder(allocator, ParentSchema, 1);
    REQUIRE(parentBuilder.AddField(ValueField, 0x7777u, value.View()));
    auto parentBytes = parentBuilder.Build();
    REQUIRE(parentBytes);
    auto parent = RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(ValueField);
    REQUIRE(field);
    auto shared = parent.Value().OpenValue(field.Value());
    REQUIRE(shared);
    const uint64 before = parent.Value().WorkUsed();
    auto read = ReadPrefixedRecord(shared.Value());
    REQUIRE(read);
    CHECK(parent.Value().WorkUsed() - before < 64);
    auto opened = parent.Value().OpenNestedRecord(allocator, read.Value(), ContentStruct, 1, 1, 4);
    REQUIRE(opened);
    CHECK(parent.Value().WorkUsed() - before < 4096);

    auto alone = ArchiveReader::Create(value.View());
    REQUIRE(alone);
    auto charged = ReadPrefixedRecord(alone.Value());
    REQUIRE(charged);
    CHECK(alone.Value().WorkUsed() >= innerBytes.Value().Size());
}
} // namespace wire_slot_tests_file
