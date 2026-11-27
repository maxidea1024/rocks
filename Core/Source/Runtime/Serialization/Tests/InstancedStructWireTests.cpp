#include <Core/Memory/SystemAllocator.h>
#include <Serialization/InstancedStructWire.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace instanced_struct_wire_tests_file {

// InstancedStruct 값(WireType::InstancedStruct = 17)의 필드 자리·원소 자리 형식, 내용 Record header
// 검사, cursor 불변, 예산 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.2절 — 리플렉션 B9a3).
namespace {

constexpr Guid ParentSchema{19, 1};
constexpr Guid ValueField{19, 2};
constexpr Guid ContentStruct{19, 3};
constexpr Guid ContentMember{19, 4};

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

} // namespace

TEST(InstancedStructWire_FieldAndElementContentRoundTrip) {
    SystemAllocator allocator;
    auto content = BuildContent(allocator, 0x0a0b0c0du, 7);
    REQUIRE(content);
    // 필드 값 자리: 비어 있음은 0 byte, 값은 Record 하나다.
    auto emptyReader = ArchiveReader::Create(Span<const uint8>());
    REQUIRE(emptyReader);
    auto empty = ReadInstancedStructField(emptyReader.Value());
    REQUIRE(empty);
    CHECK(empty.Value().IsEmpty() && empty.Value().schema == Guid{} &&
          empty.Value().schemaVersion == 0);
    auto fieldReader = ArchiveReader::Create(content.Value().View());
    REQUIRE(fieldReader);
    auto field = ReadInstancedStructField(fieldReader.Value());
    REQUIRE(field);
    CHECK(!field.Value().IsEmpty() && field.Value().schema == ContentStruct &&
          field.Value().schemaVersion == 7);
    CHECK(SameBytes(field.Value().record, content.Value().View()));
    CHECK(fieldReader.Value().AtEnd());

    // 원소 자리: u64 길이 + 같은 내용(0 = 비어 있음).
    ArchiveWriter elements(allocator);
    auto first = PrefixedRecordWriter::Begin(elements, Wire(WireType::InstancedStruct));
    REQUIRE(first && elements.WriteRaw(content.Value().View()) && first.Value().End());
    auto second = PrefixedRecordWriter::Begin(elements, Wire(WireType::InstancedStruct));
    REQUIRE(second && second.Value().End());
    auto reader = ArchiveReader::Create(elements.View());
    REQUIRE(reader);
    auto value = ReadInstancedStructElement(reader.Value());
    REQUIRE(value);
    CHECK(value.Value().schema == ContentStruct && value.Value().schemaVersion == 7);
    CHECK(SameBytes(value.Value().record, content.Value().View()));
    auto none = ReadInstancedStructElement(reader.Value());
    REQUIRE(none);
    CHECK(none.Value().IsEmpty());
    CHECK(reader.Value().AtEnd());
}

