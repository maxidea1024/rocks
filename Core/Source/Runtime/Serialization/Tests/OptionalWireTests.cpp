#include <Core/Memory/SystemAllocator.h>
#include <Serialization/OptionalWire.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace optional_wire_tests_file {

// Optional 값(WireType::Optional = 14)의 형식·present byte 범위·내부 tag 검사·Record 내부·예산
// 시험이다 (Docs/Design/ReflectionExtensions.ko.md 1.2절 — 리플렉션 B9a3).
namespace {

constexpr Guid ParentSchema{14, 1};
constexpr Guid OptionalField{14, 2};
constexpr Guid OtherField{14, 3};
constexpr Guid InnerStruct{14, 4};
constexpr Guid InnerMember{14, 5};

constexpr uint32 Wire(WireType type) noexcept {
    return static_cast<uint32>(type);
}

void AppendU32(Array<uint8>& out, uint32 value) noexcept {
    for (int index = 0; index < 4; ++index) {
        static_cast<void>(out.TryPushBack(static_cast<uint8>(value >> (8 * index))));
    }
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

ErrorCode OpenError(Span<const uint8> payload, uint32 innerWire,
                    WireValueShape shape = {}) noexcept {
    auto archive = ArchiveReader::Create(payload);
    if (!archive) {
        return archive.Error();
    }

    auto opened = OptionalWireReader::Open(Move(archive.Value()), innerWire, shape);
    return opened ? ErrorCode::None : opened.Error();
}

// present 1 + 내부 tag 뒤에 extra byte를 붙인 payload다.
Array<uint8> SetPayload(Allocator& allocator, uint32 tag, usize extra = 0) noexcept {
    Array<uint8> bytes(allocator);
    static_cast<void>(bytes.TryPushBack(1));
    AppendU32(bytes, tag);
    for (usize index = 0; index < extra; ++index) {
        static_cast<void>(bytes.TryPushBack(0));
    }

    return bytes;
}

// bytes 길이 Bytes 멤버 하나의 struct Record다.
Result<Array<uint8>> BuildInner(Allocator& allocator, usize bytes) noexcept {
    Array<uint8> filler(allocator);
    if (auto appended = filler.TryAppendUninitialized(bytes); !appended) {
        return Result<Array<uint8>>::Failure(appended.Error());
    }

    platform::MemorySet(filler.Data(), 0x42, filler.Size());
    ArchiveWriter member(allocator);
    if (auto written = member.WriteBytes(filler.View()); !written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    RecordBuilder builder(allocator, InnerStruct, 2);
    if (auto added = builder.AddField(InnerMember, WireType::Bytes, member.View()); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return builder.Build();
}

} // namespace

TEST(OptionalWire_UnsetScalarAndRecordMatchFormat) {
    SystemAllocator allocator;
    // unset은 present 0 하나다.
    ArchiveWriter unset(allocator);
    REQUIRE(OptionalWireWriter::WriteUnset(unset));
    const uint8 unsetBytes[] = {0};
    CHECK(SameBytes(unset.View(), unsetBytes));
    auto unsetReader = ArchiveReader::Create(unset.View());
    REQUIRE(unsetReader);
    auto readUnset = OptionalWireReader::Open(Move(unsetReader.Value()), Wire(WireType::U32));
    REQUIRE(readUnset);
    CHECK(!readUnset.Value().IsSet() && readUnset.Value().RecordBytes().Size() == 0);
    CHECK(readUnset.Value().Finish());

    // set U32: present 1, tag 2, 값 4 byte(little-endian).
    ArchiveWriter number(allocator);
    REQUIRE(number.WriteU32(0x5a5a5a5au)); // 바깥 writer 뒤에 제자리로 쓴다
    auto set = OptionalWireWriter::BeginSet(number, Wire(WireType::U32));
    REQUIRE(set);
    REQUIRE(number.WriteU32(0x01020304u));
    REQUIRE(set.Value().End());
    const uint8 numberBytes[] = {1, 2, 0, 0, 0, 4, 3, 2, 1};
    const Span<const uint8> payload(number.View().Data() + 4, number.ByteCount() - 4);
    CHECK(SameBytes(payload, numberBytes));
    auto numberReader = ArchiveReader::Create(numberBytes);
    REQUIRE(numberReader);
    auto readNumber = OptionalWireReader::Open(Move(numberReader.Value()), Wire(WireType::U32));
    REQUIRE(readNumber && readNumber.Value().IsSet());
    CHECK(readNumber.Value().Finish().Error() == ErrorCode::CorruptData); // 아직 읽지 않았다
    auto value = readNumber.Value().Inner().ReadU32();
    CHECK(value && value.Value() == 0x01020304u);
    CHECK(readNumber.Value().Finish());

    // set String(빈 문자열 포함)과 Digest = Bytes {40, exact}.
    ArchiveWriter text(allocator);
    auto stringSet = OptionalWireWriter::BeginSet(text, Wire(WireType::String));
    REQUIRE(stringSet && text.WriteString("") && stringSet.Value().End());
    auto textReader = ArchiveReader::Create(text.View());
    REQUIRE(textReader);
    auto readText = OptionalWireReader::Open(Move(textReader.Value()), Wire(WireType::String));
    REQUIRE(readText && readText.Value().IsSet());
    auto string = readText.Value().Inner().ReadStringView();
    CHECK(string && string.Value().ByteCount() == 0 && readText.Value().Finish());
    const WireValueShape digest{40, true};
    uint8 digestBytes[32] = {};
    ArchiveWriter digestOut(allocator);
    auto digestSet = OptionalWireWriter::BeginSet(digestOut, Wire(WireType::Bytes), digest);
    REQUIRE(digestSet && digestOut.WriteBytes(digestBytes) && digestSet.Value().End());
    CHECK(OpenError(digestOut.View(), Wire(WireType::Bytes), digest) == ErrorCode::None);

    // set Record: present 1, tag 10, 남은 byte 전부가 Record 하나(길이 prefix 없음).
    auto inner = BuildInner(allocator, 16);
    REQUIRE(inner);
    ArchiveWriter record(allocator);
    auto recordSet = OptionalWireWriter::BeginSet(record, Wire(WireType::Record));
    REQUIRE(recordSet && record.WriteRaw(inner.Value().View()) && recordSet.Value().End());
    Array<uint8> expected(allocator);
    REQUIRE(expected.TryPushBack(1));
    AppendU32(expected, Wire(WireType::Record));
    AppendBytes(expected, inner.Value().View());
    CHECK(SameBytes(record.View(), expected.View()));
    auto recordReader = ArchiveReader::Create(record.View());
    REQUIRE(recordReader);
    auto readRecord = OptionalWireReader::Open(Move(recordReader.Value()), Wire(WireType::Record));
    REQUIRE(readRecord && readRecord.Value().IsSet());
    CHECK(SameBytes(readRecord.Value().RecordBytes(), inner.Value().View()));
    CHECK(readRecord.Value().Finish()); // Record 내부는 Open이 내용으로 넘겼다

    // 이동된 reader는 unset의 빈 reader다(이동 생성·대입).
    OptionalWireReader moved(Move(readRecord.Value()));
    CHECK(!readRecord.Value().IsSet() && readRecord.Value().RecordBytes().Size() == 0);
    CHECK(moved.IsSet() && moved.RecordBytes().Size() == inner.Value().Size());
    readNumber.Value() = Move(moved);
    CHECK(!moved.IsSet() && readNumber.Value().IsSet() &&
          readNumber.Value().RecordBytes().Size() == inner.Value().Size());
}

TEST(OptionalWire_PresentByteAndInnerTagChecksRunInOrder) {
    SystemAllocator allocator;
    // 기대 내부 tag 자체가 Optional 내부 tag가 아니면(참조·InstancedStruct·Text·컨테이너) 먼저
    // InvalidArgument다.
    const uint8 unset[] = {0};
    const uint32 notInner[] = {Wire(WireType::SoftObjectReference),
                               Wire(WireType::InstancedStruct),
                               Wire(WireType::Text),
                               Wire(WireType::RecordList),
                               Wire(WireType::ValueArray),
                               Wire(WireType::Optional),
                               Wire(WireType::Set),
                               Wire(WireType::Map),
                               0u,
                               99u};
    for (uint32 tag : notInner) {
        CHECK(OpenError(unset, tag) == ErrorCode::InvalidArgument);
    }

    // shape 모순(고정 폭에 다른 폭, Record·String 밖의 exact 가변 모순)도 먼저 거부한다.
    CHECK(OpenError(unset, Wire(WireType::U32), {8, false}) == ErrorCode::InvalidArgument);
    CHECK(OpenError(unset, Wire(WireType::Record), {44, true}) == ErrorCode::InvalidArgument);
    CHECK(OpenError(unset, Wire(WireType::U32)) == ErrorCode::None);

    // present byte: 없으면·0·1 밖이면 InvalidArgument, unset 뒤에 byte가 남아도 InvalidArgument다.
    CHECK(OpenError(Span<const uint8>(), Wire(WireType::U32)) == ErrorCode::CorruptData);
    const uint8 presentValues[] = {2, 3, 0x80, 0xff};
    for (uint8 present : presentValues) {
        const uint8 bytes[] = {present, 2, 0, 0, 0, 1, 0, 0, 0};
        CHECK(OpenError(bytes, Wire(WireType::U32)) == ErrorCode::CorruptData);
    }

    const uint8 unsetTrailing[] = {0, 0};
    CHECK(OpenError(unsetTrailing, Wire(WireType::U32)) == ErrorCode::CorruptData);

    // 내부 tag 잘림(present 1 뒤 0–3 byte).
    for (usize length = 1; length < 5; ++length) {
        auto bytes = SetPayload(allocator, Wire(WireType::U32), 4);
        CHECK(OpenError(Span<const uint8>(bytes.Data(), length), Wire(WireType::U32)) ==
              ErrorCode::CorruptData);
    }

    // 저장 내부 tag가 내부 자리의 것이 아니면(기대 tag와 달라도) InvalidArgument, 내부 tag끼리
    // 다르면 TypeMismatch다(크기 검사보다 먼저).
    for (uint32 tag : notInner) {
        auto bytes = SetPayload(allocator, tag, 8);
        CHECK(OpenError(bytes.View(), Wire(WireType::U64)) == ErrorCode::CorruptData);
    }

    auto other = SetPayload(allocator, Wire(WireType::I64), 8);
    CHECK(OpenError(other.View(), Wire(WireType::U64)) == ErrorCode::TypeMismatch);
    CHECK(OpenError(other.View(), Wire(WireType::I64)) == ErrorCode::None);

    // 내부 크기: 고정 폭은 정확히, 가변은 최소 크기 이상, exact 가변은 길이 prefix까지.
    auto shortFixed = SetPayload(allocator, Wire(WireType::U64), 7);
    CHECK(OpenError(shortFixed.View(), Wire(WireType::U64)) == ErrorCode::CorruptData);
    auto longFixed = SetPayload(allocator, Wire(WireType::U64), 9);
    CHECK(OpenError(longFixed.View(), Wire(WireType::U64)) == ErrorCode::CorruptData);
    auto noValue = SetPayload(allocator, Wire(WireType::Bool));
    CHECK(OpenError(noValue.View(), Wire(WireType::Bool)) == ErrorCode::CorruptData);
    auto shortString = SetPayload(allocator, Wire(WireType::String), 7);
    CHECK(OpenError(shortString.View(), Wire(WireType::String)) == ErrorCode::CorruptData);
    const WireValueShape digest{40, true};
    auto skewedDigest = SetPayload(allocator, Wire(WireType::Bytes));
    AppendU64(skewedDigest, 31); // 합계는 40이지만 길이 prefix가 32가 아니다
    for (usize index = 0; index < 32; ++index) {
        REQUIRE(skewedDigest.TryPushBack(0));
    }

    CHECK(OpenError(skewedDigest.View(), Wire(WireType::Bytes), digest) == ErrorCode::CorruptData);
    CHECK(OpenError(skewedDigest.View(), Wire(WireType::Bytes)) == ErrorCode::None);

    // 내용 검사는 내부 Read*가 한다: bool 2는 Open이 아니라 ReadBool이 거부한다.
    auto badBool = SetPayload(allocator, Wire(WireType::Bool));
    REQUIRE(badBool.TryPushBack(2));
    auto archive = ArchiveReader::Create(badBool.View());
    REQUIRE(archive);
    auto boolReader = OptionalWireReader::Open(Move(archive.Value()), Wire(WireType::Bool));
    REQUIRE(boolReader);
    CHECK(boolReader.Value().Inner().ReadBool().Error() == ErrorCode::CorruptData);
}

TEST(OptionalWire_RecordInnerMustBeExactlyOneRecord) {
    SystemAllocator allocator;
    auto inner = BuildInner(allocator, 8);
    REQUIRE(inner);
    struct Mutation {
        usize offset; // Record 안 offset
        uint8 value;
        ErrorCode expected;
    };

    const Mutation mutations[] = {{0, 'Q', ErrorCode::CorruptData},
                                  {8, 3, ErrorCode::Unsupported},
                                  {28, 0, ErrorCode::CorruptData},
                                  {36, 1, ErrorCode::CorruptData}};
    for (const Mutation& mutation : mutations) {
        auto bytes = SetPayload(allocator, Wire(WireType::Record));
        AppendBytes(bytes, inner.Value().View());
        bytes[5 + mutation.offset] = mutation.value;
        CHECK(OpenError(bytes.View(), Wire(WireType::Record)) == mutation.expected);
    }

    // Record 뒤 trailing byte, 잘린 Record, Record 없음.
    auto trailing = SetPayload(allocator, Wire(WireType::Record));
    AppendBytes(trailing, inner.Value().View());
    REQUIRE(trailing.TryPushBack(0));
    CHECK(OpenError(trailing.View(), Wire(WireType::Record)) == ErrorCode::CorruptData);
    auto truncated = SetPayload(allocator, Wire(WireType::Record));
    AppendBytes(truncated, Span<const uint8>(inner.Value().Data(), inner.Value().Size() - 1));
    CHECK(OpenError(truncated.View(), Wire(WireType::Record)) == ErrorCode::CorruptData);
    auto missing = SetPayload(allocator, Wire(WireType::Record));
    CHECK(OpenError(missing.View(), Wire(WireType::Record)) == ErrorCode::CorruptData);
}

TEST(OptionalWire_WriterChecksInnerPayload) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    // 내부 tag가 아니면 쓰기 전에 거부한다.
    CHECK(OptionalWireWriter::BeginSet(out, Wire(WireType::Text)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(OptionalWireWriter::BeginSet(out, Wire(WireType::SoftObjectReference)).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(OptionalWireWriter::BeginSet(out, Wire(WireType::U32), {8, false}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.ByteCount() == 0);

    // 내부 값이 없거나 폭이 다르면 InvalidArgument다.
    ArchiveWriter missing(allocator);
    auto noValue = OptionalWireWriter::BeginSet(missing, Wire(WireType::U32));
    REQUIRE(noValue);
    CHECK(noValue.Value().End().Error() == ErrorCode::InvalidArgument);
    REQUIRE(missing.WriteU64(1));
    CHECK(noValue.Value().End().Error() == ErrorCode::InvalidArgument);

    // Record 내부가 Record 하나가 아니면 InvalidArgument다.
    ArchiveWriter record(allocator);
    auto recordSet = OptionalWireWriter::BeginSet(record, Wire(WireType::Record));
    REQUIRE(recordSet);
    CHECK(recordSet.Value().End().Error() == ErrorCode::InvalidArgument);
    REQUIRE(record.WriteU64(0));
    CHECK(recordSet.Value().End().Error() == ErrorCode::InvalidArgument);

    // exact 가변(Digest)의 길이 prefix도 본다.
    ArchiveWriter digest(allocator);
    auto digestSet = OptionalWireWriter::BeginSet(digest, Wire(WireType::Bytes), {40, true});
    REQUIRE(digestSet);
    uint8 data[33] = {};
    REQUIRE(digest.WriteBytes(Span<const uint8>(data, 31)));
    CHECK(digestSet.Value().End().Error() == ErrorCode::InvalidArgument);
}

TEST(OptionalWire_OpenFieldChecksWireAndChargesRecordOnce) {
    SystemAllocator allocator;
    constexpr usize Bytes = 64 * 1024;
    auto inner = BuildInner(allocator, Bytes);
    REQUIRE(inner);
    ArchiveWriter optional(allocator);
    auto set = OptionalWireWriter::BeginSet(optional, Wire(WireType::Record));
    REQUIRE(set && optional.WriteRaw(inner.Value().View()) && set.Value().End());
    ArchiveWriter scalar(allocator);
    REQUIRE(scalar.WriteU32(5));
    RecordBuilder builder(allocator, ParentSchema, 1);
    REQUIRE(builder.AddField(OptionalField, WireType::Optional, optional.View()));
    REQUIRE(builder.AddField(OtherField, WireType::U32, scalar.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto parent = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(OptionalField);
    auto other = parent.Value().Find(OtherField);
    REQUIRE(field && other);

    // Record 내부는 트리의 Open이 이미 청구했으므로 Optional reader·중첩 Open이 다시 청구하지
    // 않는다.
    const uint64 before = parent.Value().WorkUsed();
    auto reader =
        OptionalWireReader::OpenField(parent.Value(), field.Value(), Wire(WireType::Record));
    REQUIRE(reader && reader.Value().IsSet());
    auto nested = parent.Value().OpenNestedRecord(allocator, reader.Value().RecordBytes(),
                                                  InnerStruct, 1, 2, 4);
    REQUIRE(nested);
    CHECK(parent.Value().WorkUsed() - before < 4096);
    auto member = nested.Value().Find(InnerMember);
    REQUIRE(member);
    auto value = nested.Value().OpenValue(member.Value());
    REQUIRE(value);
    auto read = value.Value().ReadBytes();
    CHECK(read && read.Value().Size() == Bytes);
    CHECK(reader.Value().Finish());

    // 단독 reader(ArchiveReader::Create)는 Record 내용 byte를 청구한다.
    auto alone = ArchiveReader::Create(optional.View());
    REQUIRE(alone);
    auto aloneReader = OptionalWireReader::Open(Move(alone.Value()), Wire(WireType::Record));
    REQUIRE(aloneReader);
    CHECK(aloneReader.Value().Inner().WorkUsed() >= inner.Value().Size());

    // OpenField 오류: Optional이 아닌 필드는 TypeMismatch, 다른 Record의 범위·이동된 parent는
    // InvalidArgument이고, 실패한 호출도 operation 하나를 소비한다.
    uint64 operations = parent.Value().OperationsUsed();
    CHECK(
        OptionalWireReader::OpenField(parent.Value(), other.Value(), Wire(WireType::U32)).Error() ==
        ErrorCode::TypeMismatch);
    CHECK(parent.Value().OperationsUsed() == operations + 1);
    FieldView foreign = field.Value();
    foreign.payload = optional.View();
    CHECK(OptionalWireReader::OpenField(parent.Value(), foreign, Wire(WireType::Record)).Error() ==
          ErrorCode::InvalidArgument);
    RecordReader moved(Move(parent.Value()));
    CHECK(OptionalWireReader::OpenField(parent.Value(), field.Value(), Wire(WireType::Record))
              .Error() == ErrorCode::InvalidArgument);
    CHECK(OptionalWireReader::OpenField(moved, field.Value(), Wire(WireType::Record)));
}

TEST(OptionalWire_ArgumentErrorsKeepPayloadAndWriterRejectsShrunkOutput) {
    SystemAllocator allocator;
    // 기대 내부 tag·shape 오류는 payload를 옮기기 전에 거부하므로 호출자 reader가 그대로다.
    const uint8 number[] = {1, 2, 0, 0, 0, 7, 0, 0, 0};
    auto payload = ArchiveReader::Create(number);
    REQUIRE(payload);
    CHECK(OptionalWireReader::Open(Move(payload.Value()), WireType::Text).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(OptionalWireReader::Open(Move(payload.Value()), WireType::U32, {8, false}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(payload.Value().Remaining() == sizeof(number));
    auto reader = OptionalWireReader::Open(Move(payload.Value()), WireType::U32);
    REQUIRE(reader && reader.Value().IsSet());
    CHECK(reader.Value().Inner().ReadU32().Value() == 7);

    // Record 내부가 reader의 maxValueBytes보다 크면 CapacityExceeded다(단독 reader).
    auto inner = BuildInner(allocator, 32);
    REQUIRE(inner);
    auto bytes = SetPayload(allocator, Wire(WireType::Record));
    AppendBytes(bytes, inner.Value().View());
    ArchiveLimits small;
    small.maxValueBytes = inner.Value().Size() - 1;
    auto limited = ArchiveReader::Create(bytes.View(), small);
    REQUIRE(limited);
    CHECK(OptionalWireReader::Open(Move(limited.Value()), WireType::Record).Error() ==
          ErrorCode::CapacityExceeded);
    small.maxValueBytes = inner.Value().Size();
    auto enough = ArchiveReader::Create(bytes.View(), small);
    REQUIRE(enough);
    CHECK(OptionalWireReader::Open(Move(enough.Value()), WireType::Record));

    // 호출자가 End 전에 out을 머리보다 짧게 줄이면 InvalidArgument다.
    ArchiveWriter out(allocator);
    auto set = OptionalWireWriter::BeginSet(out, WireType::U64);
    REQUIRE(set && out.WriteU64(1));
    out.Clear();
    CHECK(set.Value().End().Error() == ErrorCode::InvalidArgument);
}
} // namespace optional_wire_tests_file
