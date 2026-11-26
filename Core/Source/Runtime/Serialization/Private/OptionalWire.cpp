#include <Serialization/OptionalWire.h>

#include "WireInternal.h"

namespace rocks {
namespace {

constexpr uint32 RecordWire = static_cast<uint32>(WireType::Record);

// 단일 값 내부 payload(value — 남은 byte 전부)의 크기가 resolved와 맞는지 본다.
bool InnerSizeMatches(uint32 wire, WireValueShape resolved, Span<const uint8> value) noexcept {
    if (value.Size() < resolved.minElementBytes ||
        (resolved.exact && value.Size() != resolved.minElementBytes)) {
        return false;
    }

    return detail::LengthPrefixMatches(wire, resolved, value.Data());
}

} // namespace

OptionalWireReader::OptionalWireReader(ArchiveReader&& archive, bool set,
                                       Span<const uint8> record) noexcept
    : archive_(Move(archive)), record_(record), set_(set) {
}

OptionalWireReader::OptionalWireReader(OptionalWireReader&& other) noexcept
    : archive_(Move(other.archive_)), record_(other.record_), set_(other.set_) {
    // 이동된 원본은 unset의 빈 reader다(ArchiveReader 원본도 빈 입력이 된다).
    other.record_ = Span<const uint8>();
    other.set_ = false;
}

OptionalWireReader& OptionalWireReader::operator=(OptionalWireReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        record_ = other.record_;
        set_ = other.set_;
        other.record_ = Span<const uint8>();
        other.set_ = false;
    }

    return *this;
}

Result<OptionalWireReader> OptionalWireReader::Open(ArchiveReader&& payload, uint32 innerWire,
                                                    WireValueShape shape) noexcept {
    auto resolved = detail::ResolveSlotShape(WireSlot::OptionalInner, innerWire, shape);
    if (!resolved) {
        return Result<OptionalWireReader>::Failure(resolved.Error());
    }

    ArchiveReader archive = Move(payload);
    auto present = archive.ReadBool(); // 0·1 밖은 CorruptData
    if (!present) {
        return Result<OptionalWireReader>::Failure(present.Error());
    }

    if (!present.Value()) {
        if (!archive.AtEnd()) {
            return Result<OptionalWireReader>::Failure(ErrorCode::CorruptData);
        }

        return Result<OptionalWireReader>::Success(
            OptionalWireReader(Move(archive), false, Span<const uint8>()));
    }

    auto stored = archive.ReadU32();
    if (!stored) {
        return Result<OptionalWireReader>::Failure(stored.Error());
    }

    if (!WireSlotShape(WireSlot::OptionalInner, stored.Value())) {
        return Result<OptionalWireReader>::Failure(ErrorCode::CorruptData);
    }

    if (stored.Value() != innerWire) {
        return Result<OptionalWireReader>::Failure(ErrorCode::TypeMismatch);
    }

    const Span<const uint8> inner = detail::WireAccess::Rest(archive);
    if (innerWire == RecordWire) {
        // 남은 byte 전부가 Record 하나여야 한다. 검사한 뒤 내용으로 넘긴다(Record 트리 안이면
        // byte를 다시 청구하지 않는다).
        if (auto header = ReadRecordHeader(inner); !header) {
            return Result<OptionalWireReader>::Failure(header.Error());
        }

        auto content = detail::WireAccess::SkipContent(archive, inner.Size());
        if (!content) {
            return Result<OptionalWireReader>::Failure(content.Error());
        }

        return Result<OptionalWireReader>::Success(
            OptionalWireReader(Move(archive), true, content.Value()));
    }

    if (!InnerSizeMatches(innerWire, resolved.Value(), inner)) {
        return Result<OptionalWireReader>::Failure(ErrorCode::CorruptData);
    }

    return Result<OptionalWireReader>::Success(
        OptionalWireReader(Move(archive), true, Span<const uint8>()));
}

Result<OptionalWireReader> OptionalWireReader::OpenField(const RecordReader& parent,
                                                         const FieldView& field, uint32 innerWire,
                                                         WireValueShape shape) noexcept {
    auto archive =
        detail::WireAccess::OpenFieldValue(parent, field, static_cast<uint32>(WireType::Optional));
    if (!archive) {
        return Result<OptionalWireReader>::Failure(archive.Error());
    }

    return Open(Move(archive.Value()), innerWire, shape);
}

bool OptionalWireReader::IsSet() const noexcept {
    return set_;
}

ArchiveReader& OptionalWireReader::Inner() noexcept {
    return archive_;
}

Span<const uint8> OptionalWireReader::RecordBytes() const noexcept {
    return record_;
}

Result<void> OptionalWireReader::Finish() const noexcept {
    if (!archive_.AtEnd()) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    return Result<void>::Success();
}

Result<void> OptionalWireWriter::WriteUnset(ArchiveWriter& out) noexcept {
    return out.WriteBool(false);
}

Result<OptionalWireWriter> OptionalWireWriter::BeginSet(ArchiveWriter& out, uint32 innerWire,
                                                        WireValueShape shape) noexcept {
    auto resolved = detail::ResolveSlotShape(WireSlot::OptionalInner, innerWire, shape);
    if (!resolved) {
        return Result<OptionalWireWriter>::Failure(resolved.Error());
    }

    if (auto written = out.WriteBool(true); !written) {
        return Result<OptionalWireWriter>::Failure(written.Error());
    }

    if (auto written = out.WriteU32(innerWire); !written) {
        return Result<OptionalWireWriter>::Failure(written.Error());
    }

    return Result<OptionalWireWriter>::Success(
        OptionalWireWriter(out, out.ByteCount(), innerWire, resolved.Value()));
}

Result<void> OptionalWireWriter::End() const noexcept {
    if (out_->ByteCount() < inner_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument); // 호출자가 out을 줄였다
    }

    const Span<const uint8> inner(out_->View().Data() + inner_, out_->ByteCount() - inner_);
    if (wire_ == RecordWire) {
        if (!ReadRecordHeader(inner)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return Result<void>::Success();
    }

    if (!InnerSizeMatches(wire_, shape_, inner)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

} // namespace rocks
