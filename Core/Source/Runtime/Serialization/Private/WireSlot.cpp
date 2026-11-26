#include "WireInternal.h"

#include <Base/Endian.h>

// 자리 표(WireSlotShape)·shape 해석·길이 prefix Record·WireAccess 구현이다. 원소 열은
// WireSequence.cpp다.
namespace rocks {
namespace {

using detail::LengthPrefixBytes;
using detail::RecordHeaderBytes;

constexpr uint32 Wire(WireType type) noexcept {
    return static_cast<uint32>(type);
}

// 자리 비트(WireSlot마다 하나)다.
constexpr uint8 SlotBit(WireSlot slot) noexcept {
    return static_cast<uint8>(1u << static_cast<uint8>(slot));
}

constexpr uint8 Scalar = SlotBit(WireSlot::ArrayElement) | SlotBit(WireSlot::Key) |
                         SlotBit(WireSlot::MapValue) | SlotBit(WireSlot::OptionalInner);
constexpr uint8 NonKey = SlotBit(WireSlot::ArrayElement) | SlotBit(WireSlot::MapValue) |
                         SlotBit(WireSlot::OptionalInner);
constexpr uint8 Element = SlotBit(WireSlot::ArrayElement) | SlotBit(WireSlot::MapValue);
constexpr uint8 Reference = Element | SlotBit(WireSlot::Key);

} // namespace

Result<WireValueShape> WireSlotShape(WireSlot slot, uint32 wire) noexcept {
    // 열거자 밖의 자리는 받는 tag가 없다(아래 비트 이동의 범위도 지킨다).
    if (static_cast<uint8>(slot) > static_cast<uint8>(WireSlot::OptionalInner)) {
        return Result<WireValueShape>::Failure(ErrorCode::InvalidArgument);
    }

    WireValueShape shape;
    uint8 slots = 0;
    switch (static_cast<WireType>(wire)) {
    case WireType::Bool:
        shape = {1, true};
        slots = Scalar;
        break;
    case WireType::U32:
        shape = {4, true};
        slots = Scalar;
        break;
    case WireType::F32:
        shape = {4, true};
        slots = NonKey;
        break;
    case WireType::U64:
    case WireType::I64:
        shape = {8, true};
        slots = Scalar;
        break;
    case WireType::F64:
        shape = {8, true};
        slots = NonKey;
        break;
    case WireType::Guid:
        shape = {16, true};
        slots = Scalar;
        break;
    case WireType::String:
    case WireType::Bytes:
        shape = {LengthPrefixBytes, false};
        slots = Scalar;
        break;
    case WireType::SoftObjectReference:
        shape = {16 + LengthPrefixBytes, false};
        slots = Reference;
        break;
    case WireType::InstancedStruct:
        shape = {LengthPrefixBytes, false};
        slots = Element;
        break;
    case WireType::Text:
        shape = {1, false};
        slots = Element;
        break;
    case WireType::Record:
        // Map 값은 u64 길이 + Record, Optional 내부는 길이 prefix 없이 Record다.
        if (slot == WireSlot::MapValue) {
            return Result<WireValueShape>::Success({LengthPrefixBytes + RecordHeaderBytes, false});
        }

        if (slot == WireSlot::OptionalInner) {
            return Result<WireValueShape>::Success({RecordHeaderBytes, false});
        }

        break;
    case WireType::RecordList:
    case WireType::ValueArray:
    case WireType::Optional:
    case WireType::Set:
    case WireType::Map:
        break;
    }

    if ((slots & SlotBit(slot)) == 0) {
        return Result<WireValueShape>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<WireValueShape>::Success(shape);
}

namespace detail {

bool LengthPrefix(uint32 wire, usize& offset, usize& fixedBytes) noexcept {
    switch (static_cast<WireType>(wire)) {
    case WireType::String:
    case WireType::Bytes:
        offset = 0;
        fixedBytes = LengthPrefixBytes;
        return true;
    case WireType::SoftObjectReference:
        offset = 16;
        fixedBytes = 16 + LengthPrefixBytes;
        return true;
    default:
        return false;
    }
}

Result<WireValueShape> ResolveSlotShape(WireSlot slot, uint32 wire, WireValueShape shape) noexcept {
    auto resolved = WireSlotShape(slot, wire);
    if (!resolved) {
        return resolved;
    }

    const WireValueShape element = resolved.Value();
    usize offset = 0;
    usize fixedBytes = 0;
    // 고정 폭에 다른 폭, exact인데 wire 최소보다 작은 크기, 길이 prefix 하나로 크기가 정해지지 않는
    // 가변 wire(Record·InstancedStruct·Text)의 exact는 모순이다.
    if ((element.exact && shape.minElementBytes != 0 &&
         shape.minElementBytes != element.minElementBytes) ||
        (shape.exact && shape.minElementBytes < element.minElementBytes) ||
        (shape.exact && !element.exact && !LengthPrefix(wire, offset, fixedBytes))) {
        return Result<WireValueShape>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<WireValueShape>::Success({shape.minElementBytes > element.minElementBytes
                                                ? shape.minElementBytes
                                                : element.minElementBytes,
                                            element.exact || shape.exact});
}

bool LengthPrefixMatches(uint32 wire, WireValueShape resolved, const uint8* value) noexcept {
    usize offset = 0;
    usize fixedBytes = 0;
    if (!resolved.exact || !LengthPrefix(wire, offset, fixedBytes)) {
        return true;
    }

    return LoadLittleEndian<uint64>(value + offset) ==
           static_cast<uint64>(resolved.minElementBytes - fixedBytes);
}

Result<Span<const uint8>> WireAccess::SkipContent(ArchiveReader& reader, usize count) noexcept {
    if (auto charged = reader.Charge(1); !charged) {
        return Result<Span<const uint8>>::Failure(charged.Error());
    }

    if (count > reader.limits_.maxValueBytes) {
        return Result<Span<const uint8>>::Failure(ErrorCode::CapacityExceeded);
    }

    if (count > reader.Remaining()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::CorruptData);
    }

    // Record 트리 안의 byte는 트리의 Open이 청구했다. 단독 reader만 내용 byte를 청구한다.
    if (!reader.shared_) {
        if (auto spent = reader.Spend(static_cast<uint64>(count)); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }
    }

    const Span<const uint8> content(reader.source_.Data() + reader.position_, count);
    reader.position_ += count;
    return Result<Span<const uint8>>::Success(content);
}

Result<Span<const uint8>> WireAccess::ReadPrefixedContent(ArchiveReader& reader) noexcept {
    auto prefix = reader.Fixed(LengthPrefixBytes);
    if (!prefix) {
        return Result<Span<const uint8>>::Failure(prefix.Error());
    }

    const uint64 length = LoadLittleEndian<uint64>(prefix.Value().Data());
    if (length > reader.limits_.maxValueBytes) {
        return Result<Span<const uint8>>::Failure(ErrorCode::CapacityExceeded);
    }

    if (length > reader.Remaining() - LengthPrefixBytes) {
        return Result<Span<const uint8>>::Failure(ErrorCode::CorruptData);
    }

    if (!reader.shared_) {
        if (auto spent = reader.Spend(length); !spent) {
            return Result<Span<const uint8>>::Failure(spent.Error());
        }
    }

    const auto count = static_cast<usize>(length);
    const Span<const uint8> content(reader.source_.Data() + reader.position_ + LengthPrefixBytes,
                                    count);
    reader.position_ += LengthPrefixBytes + count;
    return Result<Span<const uint8>>::Success(content);
}

Result<ArchiveReader> WireAccess::OpenFieldValue(const RecordReader& parent, const FieldView& field,
                                                 uint32 wire) noexcept {
    if (!parent.archive_.shared_) {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument); // 이동된 parent
    }

    // 여는 호출도 operation을 소비한다(부모 트리의 예산).
    if (auto charged = parent.archive_.shared_->Charge(1); !charged) {
        return Result<ArchiveReader>::Failure(charged.Error());
    }

    if (field.wireType != wire) {
        return Result<ArchiveReader>::Failure(ErrorCode::TypeMismatch);
    }

    if (!parent.Contains(field.payload)) {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
    }

    return ArchiveReader::CreateShared(field.payload, parent.limits_.archive,
                                       parent.archive_.shared_);
}

} // namespace detail

Result<PrefixedRecordWriter> PrefixedRecordWriter::Begin(ArchiveWriter& out, uint32 wire) noexcept {
    if (wire != Wire(WireType::Record) && wire != Wire(WireType::InstancedStruct)) {
        return Result<PrefixedRecordWriter>::Failure(ErrorCode::InvalidArgument);
    }

    const usize start = out.ByteCount();
    if (auto written = out.WriteU64(0); !written) {
        return Result<PrefixedRecordWriter>::Failure(written.Error());
    }

    return Result<PrefixedRecordWriter>::Success(PrefixedRecordWriter(out, start, wire));
}

Result<void> PrefixedRecordWriter::End() noexcept {
    // 이미 끝냈거나 out이 길이 자리보다 짧으면(호출자가 out을 바꿨다) 버퍼를 읽지 않는다.
    if (ended_ || out_->ByteCount() < start_ + LengthPrefixBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize contentBytes = out_->ByteCount() - start_ - LengthPrefixBytes;
    const Span<const uint8> content(out_->View().Data() + start_ + LengthPrefixBytes, contentBytes);
    const bool empty = contentBytes == 0 && wire_ == Wire(WireType::InstancedStruct);
    if (!empty && !ReadRecordHeader(content)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 length[LengthPrefixBytes];
    StoreLittleEndian<uint64>(length, static_cast<uint64>(contentBytes));
    if (auto patched = detail::WireAccess::Patch(*out_, start_, length); !patched) {
        return patched;
    }

    ended_ = true;
    return Result<void>::Success();
}

Result<Span<const uint8>> ReadPrefixedRecord(ArchiveReader& entries) noexcept {
    const usize start = entries.Position();
    auto content = detail::WireAccess::ReadPrefixedContent(entries);
    if (!content) {
        return content;
    }

    if (auto header = ReadRecordHeader(content.Value()); !header) {
        detail::WireAccess::Rewind(entries, start);
        return Result<Span<const uint8>>::Failure(header.Error());
    }

    return content;
}

} // namespace rocks
