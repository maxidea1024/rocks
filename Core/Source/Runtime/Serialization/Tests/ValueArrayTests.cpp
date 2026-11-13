#include <Core/Memory/SystemAllocator.h>
#include <Serialization/ValueArray.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <Platform/Memory.h>

using namespace rocks;

// 단일 값 원소 배열(WireType::ValueArray)의 형식·header 검사 순서·원소 내용·예산 공유·writer 한도
// 시험이다(Docs/Design/ReflectionFoundations.ko.md 5.4·5.6절).
namespace {

constexpr Guid ParentSchema{13, 1};
constexpr Guid ArrayField{13, 2};
constexpr Guid OtherField{13, 3};

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

// payload를 새 예산의 ArchiveReader로 열어 header를 검사한다.
Result<ValueArrayReader> OpenPayload(Span<const uint8> payload, uint32 elementWire, uint32 maxCount,
                                     ValueArrayShape shape = {},
                                     ArchiveLimits limits = {}) noexcept {
    auto archive = ArchiveReader::Create(payload, limits);
    if (!archive) {
        return Result<ValueArrayReader>::Failure(archive.Error());
    }

    return ValueArrayReader::Open(Move(archive.Value()), elementWire, maxCount, shape);
}

ErrorCode OpenError(Span<const uint8> payload, uint32 elementWire, uint32 maxCount,
                    ValueArrayShape shape = {}, ArchiveLimits limits = {}) noexcept {
    auto opened = OpenPayload(payload, elementWire, maxCount, shape, limits);
    return opened ? ErrorCode::None : opened.Error();
}

// header(원소 tag, count) 뒤에 extra byte를 붙인 payload다.
Array<uint8> Header(Allocator& allocator, uint32 tag, uint32 count, usize extra = 0,
                    uint8 fill = 0) noexcept {
    Array<uint8> bytes(allocator);
    AppendU32(bytes, tag);
    AppendU32(bytes, count);
    for (usize index = 0; index < extra; ++index) {
        static_cast<void>(bytes.TryPushBack(fill));
    }

    return bytes;
}

// 원소 wire마다 3개를 쓰고(writer) 다시 읽는다(reader). 실패하면 false다.
bool RoundTrip(Allocator& allocator, WireType wire) noexcept {
    ArchiveWriter out(allocator);
    // 바깥 writer 뒤에 제자리로 쓰는지 본다.
    if (!out.WriteU32(0x5a5a5a5a)) {
        return false;
    }

    auto writer = ValueArrayWriter::Begin(out, Wire(wire), 3, 4096);
    if (!writer) {
        return false;
    }

    for (uint32 index = 0; index < 3; ++index) {
        Result<void> written = Result<void>::Success();
        switch (wire) {
        case WireType::Bool:
            written = out.WriteBool(index != 1);
            break;
        case WireType::U32:
            written = out.WriteU32(0xfffffff0u + index);
            break;
        case WireType::U64:
            written = out.WriteU64(0xfffffffff0ull + index);
            break;
        case WireType::I64:
            written = out.WriteI64(-5 + static_cast<int64>(index));
            break;
        case WireType::F32:
            written = out.WriteF32(1.5f * static_cast<float>(index));
            break;
        case WireType::F64:
            written = out.WriteF64(-2.25 * static_cast<double>(index));
            break;
        case WireType::Guid:
            written = out.WriteGuid({index + 1, index + 2});
            break;
        case WireType::String:
            written = out.WriteString(index == 0 ? StringView("") : StringView("원소"));
            break;
        case WireType::Bytes: {
            const uint8 data[] = {1, 2, 3};
            written = out.WriteBytes(Span<const uint8>(data, index));
            break;
        }
        case WireType::SoftObjectReference:
            written = out.WriteGuid({7, index});
            if (written) {
                written =
                    out.WriteString(index == 2 ? StringView("/Game/Map.Actor") : StringView(""));
            }

            break;
        default:
            return false;
        }

        if (!written || !writer.Value().Check()) {
            return false;
        }
    }

    if (!writer.Value().End()) {
        return false;
    }

    const Span<const uint8> payload(out.View().Data() + 4, out.ByteCount() - 4);
    auto reader = OpenPayload(payload, Wire(wire), 3);
    if (!reader || reader.Value().Count() != 3) {
        return false;
    }

    ArchiveReader& elements = reader.Value().Elements();
    for (uint32 index = 0; index < 3; ++index) {
        bool same = false;
        switch (wire) {
        case WireType::Bool: {
            auto value = elements.ReadBool();
            same = value && value.Value() == (index != 1);
            break;
        }
        case WireType::U32: {
            auto value = elements.ReadU32();
            same = value && value.Value() == 0xfffffff0u + index;
            break;
        }
        case WireType::U64: {
            auto value = elements.ReadU64();
            same = value && value.Value() == 0xfffffffff0ull + index;
            break;
        }
        case WireType::I64: {
            auto value = elements.ReadI64();
            same = value && value.Value() == -5 + static_cast<int64>(index);
            break;
        }
        case WireType::F32: {
            auto value = elements.ReadF32();
            same = value && value.Value() == 1.5f * static_cast<float>(index);
            break;
        }
        case WireType::F64: {
            auto value = elements.ReadF64();
            same = value && value.Value() == -2.25 * static_cast<double>(index);
            break;
        }
        case WireType::Guid: {
            auto value = elements.ReadGuid();
            same = value && value.Value() == Guid{index + 1, index + 2};
            break;
        }
        case WireType::String: {
            auto value = elements.ReadStringView();
            same = value && value.Value() == (index == 0 ? StringView("") : StringView("원소"));
            break;
        }
        case WireType::Bytes: {
            auto value = elements.ReadBytes();
            same = value && value.Value().Size() == index;
            break;
        }
        case WireType::SoftObjectReference: {
            auto asset = elements.ReadGuid();
            auto path = elements.ReadStringView();
            same = asset && path && asset.Value() == Guid{7, index} &&
                   path.Value() == (index == 2 ? StringView("/Game/Map.Actor") : StringView(""));
            break;
        }
        default:
            break;
        }

        if (!same) {
            return false;
        }
    }

    return static_cast<bool>(reader.Value().Finish());
}

} // namespace

