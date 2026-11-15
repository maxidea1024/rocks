#include <Serialization/MapWire.h>

#include "WireInternal.h"

namespace rocks {
namespace {

constexpr usize HeaderBytes = detail::WireSequenceHeaderBytes(2); // 키 tag + 값 tag + count

} // namespace

MapWireReader::MapWireReader(ArchiveReader&& archive, uint32 count) noexcept
    : archive_(Move(archive)), count_(count) {
}

MapWireReader::MapWireReader(MapWireReader&& other) noexcept
    : archive_(Move(other.archive_)), count_(other.count_) {
    other.count_ = 0;
}

MapWireReader& MapWireReader::operator=(MapWireReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        count_ = other.count_;
        other.count_ = 0;
    }

    return *this;
}

Result<MapWireReader> MapWireReader::Open(ArchiveReader&& payload, uint32 keyWire, uint32 valueWire,
                                          uint32 maxCount, WireValueShape keyShape,
                                          WireValueShape valueShape) noexcept {
    const detail::WireColumn columns[] = {{WireSlot::Key, keyWire, keyShape},
                                          {WireSlot::MapValue, valueWire, valueShape}};
    auto sequence = detail::OpenWireSequence(Move(payload), columns, maxCount);
    if (!sequence) {
        return Result<MapWireReader>::Failure(sequence.Error());
    }

    return Result<MapWireReader>::Success(
        MapWireReader(Move(sequence.Value().archive), sequence.Value().count));
}

Result<MapWireReader> MapWireReader::OpenField(const RecordReader& parent, const FieldView& field,
                                               uint32 keyWire, uint32 valueWire, uint32 maxCount,
                                               WireValueShape keyShape,
                                               WireValueShape valueShape) noexcept {
    auto archive =
        detail::WireAccess::OpenFieldValue(parent, field, static_cast<uint32>(WireType::Map));
    if (!archive) {
        return Result<MapWireReader>::Failure(archive.Error());
    }

    return Open(Move(archive.Value()), keyWire, valueWire, maxCount, keyShape, valueShape);
}

uint32 MapWireReader::Count() const noexcept {
    return count_;
}

ArchiveReader& MapWireReader::Entries() noexcept {
    return archive_;
}

Result<void> MapWireReader::Finish() const noexcept {
    if (!archive_.AtEnd()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<MapWireWriter> MapWireWriter::Begin(ArchiveWriter& out, uint32 keyWire, uint32 valueWire,
                                           uint32 count, usize maxBytes, uint32 maxCount,
                                           WireValueShape keyShape,
                                           WireValueShape valueShape) noexcept {
    const usize start = out.ByteCount();
    const detail::WireColumn columns[] = {{WireSlot::Key, keyWire, keyShape},
                                          {WireSlot::MapValue, valueWire, valueShape}};
    auto entry = detail::BeginWireSequence(out, columns, count, maxBytes, maxCount);
    if (!entry) {
        return Result<MapWireWriter>::Failure(entry.Error());
    }

    return Result<MapWireWriter>::Success(
        MapWireWriter(out, start, count, maxBytes, entry.Value()));
}

Result<void> MapWireWriter::Check() const noexcept {
    return detail::CheckWireSequence(*out_, start_, maxBytes_);
}

Result<void> MapWireWriter::End() const noexcept {
    return detail::EndWireSequence(*out_, start_, HeaderBytes, count_, entry_, maxBytes_);
}

} // namespace rocks