TEST(InstancedStructWire_MalformedContentIsRejectedWithoutMovingCursor) {
    SystemAllocator allocator;
    auto content = BuildContent(allocator, 1);
    REQUIRE(content);
    struct Mutation {
        usize offset;
        uint8 value;
        ErrorCode expected;
    };

    const Mutation mutations[] = {{0, 'r', ErrorCode::CorruptData},
                                  {8, 9, ErrorCode::Unsupported},
                                  {12, 0, ErrorCode::None}, // schema high만 0(low가 남는다)
                                  {28, 0, ErrorCode::CorruptData},
                                  {36, 0, ErrorCode::CorruptData}};
    for (const Mutation& mutation : mutations) {
        Array<uint8> bytes(allocator);
        AppendBytes(bytes, content.Value().View());
        bytes[mutation.offset] = mutation.value;
        auto fieldReader = ArchiveReader::Create(bytes.View());
        REQUIRE(fieldReader);
        auto field = ReadInstancedStructField(fieldReader.Value());
        CHECK(field.Error() == mutation.expected);
        CHECK(fieldReader.Value().AtEnd() == (mutation.expected == ErrorCode::None));

        Array<uint8> element(allocator);
        AppendU64(element, bytes.Size());
        AppendBytes(element, bytes.View());
        auto elementReader = ArchiveReader::Create(element.View());
        REQUIRE(elementReader);
        CHECK(ReadInstancedStructElement(elementReader.Value()).Error() == mutation.expected);
        CHECK(elementReader.Value().AtEnd() == (mutation.expected == ErrorCode::None));
    }

    // schema 0(두 u64 모두 0)과 Record 뒤 trailing byte는 InvalidArgument다.
    Array<uint8> zeroSchema(allocator);
    AppendBytes(zeroSchema, content.Value().View());
    for (usize index = 12; index < 28; ++index) {
        zeroSchema[index] = 0;
    }

    auto zeroReader = ArchiveReader::Create(zeroSchema.View());
    REQUIRE(zeroReader);
    CHECK(ReadInstancedStructField(zeroReader.Value()).Error() == ErrorCode::CorruptData);
    CHECK(zeroReader.Value().Position() == 0);
    Array<uint8> trailing(allocator);
    AppendBytes(trailing, content.Value().View());
    REQUIRE(trailing.TryPushBack(0));
    auto trailingReader = ArchiveReader::Create(trailing.View());
    REQUIRE(trailingReader);
    CHECK(ReadInstancedStructField(trailingReader.Value()).Error() == ErrorCode::CorruptData);
    CHECK(trailingReader.Value().Position() == 0);

    // 원소 자리의 길이 잘림·남은 byte보다 큰 길이·maxValueBytes 초과.
    const uint8 shortPrefix[] = {1, 0, 0};
    auto shortReader = ArchiveReader::Create(shortPrefix);
    REQUIRE(shortReader);
    CHECK(ReadInstancedStructElement(shortReader.Value()).Error() == ErrorCode::CorruptData);
    Array<uint8> overlong(allocator);
    AppendU64(overlong, content.Value().Size() + 1);
    AppendBytes(overlong, content.Value().View());
    auto overlongReader = ArchiveReader::Create(overlong.View());
    REQUIRE(overlongReader);
    CHECK(ReadInstancedStructElement(overlongReader.Value()).Error() == ErrorCode::CorruptData);
    CHECK(overlongReader.Value().Position() == 0);
    ArchiveLimits small;
    small.maxValueBytes = content.Value().Size() - 1;
    auto fieldLimited = ArchiveReader::Create(content.Value().View(), small);
    REQUIRE(fieldLimited);
    CHECK(ReadInstancedStructField(fieldLimited.Value()).Error() == ErrorCode::CapacityExceeded);
    CHECK(fieldLimited.Value().Position() == 0);
}

TEST(InstancedStructWire_FieldContentIsChargedOnceInRecordTree) {
    // 필드 자리 내용 Record는 Record 트리의 공유 예산 reader에서 다시 청구하지
    // 않고(OpenNestedRecord는 색인만), 단독 reader는 청구한다.
    SystemAllocator allocator;
    Array<uint8> filler(allocator);
    REQUIRE(filler.TryAppendUninitialized(64 * 1024));
    platform::MemorySet(filler.Data(), 0x5c, filler.Size());
    ArchiveWriter member(allocator);
    REQUIRE(member.WriteBytes(filler.View()));
    RecordBuilder inner(allocator, ContentStruct, 3);
    REQUIRE(inner.AddField(ContentMember, WireType::Bytes, member.View()));
    auto innerBytes = inner.Build();
    REQUIRE(innerBytes);
    RecordBuilder parentBuilder(allocator, ParentSchema, 1);
    REQUIRE(
        parentBuilder.AddField(ValueField, WireType::InstancedStruct, innerBytes.Value().View()));
    auto parentBytes = parentBuilder.Build();
    REQUIRE(parentBytes);
    auto parent = RecordReader::Open(allocator, parentBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(ValueField);
    REQUIRE(field && field.Value().wireType == Wire(WireType::InstancedStruct));
    auto value = parent.Value().OpenValue(field.Value());
    REQUIRE(value);
    const uint64 before = parent.Value().WorkUsed();
    auto content = ReadInstancedStructField(value.Value());
    REQUIRE(content && content.Value().schema == ContentStruct &&
            content.Value().schemaVersion == 3);
    CHECK(parent.Value().WorkUsed() - before < 64);
    auto record = parent.Value().OpenNestedRecord(allocator, content.Value().record,
                                                  content.Value().schema, 1, 3, 4);
    REQUIRE(record);
    CHECK(parent.Value().WorkUsed() - before < 4096);
    CHECK(value.Value().AtEnd());

    auto alone = ArchiveReader::Create(innerBytes.Value().View());
    REQUIRE(alone);
    REQUIRE(ReadInstancedStructField(alone.Value()));
    CHECK(alone.Value().WorkUsed() >= innerBytes.Value().Size());
}
} // namespace instanced_struct_wire_tests_file
