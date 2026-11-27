#include <Core/Memory/SystemAllocator.h>
#include <Serialization/InstancedStructWire.h>
#include <Serialization/MapWire.h>
#include <Serialization/SetWire.h>
#include <Serialization/TextWire.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;
namespace keyed_wire_tests_file {

// 키 집합(WireType::Set = 15)·맵(WireType::Map = 16)의 형식·header 검사 순서·count 사전 검사·키
// 자리 tag 제한·예산·writer 한도 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.2절 — 리플렉션
// B9a3).
namespace {

constexpr Guid ParentSchema{15, 1};
constexpr Guid KeyedField{15, 2};
constexpr Guid OtherField{15, 3};
constexpr Guid ValueStruct{15, 4};
constexpr Guid ValueMember{15, 5};

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

bool SameBytes(Span<const uint8> left, Span<const uint8> right) noexcept {
    return left.Size() == right.Size() &&
           (left.Size() == 0 || platform::MemoryEquals(left.Data(), right.Data(), left.Size()));
}

// tag(들)와 count 뒤에 extra byte를 붙인 payload다.
Array<uint8> SetHeader(Allocator& allocator, uint32 tag, uint32 count, usize extra = 0) noexcept {
    Array<uint8> bytes(allocator);
    AppendU32(bytes, tag);
    AppendU32(bytes, count);
    for (usize index = 0; index < extra; ++index) {
        static_cast<void>(bytes.TryPushBack(0));
    }

    return bytes;
}

Array<uint8> MapHeader(Allocator& allocator, uint32 keyTag, uint32 valueTag, uint32 count,
                       usize extra = 0) noexcept {
    Array<uint8> bytes(allocator);
    AppendU32(bytes, keyTag);
    AppendU32(bytes, valueTag);
    AppendU32(bytes, count);
    for (usize index = 0; index < extra; ++index) {
        static_cast<void>(bytes.TryPushBack(0));
    }

    return bytes;
}

ErrorCode SetError(Span<const uint8> payload, uint32 elementWire, uint32 maxCount,
                   WireValueShape shape = {}, ArchiveLimits limits = {}) noexcept {
    auto archive = ArchiveReader::Create(payload, limits);
    if (!archive) {
        return archive.Error();
    }

    auto opened = SetWireReader::Open(Move(archive.Value()), elementWire, maxCount, shape);
    return opened ? ErrorCode::None : opened.Error();
}

ErrorCode MapError(Span<const uint8> payload, uint32 keyWire, uint32 valueWire, uint32 maxCount,
                   WireValueShape keyShape = {}, WireValueShape valueShape = {},
                   ArchiveLimits limits = {}) noexcept {
    auto archive = ArchiveReader::Create(payload, limits);
    if (!archive) {
        return archive.Error();
    }

    auto opened = MapWireReader::Open(Move(archive.Value()), keyWire, valueWire, maxCount, keyShape,
                                      valueShape);
    return opened ? ErrorCode::None : opened.Error();
}

// 키 자리가 받지 않는 tag(실수·InstancedStruct·Text·컨테이너·Record·모르는 값)다.
constexpr uint32 NotKeyTags[] = {Wire(WireType::F32),
                                 Wire(WireType::F64),
                                 Wire(WireType::InstancedStruct),
                                 Wire(WireType::Text),
                                 Wire(WireType::Record),
                                 Wire(WireType::RecordList),
                                 Wire(WireType::ValueArray),
                                 Wire(WireType::Optional),
                                 Wire(WireType::Set),
                                 Wire(WireType::Map),
                                 0u,
                                 99u};

// 키 원소 하나를 쓴다(index로 값을 바꾼다).
Result<void> WriteKey(ArchiveWriter& out, WireType wire, uint32 index) noexcept {
    switch (wire) {
    case WireType::Bool:
        return out.WriteBool(index != 0);
    case WireType::U32:
        return out.WriteU32(0xfffffff0u + index);
    case WireType::U64:
        return out.WriteU64(0xfffffffff0ull + index);
    case WireType::I64:
        return out.WriteI64(-3 + static_cast<int64>(index));
    case WireType::Guid:
        return out.WriteGuid({index + 1, index + 9});
    case WireType::String:
        return out.WriteString(index == 0 ? StringView("") : StringView("키"));
    case WireType::Bytes: {
        const uint8 data[] = {9, 8, 7};
        return out.WriteBytes(Span<const uint8>(data, index));
    }
    case WireType::SoftObjectReference: {
        if (auto asset = out.WriteGuid({4, index}); !asset) {
            return asset;
        }

        return out.WriteString(index == 1 ? StringView("/Game/A.B") : StringView(""));
    }
    default:
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
}

// WriteKey가 쓴 키를 읽어 같은지 본다.
bool ReadKey(ArchiveReader& in, WireType wire, uint32 index) noexcept {
    switch (wire) {
    case WireType::Bool: {
        auto value = in.ReadBool();
        return value && value.Value() == (index != 0);
    }
    case WireType::U32: {
        auto value = in.ReadU32();
        return value && value.Value() == 0xfffffff0u + index;
    }
    case WireType::U64: {
        auto value = in.ReadU64();
        return value && value.Value() == 0xfffffffff0ull + index;
    }
    case WireType::I64: {
        auto value = in.ReadI64();
        return value && value.Value() == -3 + static_cast<int64>(index);
    }
    case WireType::Guid: {
        auto value = in.ReadGuid();
        return value && value.Value() == Guid{index + 1, index + 9};
    }
    case WireType::String: {
        auto value = in.ReadStringView();
        return value && value.Value() == (index == 0 ? StringView("") : StringView("키"));
    }
    case WireType::Bytes: {
        auto value = in.ReadBytes();
        return value && value.Value().Size() == index;
    }
    case WireType::SoftObjectReference: {
        auto asset = in.ReadGuid();
        auto path = in.ReadStringView();
        return asset && path && asset.Value() == Guid{4, index} &&
               path.Value() == (index == 1 ? StringView("/Game/A.B") : StringView(""));
    }
    default:
        return false;
    }
}

constexpr WireType KeyWires[] = {
    WireType::Bool, WireType::U32,    WireType::U64,   WireType::I64,
    WireType::Guid, WireType::String, WireType::Bytes, WireType::SoftObjectReference};

// U32 멤버 하나의 값 struct Record를 out에 제자리로 쓴다(Map 값 자리 — u64 길이 + Record).
Result<void> WriteStructValue(ArchiveWriter& out, uint32 value) noexcept {
    auto prefixed = PrefixedRecordWriter::Begin(out, Wire(WireType::Record));
    if (!prefixed) {
        return Result<void>::Failure(prefixed.Error());
    }

    auto record = RecordWriter::Begin(out, ValueStruct, 1);
    if (!record) {
        return Result<void>::Failure(record.Error());
    }

    if (auto begun = record.Value().BeginField(ValueMember); !begun) {
        return begun;
    }

    if (auto written = out.WriteU32(value); !written) {
        return written;
    }

    if (auto ended = record.Value().EndField(WireType::U32); !ended) {
        return ended;
    }

    if (auto ended = record.Value().End(); !ended) {
        return ended;
    }

    return prefixed.Value().End();
}

} // namespace

TEST(SetWire_RoundTripEveryKeyWireAndByteLayout) {
    SystemAllocator allocator;
    for (WireType wire : KeyWires) {
        ArchiveWriter out(allocator);
        REQUIRE(out.WriteU32(0xa5a5a5a5u)); // 바깥 writer 뒤에 제자리로 쓴다
        auto set = SetWireWriter::Begin(out, Wire(wire), 3, 4096, 3);
        REQUIRE(set);
        for (uint32 index = 0; index < 3; ++index) {
            REQUIRE(WriteKey(out, wire, index % 2 == 0 ? index : 1));
            CHECK(set.Value().Check());
        }

        REQUIRE(set.Value().End());
        auto archive =
            ArchiveReader::Create(Span<const uint8>(out.View().Data() + 4, out.ByteCount() - 4));
        REQUIRE(archive);
        auto reader = SetWireReader::Open(Move(archive.Value()), Wire(wire), 3);
        REQUIRE(reader && reader.Value().Count() == 3);
        for (uint32 index = 0; index < 3; ++index) {
            CHECK(ReadKey(reader.Value().Elements(), wire, index % 2 == 0 ? index : 1));
        }

        CHECK(reader.Value().Finish());
    }

    // String {"", "ab"}: 원소 tag 8, count 2, 원소마다 u64 길이 + byte(삽입 순서).
    ArchiveWriter text(allocator);
    auto strings = SetWireWriter::Begin(text, Wire(WireType::String), 2, 64);
    REQUIRE(strings && text.WriteString("") && text.WriteString("ab") && strings.Value().End());
    const uint8 expected[] = {8, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0,   0,
                              0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'};
    CHECK(SameBytes(text.View(), expected));

    // 빈 집합도 header를 쓴다.
    ArchiveWriter empty(allocator);
    auto none = SetWireWriter::Begin(empty, Wire(WireType::Guid), 0, 8);
    REQUIRE(none && none.Value().End());
    const uint8 emptyBytes[] = {7, 0, 0, 0, 0, 0, 0, 0};
    CHECK(SameBytes(empty.View(), emptyBytes));
    CHECK(SetError(empty.View(), Wire(WireType::Guid), 0) == ErrorCode::None);

    // 이동된 reader는 원소 0개다.
    auto archive = ArchiveReader::Create(text.View());
    REQUIRE(archive);
    auto reader = SetWireReader::Open(Move(archive.Value()), Wire(WireType::String), 2);
    REQUIRE(reader);
    SetWireReader moved(Move(reader.Value()));
    CHECK(reader.Value().Count() == 0 && moved.Count() == 2);
}

TEST(SetWire_HeaderChecksRejectNonKeyTagsInOrder) {
    SystemAllocator allocator;
    auto valid = SetHeader(allocator, Wire(WireType::U32), 1, 4);
    CHECK(SetError(valid.View(), Wire(WireType::U32), 1) == ErrorCode::None);
    // 기대 원소 tag 자체가 키 tag가 아니면(실수 포함) InvalidArgument다.
    for (uint32 tag : NotKeyTags) {
        CHECK(SetError(valid.View(), tag, 1) == ErrorCode::InvalidArgument);
    }

    // 저장 tag가 키 tag가 아니면 InvalidArgument(기대 tag와 달라도 — 형식 오류가 먼저)다.
    for (uint32 tag : NotKeyTags) {
        auto bytes = SetHeader(allocator, tag, 1, 8);
        CHECK(SetError(bytes.View(), Wire(WireType::U64), 1) == ErrorCode::CorruptData);
    }

    // 키 tag끼리 다르면 TypeMismatch다(크기 검사보다 먼저).
    auto other = SetHeader(allocator, Wire(WireType::I64), 1, 4);
    CHECK(SetError(other.View(), Wire(WireType::U64), 1) == ErrorCode::TypeMismatch);

    // count > maxCount는 크기 검사보다 먼저 CapacityExceeded다.
    auto many = SetHeader(allocator, Wire(WireType::U32), 3);
    CHECK(SetError(many.View(), Wire(WireType::U32), 2) == ErrorCode::CapacityExceeded);
    CHECK(SetError(many.View(), Wire(WireType::U32), 3) == ErrorCode::CorruptData);

    // count 사전 검사: 고정 폭은 정확히, 가변은 최소 크기(String 8·Soft 24) 이상.
    auto extra = SetHeader(allocator, Wire(WireType::U32), 2, 9);
    CHECK(SetError(extra.View(), Wire(WireType::U32), 10) == ErrorCode::CorruptData);
    auto shortSoft = SetHeader(allocator, Wire(WireType::SoftObjectReference), 2, 47);
    CHECK(SetError(shortSoft.View(), Wire(WireType::SoftObjectReference), 10) ==
          ErrorCode::CorruptData);

    // header 잘림.
    const uint8 truncated[] = {2, 0, 0, 0, 1, 0, 0};
    for (usize length = 0; length < sizeof(truncated); ++length) {
        CHECK(SetError(Span<const uint8>(truncated, length), Wire(WireType::U32), 1) ==
              ErrorCode::CorruptData);
    }

    // count만큼의 work를 원소 읽기 전에 청구한다.
    auto large = SetHeader(allocator, Wire(WireType::U64), 1000, 8000);
    CHECK(SetError(large.View(), Wire(WireType::U64), 1000) == ErrorCode::None);
    ArchiveLimits smallWork;
    smallWork.maxWork = 500;
    CHECK(SetError(large.View(), Wire(WireType::U64), 1000, {}, smallWork) ==
          ErrorCode::CapacityExceeded);

    // exact 가변 원소(Digest = Bytes {40, true})는 원소마다 길이 prefix를 본다.
    auto digests = SetHeader(allocator, Wire(WireType::Bytes), 2);
    AppendU64(digests, 0);
    AppendU64(digests, 64);
    for (usize index = 0; index < 64; ++index) {
        REQUIRE(digests.TryPushBack(0));
    }

    CHECK(SetError(digests.View(), Wire(WireType::Bytes), 2, {40, true}) == ErrorCode::CorruptData);
    CHECK(SetError(digests.View(), Wire(WireType::Bytes), 2) == ErrorCode::None);
}

TEST(SetWire_OpenFieldRejectsValueArrayWireAndWriterLimits) {
    SystemAllocator allocator;
    ArchiveWriter set(allocator);
    auto writer = SetWireWriter::Begin(set, Wire(WireType::U32), 2, 64);
    REQUIRE(writer && set.WriteU32(1) && set.WriteU32(2) && writer.Value().End());
    RecordBuilder builder(allocator, ParentSchema, 1);
    REQUIRE(builder.AddField(KeyedField, WireType::Set, set.View()));
    // 같은 payload를 ValueArray wire로 둔 필드(Array → Set schema 변경)는 Set으로 열리지 않는다.
    REQUIRE(builder.AddField(OtherField, WireType::ValueArray, set.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto parent = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto field = parent.Value().Find(KeyedField);
    auto other = parent.Value().Find(OtherField);
    REQUIRE(field && other);
    auto reader = SetWireReader::OpenField(parent.Value(), field.Value(), Wire(WireType::U32), 2);
    REQUIRE(reader && reader.Value().Count() == 2);
    CHECK(reader.Value().Elements().ReadU32().Value() == 1);
    CHECK(reader.Value().Elements().ReadU32().Value() == 2);
    CHECK(reader.Value().Finish());
    const uint64 operations = parent.Value().OperationsUsed();
    CHECK(SetWireReader::OpenField(parent.Value(), other.Value(), Wire(WireType::U32), 2).Error() ==
          ErrorCode::TypeMismatch);
    CHECK(ValueArrayReader::OpenField(parent.Value(), field.Value(), Wire(WireType::U32), 2)
              .Error() == ErrorCode::TypeMismatch);
    CHECK(parent.Value().OperationsUsed() == operations + 2);
    FieldView foreign = field.Value();
    foreign.payload = set.View();
    CHECK(SetWireReader::OpenField(parent.Value(), foreign, Wire(WireType::U32), 2).Error() ==
          ErrorCode::InvalidArgument);

    // writer: 키 tag가 아니면 InvalidArgument, count·크기 한도는 쓰기 전에 CapacityExceeded,
    // 원소 수를 잘못 쓰면 End가 InvalidArgument다.
    ArchiveWriter out(allocator);
    CHECK(SetWireWriter::Begin(out, Wire(WireType::F64), 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(SetWireWriter::Begin(out, Wire(WireType::Text), 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(SetWireWriter::Begin(out, Wire(WireType::U64), 3, 1024, 2).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(SetWireWriter::Begin(out, Wire(WireType::U64), 2, 8 + 15).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(out.ByteCount() == 0);
    auto exact = SetWireWriter::Begin(out, Wire(WireType::U64), 2, 8 + 16);
    REQUIRE(exact && out.WriteU64(1));
    CHECK(exact.Value().End().Error() == ErrorCode::InvalidArgument);
    REQUIRE(out.WriteU64(2));
    CHECK(exact.Value().End());
    REQUIRE(out.WriteU32(3));
    CHECK(exact.Value().Check().Error() == ErrorCode::CapacityExceeded);
    CHECK(exact.Value().End().Error() == ErrorCode::CapacityExceeded);
}

TEST(MapWire_RoundTripScalarRecordInstancedStructAndTextValues) {
    SystemAllocator allocator;
    // Map<String, U32> {"a": 1, "": 0x01020304}: 키 tag 8, 값 tag 2, count 2, (키, 값) × 2.
    ArchiveWriter scalar(allocator);
    auto scalarMap =
        MapWireWriter::Begin(scalar, Wire(WireType::String), Wire(WireType::U32), 2, 128);
    REQUIRE(scalarMap);
    REQUIRE(scalar.WriteString("a") && scalar.WriteU32(1));
    REQUIRE(scalar.WriteString("") && scalar.WriteU32(0x01020304u));
    REQUIRE(scalarMap.Value().End());
    const uint8 expected[] = {8, 0,   0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0,
                              0, 'a', 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 3, 2, 1};
    CHECK(SameBytes(scalar.View(), expected));

    // Map<Guid, struct>: 값은 u64 길이 + Record.
    ArchiveWriter structs(allocator);
    auto structMap =
        MapWireWriter::Begin(structs, Wire(WireType::Guid), Wire(WireType::Record), 2, 1024);
    REQUIRE(structMap);
    REQUIRE(structs.WriteGuid({1, 1}) && WriteStructValue(structs, 11));
    REQUIRE(structs.WriteGuid({2, 2}) && WriteStructValue(structs, 22));
    REQUIRE(structMap.Value().End());
    CHECK(structs.ByteCount() == 12 + 2 * (16 + 8 + 76));

    // Map<U32, InstancedStruct>(비어 있음과 값)과 Map<Name(String), Text>.
    ArchiveWriter instanced(allocator);
    auto instancedMap = MapWireWriter::Begin(instanced, Wire(WireType::U32),
                                             Wire(WireType::InstancedStruct), 2, 1024);
    REQUIRE(instancedMap);
    REQUIRE(instanced.WriteU32(7));
    auto none = PrefixedRecordWriter::Begin(instanced, Wire(WireType::InstancedStruct));
    REQUIRE(none && none.Value().End());
    REQUIRE(instanced.WriteU32(8));
    {
        auto prefixed = PrefixedRecordWriter::Begin(instanced, Wire(WireType::InstancedStruct));
        REQUIRE(prefixed);
        auto record = RecordWriter::Begin(instanced, ValueStruct, 4);
        REQUIRE(record && record.Value().End());
        REQUIRE(prefixed.Value().End());
    }

    REQUIRE(instancedMap.Value().End());
    ArchiveWriter texts(allocator);
    auto textMap =
        MapWireWriter::Begin(texts, Wire(WireType::String), Wire(WireType::Text), 1, 1024);
    REQUIRE(textMap);
    TextWireValue invariant;
    invariant.form = TextWireForm::Invariant;
    invariant.source = "값";
    REQUIRE(texts.WriteString("Android") && WriteTextWire(texts, invariant));
    REQUIRE(textMap.Value().End());

    RecordBuilder builder(allocator, ParentSchema, 1);
    REQUIRE(builder.AddField(KeyedField, WireType::Map, structs.View()));
    REQUIRE(builder.AddField(OtherField, WireType::Map, instanced.View()));
    REQUIRE(builder.AddField(Guid{15, 9}, WireType::Map, texts.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    auto parent = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);

    // 값 Record는 부모의 OpenNestedRecord로 같은 예산에서 연다.
    auto structField = parent.Value().Find(KeyedField);
    REQUIRE(structField);
    auto structReader = MapWireReader::OpenField(parent.Value(), structField.Value(),
                                                 Wire(WireType::Guid), Wire(WireType::Record), 8);
    REQUIRE(structReader && structReader.Value().Count() == 2);
    for (uint32 index = 1; index <= 2; ++index) {
        auto key = structReader.Value().Entries().ReadGuid();
        CHECK(key && key.Value() == (Guid{index, index}));
        auto value = ReadPrefixedRecord(structReader.Value().Entries());
        REQUIRE(value);
        auto record =
            parent.Value().OpenNestedRecord(allocator, value.Value(), ValueStruct, 1, 1, 4);
        REQUIRE(record);
        auto member = record.Value().Find(ValueMember);
        REQUIRE(member);
        auto memberValue = record.Value().OpenValue(member.Value());
        REQUIRE(memberValue);
        auto number = memberValue.Value().ReadU32();
        CHECK(number && number.Value() == index * 11);
    }

    CHECK(structReader.Value().Finish());

    auto instancedField = parent.Value().Find(OtherField);
    REQUIRE(instancedField);
    auto instancedReader =
        MapWireReader::OpenField(parent.Value(), instancedField.Value(), Wire(WireType::U32),
                                 Wire(WireType::InstancedStruct), 8);
    REQUIRE(instancedReader && instancedReader.Value().Count() == 2);
    CHECK(instancedReader.Value().Entries().ReadU32().Value() == 7);
    auto emptyValue = ReadInstancedStructElement(instancedReader.Value().Entries());
    CHECK(emptyValue && emptyValue.Value().IsEmpty());
    CHECK(instancedReader.Value().Entries().ReadU32().Value() == 8);
    auto filled = ReadInstancedStructElement(instancedReader.Value().Entries());
    CHECK(filled && filled.Value().schema == ValueStruct && filled.Value().schemaVersion == 4);
    CHECK(instancedReader.Value().Finish());

    auto textField = parent.Value().Find(Guid{15, 9});
    REQUIRE(textField);
    auto textReader = MapWireReader::OpenField(parent.Value(), textField.Value(),
                                               Wire(WireType::String), Wire(WireType::Text), 8);
    REQUIRE(textReader && textReader.Value().Count() == 1);
    CHECK(textReader.Value().Entries().ReadStringView().Value() == StringView("Android"));
    auto text = ReadTextWire(textReader.Value().Entries());
    CHECK(text && text.Value().form == TextWireForm::Invariant &&
          text.Value().source == StringView("값"));
    CHECK(textReader.Value().Finish());

    // OpenField 오류: Map이 아닌 wire의 필드는 Map으로, Map 필드는 Set으로 열리지 않는다
    // (TypeMismatch — 실패한 호출도 operation 하나를 소비한다). 다른 Record의 범위는
    // InvalidArgument다.
    ArchiveWriter setPayload(allocator);
    auto setWriter = SetWireWriter::Begin(setPayload, WireType::U32, 0, 64);
    REQUIRE(setWriter && setWriter.Value().End());
    RecordBuilder other(allocator, ParentSchema, 1);
    REQUIRE(other.AddField(KeyedField, WireType::Set, setPayload.View()));
    REQUIRE(other.AddField(OtherField, WireType::ValueArray, setPayload.View()));
    auto otherBytes = other.Build();
    REQUIRE(otherBytes);
    auto otherParent = RecordReader::Open(allocator, otherBytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(otherParent);
    auto setField = otherParent.Value().Find(KeyedField);
    auto arrayField = otherParent.Value().Find(OtherField);
    REQUIRE(setField && arrayField);
    const uint64 operations = otherParent.Value().OperationsUsed();
    CHECK(MapWireReader::OpenField(otherParent.Value(), setField.Value(), WireType::U32,
                                   WireType::U32, 8)
              .Error() == ErrorCode::TypeMismatch);
    CHECK(MapWireReader::OpenField(otherParent.Value(), arrayField.Value(), WireType::U32,
                                   WireType::U32, 8)
              .Error() == ErrorCode::TypeMismatch);
    CHECK(otherParent.Value().OperationsUsed() == operations + 2);
    CHECK(MapWireReader::OpenField(otherParent.Value(), structField.Value(), WireType::Guid,
                                   WireType::Record, 8)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(SetWireReader::OpenField(parent.Value(), structField.Value(), Wire(WireType::Guid), 8)
              .Error() == ErrorCode::TypeMismatch);

    // 이동된 reader는 항목 0개다.
    auto archive = ArchiveReader::Create(scalar.View());
    REQUIRE(archive);
    auto reader =
        MapWireReader::Open(Move(archive.Value()), Wire(WireType::String), Wire(WireType::U32), 2);
    REQUIRE(reader);
    MapWireReader moved(Move(reader.Value()));
    CHECK(reader.Value().Count() == 0 && moved.Count() == 2);
}

TEST(MapWire_HeaderChecksRunInOrder) {
    SystemAllocator allocator;
    const uint32 u32 = Wire(WireType::U32);
    const uint32 u64 = Wire(WireType::U64);
    auto valid = MapHeader(allocator, u32, u64, 1, 12);
    CHECK(MapError(valid.View(), u32, u64, 1) == ErrorCode::None);

    // 기대 키 tag가 키 자리의 것이 아니거나 기대 값 tag가 값 자리의 것이 아니면 InvalidArgument다.
    for (uint32 tag : NotKeyTags) {
        CHECK(MapError(valid.View(), tag, u64, 1) == ErrorCode::InvalidArgument);
    }

    const uint32 notValueTags[] = {Wire(WireType::RecordList),
                                   Wire(WireType::ValueArray),
                                   Wire(WireType::Optional),
                                   Wire(WireType::Set),
                                   Wire(WireType::Map),
                                   0u,
                                   99u};
    for (uint32 tag : notValueTags) {
        CHECK(MapError(valid.View(), u32, tag, 1) == ErrorCode::InvalidArgument);
    }

    // 실수·InstancedStruct·Text·Record는 값 자리에서 받는다(키 자리에서는 아니다).
    const uint32 valueOnly[] = {Wire(WireType::F32), Wire(WireType::F64),
                                Wire(WireType::InstancedStruct), Wire(WireType::Text),
                                Wire(WireType::Record)};
    for (uint32 tag : valueOnly) {
        auto bytes = MapHeader(allocator, u32, tag, 0);
        CHECK(MapError(bytes.View(), u32, tag, 1) == ErrorCode::None);
        auto swapped = MapHeader(allocator, tag, u32, 0);
        CHECK(MapError(swapped.View(), u32, u32, 1) == ErrorCode::CorruptData);
    }

    // 저장 tag가 그 자리의 것이 아니면 InvalidArgument가 TypeMismatch보다 먼저다(키는 맞고 값이
    // 형식 밖이어도, 키가 달라도).
    for (uint32 tag : notValueTags) {
        auto bytes = MapHeader(allocator, Wire(WireType::I64), tag, 1, 12);
        CHECK(MapError(bytes.View(), u32, u64, 1) == ErrorCode::CorruptData);
    }

    // 키·값 tag가 다르면 TypeMismatch다.
    auto keyMismatch = MapHeader(allocator, Wire(WireType::I64), u64, 1, 16);
    CHECK(MapError(keyMismatch.View(), u64, u64, 1) == ErrorCode::TypeMismatch);
    auto valueMismatch = MapHeader(allocator, u32, Wire(WireType::F32), 1, 8);
    CHECK(MapError(valueMismatch.View(), u32, u32, 1) == ErrorCode::TypeMismatch);

    // count > maxCount는 크기보다 먼저 CapacityExceeded, count × (키 + 값) 사전 검사.
    auto many = MapHeader(allocator, u32, u64, 3);
    CHECK(MapError(many.View(), u32, u64, 2) == ErrorCode::CapacityExceeded);
    CHECK(MapError(many.View(), u32, u64, 3) == ErrorCode::CorruptData);
    auto fixedExtra = MapHeader(allocator, u32, u64, 2, 25);
    CHECK(MapError(fixedExtra.View(), u32, u64, 10) == ErrorCode::CorruptData);
    auto fixedExact = MapHeader(allocator, u32, u64, 2, 24);
    CHECK(MapError(fixedExact.View(), u32, u64, 10) == ErrorCode::None);
    // 가변이 섞이면 하한만 본다: String 키 8 + Record 값 52 = 60, 두 항목이면 정확히 120이 경계다.
    auto shortRecords =
        MapHeader(allocator, Wire(WireType::String), Wire(WireType::Record), 2, 119);
    CHECK(MapError(shortRecords.View(), Wire(WireType::String), Wire(WireType::Record), 10) ==
          ErrorCode::CorruptData);
    auto exactRecords =
        MapHeader(allocator, Wire(WireType::String), Wire(WireType::Record), 2, 120);
    CHECK(MapError(exactRecords.View(), Wire(WireType::String), Wire(WireType::Record), 10) ==
          ErrorCode::None);
    auto enoughRecords =
        MapHeader(allocator, Wire(WireType::String), Wire(WireType::Record), 2, 125);
    CHECK(MapError(enoughRecords.View(), Wire(WireType::String), Wire(WireType::Record), 10) ==
          ErrorCode::None);

    // header 잘림(키 tag·값 tag·count의 모든 접두).
    for (usize length = 0; length < 12; ++length) {
        CHECK(MapError(Span<const uint8>(valid.Data(), length), u32, u64, 1) ==
              ErrorCode::CorruptData);
    }

    // count만큼의 work를 항목 읽기 전에 청구한다.
    auto large = MapHeader(allocator, u32, u32, 1000, 8000);
    CHECK(MapError(large.View(), u32, u32, 1000) == ErrorCode::None);
    ArchiveLimits smallWork;
    smallWork.maxWork = 500;
    CHECK(MapError(large.View(), u32, u32, 1000, {}, {}, smallWork) == ErrorCode::CapacityExceeded);

    // 키·값이 모두 exact면 항목마다 가변 칸의 길이 prefix를 본다(Digest 키 + U32 값).
    const WireValueShape digest{40, true};
    auto digests = MapHeader(allocator, Wire(WireType::Bytes), u32, 2);
    AppendU64(digests, 32);
    for (usize index = 0; index < 32; ++index) {
        REQUIRE(digests.TryPushBack(0));
    }

    AppendU32(digests, 1);
    AppendU64(digests, 31); // 합계는 맞지만 둘째 키의 길이 prefix가 틀렸다
    for (usize index = 0; index < 32; ++index) {
        REQUIRE(digests.TryPushBack(0));
    }

    AppendU32(digests, 2);
    CHECK(MapError(digests.View(), Wire(WireType::Bytes), u32, 2, digest) ==
          ErrorCode::CorruptData);
    CHECK(MapError(digests.View(), Wire(WireType::Bytes), u32, 2) == ErrorCode::None);
    digests[12 + 44 + 0] = 32;
    CHECK(MapError(digests.View(), Wire(WireType::Bytes), u32, 2, digest) == ErrorCode::None);
}

TEST(MapWire_SizePrecheckRunsBeforeAnyAllocation) {
    SystemAllocator backing;
    test::NFailAllocator counting(backing);
    // count가 uint32 최대이고 maxCount를 풀어도 남은 byte 검사가 할당 전에 끊는다. 부모를 연 뒤의
    // OpenField·Open은 아무것도 할당하지 않는다.
    auto huge = MapHeader(backing, Wire(WireType::String), Wire(WireType::Record),
                          NumericLimits<uint32>::Max(), 64);
    RecordBuilder builder(backing, ParentSchema, 1);
    REQUIRE(builder.AddField(KeyedField, WireType::Map, huge.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    RecordLimits generous;
    generous.archive.maxOperations = NumericLimits<uint64>::Max();
    generous.archive.maxWork = NumericLimits<uint64>::Max();
    auto parent = RecordReader::Open(counting, bytes.Value().View(), ParentSchema, 1, 1, generous);
    REQUIRE(parent);
    auto field = parent.Value().Find(KeyedField);
    REQUIRE(field);
    // reader API는 allocator를 받지 않지만, 부모의 allocator로 색인을 만들게 바뀌면(RecordList처럼)
    // 첫 할당 실패가 OutOfMemory로 드러난다. 사전 검사가 먼저이므로 InvalidArgument여야 한다.
    counting.FailOnAttempt(1);
    auto opened = MapWireReader::OpenField(parent.Value(), field.Value(), Wire(WireType::String),
                                           Wire(WireType::Record), NumericLimits<uint32>::Max());
    CHECK(!opened && opened.Error() == ErrorCode::CorruptData);
    CHECK(counting.Attempts() == 0);
    auto set = SetHeader(backing, Wire(WireType::Guid), NumericLimits<uint32>::Max(), 16);
    CHECK(SetError(set.View(), Wire(WireType::Guid), NumericLimits<uint32>::Max()) ==
          ErrorCode::CorruptData);
}

TEST(MapWire_WriterLimitsAndMisuse) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    const uint32 u32 = Wire(WireType::U32);
    // 키 자리가 받지 않는 tag(실수·Text·Record)와 값 자리가 받지 않는 tag(컨테이너)는 쓰기 전에
    // InvalidArgument다.
    CHECK(MapWireWriter::Begin(out, Wire(WireType::F32), u32, 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MapWireWriter::Begin(out, Wire(WireType::Record), u32, 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MapWireWriter::Begin(out, u32, Wire(WireType::ValueArray), 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MapWireWriter::Begin(out, u32, Wire(WireType::Map), 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(MapWireWriter::Begin(out, u32, Wire(WireType::Text), 0, 64, 1, {}, {1, true}).Error() ==
          ErrorCode::InvalidArgument);
    // count·크기 한도: header 12 + count × (키 + 값 최소 크기).
    CHECK(MapWireWriter::Begin(out, u32, u32, 3, 1024, 2).Error() == ErrorCode::CapacityExceeded);
    CHECK(MapWireWriter::Begin(out, u32, u32, 2, 12 + 15).Error() == ErrorCode::CapacityExceeded);
    CHECK(MapWireWriter::Begin(out, u32, u32, 0, 11).Error() == ErrorCode::CapacityExceeded);
    CHECK(out.ByteCount() == 0);

    auto exact = MapWireWriter::Begin(out, u32, u32, 2, 12 + 16);
    REQUIRE(exact);
    REQUIRE(out.WriteU32(1) && out.WriteU32(2) && out.WriteU32(3));
    CHECK(exact.Value().End().Error() == ErrorCode::InvalidArgument); // 값 하나가 모자라다
    REQUIRE(out.WriteU32(4));
    CHECK(exact.Value().Check() && exact.Value().End());
    REQUIRE(out.WriteU32(5));
    CHECK(exact.Value().Check().Error() == ErrorCode::CapacityExceeded);
    CHECK(exact.Value().End().Error() == ErrorCode::CapacityExceeded);

    // 가변 값은 합계 하한만 본다: Record 값 자리(최소 52)에 덜 쓰면 InvalidArgument다.
    ArchiveWriter records(allocator);
    auto variable = MapWireWriter::Begin(records, u32, Wire(WireType::Record), 1, 1024);
    REQUIRE(variable && records.WriteU32(1) && records.WriteU64(0));
    CHECK(variable.Value().End().Error() == ErrorCode::InvalidArgument);
}

TEST(KeyedWire_ArgumentErrorsKeepPayloadAndShrunkOutputIsRejected) {
    SystemAllocator allocator;
    // 기대 tag·shape 오류는 payload를 옮기기 전에 거부하므로 호출자 reader가 그대로다(ValueArray·
    // Set·Map 같은 규칙).
    auto bytes = MapHeader(allocator, Wire(WireType::U32), Wire(WireType::U32), 1, 8);
    auto payload = ArchiveReader::Create(bytes.View());
    REQUIRE(payload);
    CHECK(MapWireReader::Open(Move(payload.Value()), WireType::F32, WireType::U32, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(SetWireReader::Open(Move(payload.Value()), WireType::Text, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ValueArrayReader::Open(Move(payload.Value()), WireType::Map, 1).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(payload.Value().Remaining() == bytes.Size());
    auto map = MapWireReader::Open(Move(payload.Value()), WireType::U32, WireType::U32, 1);
    REQUIRE(map && map.Value().Count() == 1);

    // 키·값 shape 최소 크기의 합이 usize를 넘으면(감기면 사전 검사가 무력해진다) reader·writer 모두
    // InvalidArgument다.
    const WireValueShape huge{MaxSize / 2 + 1, false};
    auto noEntries = MapHeader(allocator, Wire(WireType::String), Wire(WireType::String), 1000000);
    CHECK(MapError(noEntries.View(), Wire(WireType::String), Wire(WireType::String), 1000000, huge,
                   huge) == ErrorCode::InvalidArgument);
    ArchiveWriter out(allocator);
    CHECK(MapWireWriter::Begin(out, WireType::String, WireType::String, 1000000, 64,
                               NumericLimits<uint32>::Max(), huge, huge)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(out.ByteCount() == 0);

    // 호출자가 End 전에 out을 Begin 위치보다 짧게 줄이면 Check·End가 InvalidArgument다.
    ArchiveWriter setOut(allocator);
    REQUIRE(setOut.WriteU32(9));
    auto set = SetWireWriter::Begin(setOut, WireType::U32, 1, 64);
    REQUIRE(set && setOut.WriteU32(1));
    setOut.Clear();
    CHECK(set.Value().Check().Error() == ErrorCode::InvalidArgument);
    CHECK(set.Value().End().Error() == ErrorCode::InvalidArgument);
    ArchiveWriter mapOut(allocator);
    REQUIRE(mapOut.WriteU32(9));
    auto mapWriter = MapWireWriter::Begin(mapOut, WireType::U32, WireType::U32, 0, 64);
    REQUIRE(mapWriter);
    mapOut.Clear();
    CHECK(mapWriter.Value().Check().Error() == ErrorCode::InvalidArgument);
    CHECK(mapWriter.Value().End().Error() == ErrorCode::InvalidArgument);
    ArchiveWriter arrayOut(allocator);
    REQUIRE(arrayOut.WriteU32(9));
    auto array = ValueArrayWriter::Begin(arrayOut, WireType::U32, 0, 64);
    REQUIRE(array);
    arrayOut.Clear();
    CHECK(array.Value().Check().Error() == ErrorCode::InvalidArgument);
    CHECK(array.Value().End().Error() == ErrorCode::InvalidArgument);
}
} // namespace keyed_wire_tests_file
