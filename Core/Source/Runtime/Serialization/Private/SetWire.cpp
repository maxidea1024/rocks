#include <Serialization/SetWire.h>

#include "WireInternal.h"

namespace rocks {
namespace {

constexpr usize SetWireHeaderBytes = detail::WireSequenceHeaderBytes(1); // u32 원소 tag + u32 count

} // namespace

SetWireReader::SetWireReader(ArchiveReader&& archive, uint32 count) noexcept
    : archive_(Move(archive)), count_(count) {
}

SetWireReader::SetWireReader(SetWireReader&& other) noexcept
    : archive_(Move(other.archive_)), count_(other.count_) {
    other.count_ = 0;
}

SetWireReader& SetWireReader::operator=(SetWireReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        count_ = other.count_;
        other.count_ = 0;
    }

    return *this;
}

Result<SetWireReader> SetWireReader::Open(ArchiveReader&& payload, uint32 elementWire,
                                          uint32 maxCount, WireValueShape shape) noexcept {
    const detail::WireColumn columns[] = {{WireSlot::Key, elementWire, shape}};
    auto sequence = detail::OpenWireSequence(Move(payload), columns, maxCount);
    if (!sequence) {
        return Result<SetWireReader>::Failure(sequence.Error());
    }

    return Result<SetWireReader>::Success(
        SetWireReader(Move(sequence.Value().archive), sequence.Value().count));
}

Result<SetWireReader> SetWireReader::OpenField(const RecordReader& parent, const FieldView& field,
                                               uint32 elementWire, uint32 maxCount,
                                               WireValueShape shape) noexcept {
    auto archive =
        detail::WireAccess::OpenFieldValue(parent, field, static_cast<uint32>(WireType::Set));
    if (!archive) {
        return Result<SetWireReader>::Failure(archive.Error());
    }

    return Open(Move(archive.Value()), elementWire, maxCount, shape);
}

uint32 SetWireReader::Count() const noexcept {
    return count_;
}

ArchiveReader& SetWireReader::Elements() noexcept {
    return archive_;
}

Result<void> SetWireReader::Finish() const noexcept {
    if (!archive_.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<SetWireWriter> SetWireWriter::Begin(ArchiveWriter& out, uint32 elementWire, uint32 count,
                                           usize maxBytes, uint32 maxCount,
                                           WireValueShape shape) noexcept {
    const usize start = out.ByteCount();
    const detail::WireColumn columns[] = {{WireSlot::Key, elementWire, shape}};
    auto entry = detail::BeginWireSequence(out, columns, count, maxBytes, maxCount);
    if (!entry) {
        return Result<SetWireWriter>::Failure(entry.Error());
    }

    return Result<SetWireWriter>::Success(
        SetWireWriter(out, start, count, maxBytes, entry.Value()));
}

Result<void> SetWireWriter::Check() const noexcept {
    return detail::CheckWireSequence(*out_, start_, maxBytes_);
}

Result<void> SetWireWriter::End() const noexcept {
    return detail::EndWireSequence(*out_, start_, SetWireHeaderBytes, count_, shape_, maxBytes_);
}

} // namespace rocks
