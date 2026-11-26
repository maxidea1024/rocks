#include <Serialization/ValueArray.h>

#include "WireInternal.h"

namespace rocks {
namespace {

constexpr usize ValueArrayHeaderBytes =
    detail::WireSequenceHeaderBytes(1); // u32 원소 tag + u32 count

} // namespace

Result<WireValueShape> ValueArrayElementShape(uint32 elementWire) noexcept {
    return WireSlotShape(WireSlot::ArrayElement, elementWire);
}

ValueArrayReader::ValueArrayReader(ArchiveReader&& archive, uint32 count) noexcept
    : archive_(Move(archive)), count_(count) {
}

ValueArrayReader::ValueArrayReader(ValueArrayReader&& other) noexcept
    : archive_(Move(other.archive_)), count_(other.count_) {
    // 이동된 원본은 원소 0개의 빈 reader다(ArchiveReader 원본도 빈 입력이 된다).
    other.count_ = 0;
}

ValueArrayReader& ValueArrayReader::operator=(ValueArrayReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        count_ = other.count_;
        other.count_ = 0;
    }

    return *this;
}

Result<ValueArrayReader> ValueArrayReader::Open(ArchiveReader&& payload, uint32 elementWire,
                                                uint32 maxCount, WireValueShape shape) noexcept {
    const detail::WireColumn columns[] = {{WireSlot::ArrayElement, elementWire, shape}};
    auto sequence = detail::OpenWireSequence(Move(payload), columns, maxCount);
    if (!sequence) {
        return Result<ValueArrayReader>::Failure(sequence.Error());
    }

    return Result<ValueArrayReader>::Success(
        ValueArrayReader(Move(sequence.Value().archive), sequence.Value().count));
}

Result<ValueArrayReader> ValueArrayReader::OpenField(const RecordReader& parent,
                                                     const FieldView& field, uint32 elementWire,
                                                     uint32 maxCount,
                                                     WireValueShape shape) noexcept {
    auto archive = detail::WireAccess::OpenFieldValue(parent, field,
                                                      static_cast<uint32>(WireType::ValueArray));
    if (!archive) {
        return Result<ValueArrayReader>::Failure(archive.Error());
    }

    return Open(Move(archive.Value()), elementWire, maxCount, shape);
}

uint32 ValueArrayReader::Count() const noexcept {
    return count_;
}

ArchiveReader& ValueArrayReader::Elements() noexcept {
    return archive_;
}

Result<void> ValueArrayReader::Finish() const noexcept {
    if (!archive_.AtEnd()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

Result<ValueArrayWriter> ValueArrayWriter::Begin(ArchiveWriter& out, uint32 elementWire,
                                                 uint32 count, usize maxBytes, uint32 maxCount,
                                                 WireValueShape shape) noexcept {
    const usize start = out.ByteCount();
    const detail::WireColumn columns[] = {{WireSlot::ArrayElement, elementWire, shape}};
    auto entry = detail::BeginWireSequence(out, columns, count, maxBytes, maxCount);
    if (!entry) {
        return Result<ValueArrayWriter>::Failure(entry.Error());
    }

    return Result<ValueArrayWriter>::Success(
        ValueArrayWriter(out, start, count, maxBytes, entry.Value()));
}

Result<void> ValueArrayWriter::Check() const noexcept {
    return detail::CheckWireSequence(*out_, start_, maxBytes_);
}

Result<void> ValueArrayWriter::End() const noexcept {
    return detail::EndWireSequence(*out_, start_, ValueArrayHeaderBytes, count_, shape_, maxBytes_);
}

} // namespace rocks