TEST(ValueArray_RoundTripEveryElementWire) {
    SystemAllocator allocator;
    const WireType wires[] = {WireType::Bool,  WireType::U32,
                              WireType::U64,   WireType::I64,
                              WireType::F32,   WireType::F64,
                              WireType::Guid,  WireType::String,
                              WireType::Bytes, WireType::SoftObjectReference};
    for (WireType wire : wires) {
        CHECK(RoundTrip(allocator, wire));
    }

    // 원소 wire 크기 표: 고정 폭은 exact, 가변은 최소 크기, 그 밖은 InvalidArgument다.
    CHECK(ValueArrayElementShape(WireType::Bool).Value().minElementBytes == 1);
    CHECK(ValueArrayElementShape(WireType::F32).Value().minElementBytes == 4);
    CHECK(ValueArrayElementShape(WireType::I64).Value().exact);
    CHECK(ValueArrayElementShape(WireType::Guid).Value().minElementBytes == 16);
    CHECK(ValueArrayElementShape(WireType::String).Value().minElementBytes == 8 &&
          !ValueArrayElementShape(WireType::String).Value().exact);
    CHECK(ValueArrayElementShape(WireType::SoftObjectReference).Value().minElementBytes == 24);
    CHECK(ValueArrayElementShape(WireType::Record).Error() == ErrorCode::InvalidArgument);
    CHECK(ValueArrayElementShape(WireType::RecordList).Error() == ErrorCode::InvalidArgument);
    CHECK(ValueArrayElementShape(WireType::ValueArray).Error() == ErrorCode::InvalidArgument);
    CHECK(ValueArrayElementShape(0u).Error() == ErrorCode::InvalidArgument);
    CHECK(ValueArrayElementShape(99u).Error() == ErrorCode::InvalidArgument);
}

TEST(ValueArray_ByteLayoutMatchesFormat) {
    SystemAllocator allocator;
    // U32 [1, 0x01020304]: tag 2, count 2, 원소 4 byte씩(little-endian).
    ArchiveWriter out(allocator);
    auto writer = ValueArrayWriter::Begin(out, Wire(WireType::U32), 2, 64);
    REQUIRE(writer);
    REQUIRE(out.WriteU32(1) && out.WriteU32(0x01020304u));
    REQUIRE(writer.Value().End());
    const uint8 numbers[] = {2, 0, 0, 0, 2, 0, 0, 0, 1, 0, 0, 0, 4, 3, 2, 1};
    CHECK(SameBytes(out.View(), numbers));

    // String ["ab"]: tag 8, count 1, u64 길이 2, byte.
    ArchiveWriter text(allocator);
    auto strings = ValueArrayWriter::Begin(text, Wire(WireType::String), 1, 64);
    REQUIRE(strings);
    REQUIRE(text.WriteString("ab"));
    REQUIRE(strings.Value().End());
    const uint8 expected[] = {8, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 'a', 'b'};
    CHECK(SameBytes(text.View(), expected));

    // 빈 배열도 header를 쓴다(count 0).
    ArchiveWriter empty(allocator);
    auto none = ValueArrayWriter::Begin(empty, Wire(WireType::Guid), 0, 8);
    REQUIRE(none);
    REQUIRE(none.Value().End());
    const uint8 emptyBytes[] = {7, 0, 0, 0, 0, 0, 0, 0};
    CHECK(SameBytes(empty.View(), emptyBytes));
    auto opened = OpenPayload(empty.View(), Wire(WireType::Guid), 0);
    REQUIRE(opened);
    CHECK(opened.Value().Count() == 0 && opened.Value().Finish());
}

TEST(ValueArray_HeaderChecksRunInOrder) {
    SystemAllocator allocator;
    // 기대 원소 tag 자체가 원소가 될 수 없으면 InvalidArgument다.
    auto valid = Header(allocator, Wire(WireType::U32), 1, 4);
    CHECK(OpenError(valid.View(), Wire(WireType::U32), 1) == ErrorCode::None);
    CHECK(OpenError(valid.View(), Wire(WireType::Record), 1) == ErrorCode::InvalidArgument);

    // 저장 tag가 Record·RecordList·ValueArray·모르는 값·0이면(기대 tag와 달라도) InvalidArgument다.
    const uint32 invalidTags[] = {Wire(WireType::Record), Wire(WireType::RecordList),
                                  Wire(WireType::ValueArray), 0u, 99u};
    for (uint32 tag : invalidTags) {
        auto bytes = Header(allocator, tag, 1, 4);
        CHECK(OpenError(bytes.View(), Wire(WireType::U32), 1) == ErrorCode::InvalidArgument);
    }

    // 원소 tag끼리 다르면 TypeMismatch다(크기 검사보다 먼저).
    auto otherElement = Header(allocator, Wire(WireType::U64), 1, 4);
    CHECK(OpenError(otherElement.View(), Wire(WireType::U32), 1) == ErrorCode::TypeMismatch);

    // count > maxCount는 남은 byte가 모자라도 CapacityExceeded다(크기 검사보다 먼저).
    auto tooMany = Header(allocator, Wire(WireType::U32), 3);
    CHECK(OpenError(tooMany.View(), Wire(WireType::U32), 2) == ErrorCode::CapacityExceeded);
    CHECK(OpenError(tooMany.View(), Wire(WireType::U32), 3) == ErrorCode::InvalidArgument);

    // 가변 원소: count × 최소 크기(String 8)가 남은 byte보다 크면 InvalidArgument다.
    auto shortStrings = Header(allocator, Wire(WireType::String), 2, 15);
    CHECK(OpenError(shortStrings.View(), Wire(WireType::String), 10) == ErrorCode::InvalidArgument);
    auto softShort = Header(allocator, Wire(WireType::SoftObjectReference), 1, 23);
    CHECK(OpenError(softShort.View(), Wire(WireType::SoftObjectReference), 10) ==
          ErrorCode::InvalidArgument);

    // 고정 폭은 남은 byte가 정확히 count × 폭이어야 한다(많아도 InvalidArgument).
    auto extra = Header(allocator, Wire(WireType::U32), 2, 9);
    CHECK(OpenError(extra.View(), Wire(WireType::U32), 10) == ErrorCode::InvalidArgument);

    // 스키마 최소 크기가 wire 최소보다 크면 그것을 쓴다(Digest = Bytes 40 exact).
    const ValueArrayShape digest{40, true};
    auto digestShort = Header(allocator, Wire(WireType::Bytes), 1, 39);
    CHECK(OpenError(digestShort.View(), Wire(WireType::Bytes), 10, digest) ==
          ErrorCode::InvalidArgument);
    auto digestLong = Header(allocator, Wire(WireType::Bytes), 1, 41);
    CHECK(OpenError(digestLong.View(), Wire(WireType::Bytes), 10, digest) ==
          ErrorCode::InvalidArgument);
    CHECK(OpenError(digestLong.View(), Wire(WireType::Bytes), 10) == ErrorCode::None);

    // header 잘림.
    const uint8 truncated[] = {2, 0, 0, 0, 1, 0, 0};
    CHECK(OpenError(Span<const uint8>(truncated, 3), Wire(WireType::U32), 1) ==
          ErrorCode::InvalidArgument);
    CHECK(OpenError(Span<const uint8>(truncated, 7), Wire(WireType::U32), 1) ==
          ErrorCode::InvalidArgument);

    // count만큼의 work를 원소 읽기 전에 청구한다: 크기는 맞지만 work 한도가 count보다 작으면
    // CapacityExceeded다.
    auto many = Header(allocator, Wire(WireType::U64), 1000, 8000);
    CHECK(OpenError(many.View(), Wire(WireType::U64), 1000) == ErrorCode::None);
    ArchiveLimits smallWork;
    smallWork.maxWork = 500;
    CHECK(OpenError(many.View(), Wire(WireType::U64), 1000, {}, smallWork) ==
          ErrorCode::CapacityExceeded);
}

TEST(ValueArray_SizePrecheckRunsBeforeAnyAllocation) {
    SystemAllocator backing;
    test::NFailAllocator counting(backing);
    counting.DisableFailure();
    // count가 uint32 최대이고 maxCount를 풀어도 남은 byte 검사가 원소 할당 전에 끊는다. 부모를
    // 연 뒤의 OpenField·Open은 아무것도 할당하지 않는다.
    auto huge = Header(backing, Wire(WireType::String), 0xffffffffu, 16);
    RecordBuilder builder(backing, ParentSchema, 1);
    REQUIRE(builder.AddField(ArrayField, WireType::ValueArray, huge.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);
    RecordLimits generous;
    generous.archive.maxOperations = ~0ull;
    generous.archive.maxWork = ~0ull;
    auto parent = RecordReader::Open(counting, bytes.Value().View(), ParentSchema, 1, 1, generous);
    REQUIRE(parent);
    auto field = parent.Value().Find(ArrayField);
    REQUIRE(field);
    counting.DisableFailure();
    auto opened = ValueArrayReader::OpenField(parent.Value(), field.Value(), Wire(WireType::String),
                                              0xffffffffu);
    CHECK(!opened && opened.Error() == ErrorCode::InvalidArgument);
    CHECK(counting.Attempts() == 0);
}

TEST(ValueArray_ElementContentIsValidatedByReads) {
    SystemAllocator allocator;
    // bool은 0/1만 받는다.
    auto bools = Header(allocator, Wire(WireType::Bool), 2);
    REQUIRE(bools.TryPushBack(1) && bools.TryPushBack(2));
    auto boolReader = OpenPayload(bools.View(), Wire(WireType::Bool), 2);
    REQUIRE(boolReader);
    CHECK(boolReader.Value().Elements().ReadBool());
    CHECK(boolReader.Value().Elements().ReadBool().Error() == ErrorCode::InvalidArgument);

    // 잘못된 UTF-8 문자열 원소.
    auto strings = Header(allocator, Wire(WireType::String), 1);
    AppendU64(strings, 2);
    REQUIRE(strings.TryPushBack(0xc3) && strings.TryPushBack(0x28));
    auto stringReader = OpenPayload(strings.View(), Wire(WireType::String), 1);
    REQUIRE(stringReader);
    CHECK(stringReader.Value().Elements().ReadStringView().Error() == ErrorCode::InvalidArgument);

    // 원소 길이가 남은 byte보다 크면 잘림(InvalidArgument), 원소를 다 읽고 남은 byte는 trailing.
    auto truncated = Header(allocator, Wire(WireType::String), 1);
    AppendU64(truncated, 5);
    REQUIRE(truncated.TryPushBack('a'));
    auto truncatedReader = OpenPayload(truncated.View(), Wire(WireType::String), 1);
    REQUIRE(truncatedReader);
    CHECK(truncatedReader.Value().Elements().ReadStringView().Error() ==
          ErrorCode::InvalidArgument);
    auto trailing = Header(allocator, Wire(WireType::String), 1);
    AppendU64(trailing, 1);
    REQUIRE(trailing.TryPushBack('a') && trailing.TryPushBack('b'));
    auto trailingReader = OpenPayload(trailing.View(), Wire(WireType::String), 1);
    REQUIRE(trailingReader);
    auto first = trailingReader.Value().Elements().ReadStringView();
    CHECK(first && first.Value() == StringView("a"));
    CHECK(trailingReader.Value().Finish().Error() == ErrorCode::InvalidArgument);

    // 원소를 덜 읽어도 Finish가 남은 byte를 거부한다.
    auto numbers = Header(allocator, Wire(WireType::U32), 2, 8);
    auto numberReader = OpenPayload(numbers.View(), Wire(WireType::U32), 2);
    REQUIRE(numberReader);
    REQUIRE(numberReader.Value().Elements().ReadU32());
    CHECK(numberReader.Value().Finish().Error() == ErrorCode::InvalidArgument);
    REQUIRE(numberReader.Value().Elements().ReadU32());
    CHECK(numberReader.Value().Finish());
}

TEST(ValueArray_OpenFieldSharesParentBudget) {
    SystemAllocator allocator;
    ArchiveWriter payload(allocator);
    auto writer = ValueArrayWriter::Begin(payload, Wire(WireType::U64), 64, 1024);
    REQUIRE(writer);
    for (uint64 index = 0; index < 64; ++index) {
        REQUIRE(payload.WriteU64(index * 3));
    }

    REQUIRE(writer.Value().End());
    ArchiveWriter scalar(allocator);
    REQUIRE(scalar.WriteU32(5));
    RecordBuilder builder(allocator, ParentSchema, 1);
    REQUIRE(builder.AddField(ArrayField, WireType::ValueArray, payload.View()));
    REQUIRE(builder.AddField(OtherField, WireType::U32, scalar.View()));
    auto bytes = builder.Build();
    REQUIRE(bytes);

    // 원소를 모두 읽는 데 드는 부모 예산(operation·work)을 잰다.
    auto read = [&](RecordReader& parent) noexcept -> Result<uint64> {
        auto field = parent.Find(ArrayField);
        if (!field) {
            return Result<uint64>::Failure(field.Error());
        }

        auto array = ValueArrayReader::OpenField(parent, field.Value(), Wire(WireType::U64), 1024);
        if (!array) {
            return Result<uint64>::Failure(array.Error());
        }

        uint64 sum = 0;
        for (uint32 index = 0; index < array.Value().Count(); ++index) {
            auto value = array.Value().Elements().ReadU64();
            if (!value) {
                return Result<uint64>::Failure(value.Error());
            }

            sum += value.Value();
        }

        if (auto finished = array.Value().Finish(); !finished) {
            return Result<uint64>::Failure(finished.Error());
        }

        return Result<uint64>::Success(sum);
    };

    auto full = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(full);
    const uint64 openOperations = full.Value().OperationsUsed();
    auto sum = read(full.Value());
    REQUIRE(sum);
    CHECK(sum.Value() == 3 * (63 * 64 / 2));
    const uint64 used = full.Value().OperationsUsed();
    CHECK(used > openOperations + 64); // 원소마다 부모 예산에 operation이 청구됐다

    // 순회 전체보다 하나 작은 operation 한도로는 끝나지 않는다(새 예산을 받지 않는다).
    RecordLimits tight;
    tight.archive.maxOperations = used - 1;
    auto limited = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1, tight);
    REQUIRE(limited);
    CHECK(read(limited.Value()).Error() == ErrorCode::CapacityExceeded);

    // OpenField 오류: ValueArray가 아닌 필드는 TypeMismatch, 다른 Record의 범위·이동된 parent는
    // InvalidArgument이고, 실패한 호출도 operation 하나를 소비한다.
    auto parent = RecordReader::Open(allocator, bytes.Value().View(), ParentSchema, 1, 1);
    REQUIRE(parent);
    auto other = parent.Value().Find(OtherField);
    auto array = parent.Value().Find(ArrayField);
    REQUIRE(other && array);
    uint64 before = parent.Value().OperationsUsed();
    CHECK(ValueArrayReader::OpenField(parent.Value(), other.Value(), Wire(WireType::U64), 64)
              .Error() == ErrorCode::TypeMismatch);
    CHECK(parent.Value().OperationsUsed() == before + 1);
    FieldView foreign = array.Value();
    foreign.payload = payload.View();
    before = parent.Value().OperationsUsed();
    CHECK(ValueArrayReader::OpenField(parent.Value(), foreign, Wire(WireType::U64), 64).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(parent.Value().OperationsUsed() == before + 1);
    RecordReader moved(Move(parent.Value()));
    CHECK(ValueArrayReader::OpenField(parent.Value(), array.Value(), Wire(WireType::U64), 64)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(ValueArrayReader::OpenField(moved, array.Value(), Wire(WireType::U64), 64));
}

TEST(ValueArray_WriterLimitsAndMisuse) {
    SystemAllocator allocator;
    ArchiveWriter out(allocator);
    CHECK(ValueArrayWriter::Begin(out, Wire(WireType::Record), 0, 64).Error() ==
          ErrorCode::InvalidArgument);
    // header + count × 최소 크기가 maxBytes를 넘으면 쓰기 전에 거부한다.
    CHECK(ValueArrayWriter::Begin(out, Wire(WireType::U64), 2, 8 + 15).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(ValueArrayWriter::Begin(out, Wire(WireType::U32), 0, 7).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(ValueArrayWriter::Begin(out, Wire(WireType::String), 0xffffffffu, 1024).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(out.ByteCount() == 0);

    // 정확히 맞으면 통과한다.
    auto exact = ValueArrayWriter::Begin(out, Wire(WireType::U64), 2, 8 + 16);
    REQUIRE(exact);
    REQUIRE(out.WriteU64(1));
    CHECK(exact.Value().End().Error() == ErrorCode::InvalidArgument); // 원소가 하나 모자라다
    REQUIRE(out.WriteU64(2));
    CHECK(exact.Value().Check() && exact.Value().End());
    REQUIRE(out.WriteU32(3));
    CHECK(exact.Value().Check().Error() == ErrorCode::CapacityExceeded);
    CHECK(exact.Value().End().Error() == ErrorCode::CapacityExceeded);

    // 고정 폭인데 원소 byte가 count × 폭과 다르면 InvalidArgument다.
    ArchiveWriter fixed(allocator);
    auto wrongWidth = ValueArrayWriter::Begin(fixed, Wire(WireType::U32), 1, 64);
    REQUIRE(wrongWidth);
    REQUIRE(fixed.WriteU64(1));
    CHECK(wrongWidth.Value().End().Error() == ErrorCode::InvalidArgument);

    // 가변 원소의 전체 크기 한도(Check로 일찍 끊는다).
    ArchiveWriter strings(allocator);
    auto variable = ValueArrayWriter::Begin(strings, Wire(WireType::String), 2, 8 + 8 + 8 + 4);
    REQUIRE(variable);
    REQUIRE(strings.WriteString("abcd"));
    CHECK(variable.Value().Check());
    REQUIRE(strings.WriteString("x"));
    CHECK(variable.Value().Check().Error() == ErrorCode::CapacityExceeded);
    CHECK(variable.Value().End().Error() == ErrorCode::CapacityExceeded);
}

namespace {

// Bytes 원소(u64 길이 + byte)들의 payload다.
Array<uint8> BytesElements(Allocator& allocator, Span<const usize> lengths) noexcept {
    auto bytes = Header(allocator, Wire(WireType::Bytes), static_cast<uint32>(lengths.Size()));
    for (usize length : lengths) {
        AppendU64(bytes, length);
        for (usize index = 0; index < length; ++index) {
            static_cast<void>(bytes.TryPushBack(static_cast<uint8>(index)));
        }
    }

    return bytes;
}

} // namespace

TEST(ValueArray_ExactVariableElementsCheckEachLength) {
    // 가변 wire에 exact shape를 주면 합계만 맞고 원소 경계가 어긋난 입력도 Open이 거부한다: Digest
    // {40, true} 두 원소의 길이 0·64(합 80)는 count × 40과 같지만 InvalidArgument다.
    SystemAllocator allocator;
    const ValueArrayShape digest{40, true};
    const usize skewed[] = {0, 64};
    auto bad = BytesElements(allocator, skewed);
    CHECK(bad.Size() == 8 + 80);
    CHECK(OpenError(bad.View(), Wire(WireType::Bytes), 10, digest) == ErrorCode::InvalidArgument);
    // 같은 byte도 exact가 아니면 열리고(경계는 Read가 검사한다) 원소를 그대로 읽는다.
    auto loose = OpenPayload(bad.View(), Wire(WireType::Bytes), 10);
    REQUIRE(loose);
    CHECK(loose.Value().Elements().ReadBytes().Value().Size() == 0);
    CHECK(loose.Value().Elements().ReadBytes().Value().Size() == 64);
    CHECK(loose.Value().Finish());

    // 올바른 Digest 원소(길이 32씩)는 열린다. 마지막 원소만 틀려도 거부한다.
    const usize valid[] = {32, 32, 32};
    auto good = BytesElements(allocator, valid);
    auto opened = OpenPayload(good.View(), Wire(WireType::Bytes), 10, digest);
    REQUIRE(opened && opened.Value().Count() == 3);
    for (uint32 index = 0; index < 3; ++index) {
        auto element = opened.Value().Elements().ReadBytes();
        CHECK(element && element.Value().Size() == 32);
    }

    CHECK(opened.Value().Finish());
    const usize lastWrong[] = {32, 32, 31};
    auto tail = BytesElements(allocator, lastWrong);
    REQUIRE(tail.TryPushBack(0)); // 합계를 count × 40으로 맞춘다
    CHECK(OpenError(tail.View(), Wire(WireType::Bytes), 10, digest) == ErrorCode::InvalidArgument);

    // SoftObjectReference의 길이 prefix는 Guid 뒤에 있다(exact {24 + 4, true} = 경로 4 byte). 원소
    // 0의 경로 길이 5 + byte 3, 원소 1의 길이 3 + byte 5는 합계가 2 × 28이지만 경계가 어긋난다.
    auto softElements = [&](const usize(&lengths)[2], const usize(&bytes)[2]) noexcept {
        auto soft = Header(allocator, Wire(WireType::SoftObjectReference), 2);
        for (usize element = 0; element < 2; ++element) {
            AppendU64(soft, 1);
            AppendU64(soft, 2);
            AppendU64(soft, lengths[element]);
            for (usize index = 0; index < bytes[element]; ++index) {
                static_cast<void>(soft.TryPushBack('p'));
            }
        }

        return soft;
    };
    auto skewedSoft = softElements({5, 3}, {3, 5});
    CHECK(skewedSoft.Size() == 8 + 56);
    CHECK(OpenError(skewedSoft.View(), Wire(WireType::SoftObjectReference), 2, {28, true}) ==
          ErrorCode::InvalidArgument);
    auto validSoft = softElements({4, 4}, {4, 4});
    CHECK(OpenError(validSoft.View(), Wire(WireType::SoftObjectReference), 2, {28, true}) ==
          ErrorCode::None);
}

TEST(ValueArray_ContradictoryShapesAreRejected) {
    // 고정 폭 wire에 다른 폭, exact인데 wire 최소보다 작은 크기는 reader·writer 모두
    // InvalidArgument다.
    SystemAllocator allocator;
    auto numbers = Header(allocator, Wire(WireType::U32), 1, 4);
    CHECK(OpenError(numbers.View(), Wire(WireType::U32), 1, {8, false}) ==
          ErrorCode::InvalidArgument);
    CHECK(OpenError(numbers.View(), Wire(WireType::U32), 1, {0, true}) ==
          ErrorCode::InvalidArgument);
    CHECK(OpenError(numbers.View(), Wire(WireType::U32), 1, {4, true}) == ErrorCode::None);
    CHECK(OpenError(numbers.View(), Wire(WireType::U32), 1, {4, false}) == ErrorCode::None);
    auto strings = Header(allocator, Wire(WireType::Bytes), 1);
    AppendU64(strings, 0);
    CHECK(OpenError(strings.View(), Wire(WireType::Bytes), 1, {4, true}) ==
          ErrorCode::InvalidArgument);
    CHECK(OpenError(strings.View(), Wire(WireType::Bytes), 1, {4, false}) == ErrorCode::None);
    ArchiveWriter out(allocator);
    CHECK(ValueArrayWriter::Begin(out, WireType::U32, 1, 64, 1, {8, false}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(ValueArrayWriter::Begin(out, WireType::Bytes, 1, 64, 1, {4, true}).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(out.ByteCount() == 0);
}

TEST(ValueArray_WriterMatchesReaderCountAndShape) {
    SystemAllocator allocator;
    // reader와 같은 maxCount: count > maxCount는 쓰기 전에 CapacityExceeded다.
    ArchiveWriter out(allocator);
    CHECK(ValueArrayWriter::Begin(out, WireType::U64, 3, 1024, 2).Error() ==
          ErrorCode::CapacityExceeded);
    CHECK(out.ByteCount() == 0);
    auto counted = ValueArrayWriter::Begin(out, WireType::U64, 2, 1024, 2);
    REQUIRE(counted && out.WriteU64(1) && out.WriteU64(2) && counted.Value().End());

    // exact shape: header + count × 최소 크기가 maxBytes를 넘으면 Begin에서, 원소 byte가 정확히
    // count × 크기가 아니면 End에서 거부한다.
    const ValueArrayShape digest{40, true};
    ArchiveWriter small(allocator);
    CHECK(ValueArrayWriter::Begin(small, WireType::Bytes, 2, 8 + 79, 10, digest).Error() ==
          ErrorCode::CapacityExceeded);
    ArchiveWriter digests(allocator);
    auto writer = ValueArrayWriter::Begin(digests, WireType::Bytes, 1, 1024, 10, digest);
    REQUIRE(writer);
    uint8 data[33] = {};
    REQUIRE(digests.WriteBytes(Span<const uint8>(data, 31)));
    CHECK(writer.Value().End().Error() == ErrorCode::InvalidArgument);
    ArchiveWriter wide(allocator);
    auto wideWriter = ValueArrayWriter::Begin(wide, WireType::Bytes, 1, 1024, 10, digest);
    REQUIRE(wideWriter && wide.WriteBytes(Span<const uint8>(data, 33)));
    CHECK(wideWriter.Value().End().Error() == ErrorCode::InvalidArgument);
    ArchiveWriter right(allocator);
    auto rightWriter = ValueArrayWriter::Begin(right, WireType::Bytes, 1, 1024, 10, digest);
    REQUIRE(rightWriter && right.WriteBytes(Span<const uint8>(data, 32)));
    CHECK(rightWriter.Value().End());
    // 쓴 byte를 같은 maxCount·shape의 reader가 연다(WireType 오버로드).
    auto archive = ArchiveReader::Create(right.View());
    REQUIRE(archive);
    auto reader = ValueArrayReader::Open(Move(archive.Value()), WireType::Bytes, 10, digest);
    REQUIRE(reader && reader.Value().Count() == 1);

    // 이동된 reader는 원소 0개의 빈 reader다(이동 생성·대입).
    ValueArrayReader moved(Move(reader.Value()));
    CHECK(reader.Value().Count() == 0 && moved.Count() == 1);
    auto otherArchive = ArchiveReader::Create(right.View());
    REQUIRE(otherArchive);
    auto other = ValueArrayReader::Open(Move(otherArchive.Value()), WireType::Bytes, 10, digest);
    REQUIRE(other);
    other.Value() = Move(moved);
    CHECK(moved.Count() == 0 && other.Value().Count() == 1);
    auto element = other.Value().Elements().ReadBytes();
    CHECK(element && element.Value().Size() == 32 && other.Value().Finish());
}
