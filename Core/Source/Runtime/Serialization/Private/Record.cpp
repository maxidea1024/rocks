#include <Serialization/Record.h>
#include "ArchiveInternal.h"

namespace rocks {
namespace {

constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'R', 'E', 'C'};
constexpr usize HeaderBytes = 44;
constexpr usize FieldHeaderBytes = 28;

ArchiveLimits PayloadLimits(ArchiveLimits limits) noexcept {
    limits.maxTotalBytes =
        limits.maxTotalBytes < HeaderBytes ? 0 : limits.maxTotalBytes - HeaderBytes;
    return limits;
}

bool IsZero(Guid value) noexcept {
    return value == Guid{};
}

} // namespace

RecordBuilder::RecordBuilder(Allocator& allocator, Guid schema, uint32 schemaVersion,
                             RecordLimits limits) noexcept
    : allocator_(&allocator), schema_(schema), schemaVersion_(schemaVersion), limits_(limits),
      payload_(allocator, PayloadLimits(limits.archive)), ids_(allocator) {
}

Result<void> RecordBuilder::AddField(Guid id, uint32 type, Span<const uint8> encoded) noexcept {
    usize appended = 0;
    usize required = 0;
    usize work = 0;
    if (!TryAdd(FieldHeaderBytes, encoded.Size(), appended) ||
        !TryAdd(appended, payload_.ByteCount(), required) || !TryAdd(appended, 1, work) ||
        !TryAdd(work, ids_.Size(), work) ||
        (required > payload_.bytes_.Capacity() && !TryAdd(work, payload_.ByteCount(), work)) ||
        (ids_.Size() == ids_.Capacity() && !TryAdd(work, ids_.Size(), work))) {
        auto charged = payload_.Charge(1);
        return Result<void>::Failure(charged ? ErrorCode::Overflow : charged.Error());
    }
    auto charged = payload_.Charge(static_cast<uint64>(work));
    if (!charged) {
        return charged;
    }
    if (IsZero(schema_) || schemaVersion_ == 0 || IsZero(id) || type == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    if (limits_.archive.maxTotalBytes < HeaderBytes || required > payload_.limits_.maxTotalBytes ||
        encoded.Size() > limits_.archive.maxValueBytes || ids_.Size() >= limits_.maxFields) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }
    for (usize index = 0; index < ids_.Size(); ++index) {
        if (ids_[index] == id) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }
    }

    // id와 TLV 양쪽의 할당을 commit 전에 확보한다. 뒤쪽 OOM이 앞선 field 수를 늘리지 않는다.
    usize idCapacity = ids_.Capacity();
    if (idCapacity == ids_.Size()) {
        const usize growth = idCapacity / 2 + 1;
        idCapacity =
            growth > limits_.maxFields - idCapacity ? limits_.maxFields : idCapacity + growth;
    }
    auto reserved = ids_.TryReserve(idCapacity);
    if (!reserved) {
        return reserved;
    }
    uint8 header[FieldHeaderBytes];
    archive_detail::Store64(header, id.high);
    archive_detail::Store64(header + 8, id.low);
    archive_detail::Store32(header + 16, type);
    archive_detail::Store64(header + 20, static_cast<uint64>(encoded.Size()));
    auto appendedResult = payload_.AppendReserved(header, encoded);
    if (!appendedResult) {
        return appendedResult;
    }
    if (!ids_.TryPushBack(id)) {
        platform::Fatal("Record field reservation invariant failed");
    }
    return Result<void>::Success();
}

Result<Array<uint8>> RecordBuilder::Build() noexcept {
    usize total = 0;
    usize work = 0;
    if (!TryAdd(HeaderBytes, payload_.ByteCount(), total) || !TryAdd(total, 1, work)) {
        auto charged = payload_.Charge(1);
        return Result<Array<uint8>>::Failure(charged ? ErrorCode::Overflow : charged.Error());
    }
    auto charged = payload_.Charge(static_cast<uint64>(work));
    if (!charged) {
        return Result<Array<uint8>>::Failure(charged.Error());
    }
    if (IsZero(schema_) || schemaVersion_ == 0) {
        return Result<Array<uint8>>::Failure(ErrorCode::InvalidArgument);
    }
    if (total > limits_.archive.maxTotalBytes) {
        return Result<Array<uint8>>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 header[HeaderBytes];
    for (usize index = 0; index < sizeof(Magic); ++index) {
        header[index] = Magic[index];
    }
    archive_detail::Store32(header + 8, 1);
    archive_detail::Store64(header + 12, schema_.high);
    archive_detail::Store64(header + 20, schema_.low);
    archive_detail::Store32(header + 28, schemaVersion_);
    archive_detail::Store32(header + 32, static_cast<uint32>(ids_.Size()));
    archive_detail::Store64(header + 36, static_cast<uint64>(payload_.ByteCount()));

    // 별도 output을 완성한 뒤 반환한다. builder는 계속 재사용 가능하며 byte 소유권을 잃지 않는다.
    ArchiveWriter output(*allocator_, limits_.archive);
    auto written = output.AppendReserved(header, payload_.View());
    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }
    return Result<Array<uint8>>::Success(output.TakeBytes());
}

usize RecordBuilder::FieldCount() const noexcept {
    return ids_.Size();
}

uint64 RecordBuilder::WorkUsed() const noexcept {
    return payload_.WorkUsed();
}

RecordReader::RecordReader(Allocator& allocator, ArchiveReader&& archive, uint32 version) noexcept
    : archive_(Move(archive)), fields_(allocator), schemaVersion_(version) {
}

RecordReader::RecordReader(RecordReader&& other) noexcept
    : archive_(Move(other.archive_)), fields_(Move(other.fields_)),
      schemaVersion_(other.schemaVersion_), next_(other.next_) {
    other.schemaVersion_ = 0;
    other.next_ = 0;
}

RecordReader& RecordReader::operator=(RecordReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        fields_ = Move(other.fields_);
        schemaVersion_ = other.schemaVersion_;
        next_ = other.next_;
        other.schemaVersion_ = 0;
        other.next_ = 0;
    }
    return *this;
}

Result<RecordReader> RecordReader::Open(Allocator& allocator, Span<const uint8> source,
                                        Guid expectedSchema, uint32 minimum, uint32 maximum,
                                        RecordLimits limits) noexcept {
    if (IsZero(expectedSchema) || minimum == 0 || maximum < minimum) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }
    auto sourceReader = ArchiveReader::Create(source, limits.archive);
    if (!sourceReader) {
        return Result<RecordReader>::Failure(sourceReader.Error());
    }
    auto& archive = sourceReader.Value();
    // 고정 envelope prefix는 maxValueBytes와 독립적이다. 값 한도가 0인 빈 record도 허용한다.
    auto magic = archive.Fixed(sizeof(Magic));
    if (!magic) {
        return Result<RecordReader>::Failure(magic.Error());
    }
    for (usize index = 0; index < sizeof(Magic); ++index) {
        if (magic.Value()[index] != Magic[index]) {
            return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
        }
    }
    archive.position_ += sizeof(Magic);
    auto envelope = archive.ReadU32();
    if (!envelope) {
        return Result<RecordReader>::Failure(envelope.Error());
    }
    if (envelope.Value() != 1) {
        return Result<RecordReader>::Failure(ErrorCode::Unsupported);
    }
    auto schema = archive.ReadGuid();
    if (!schema) {
        return Result<RecordReader>::Failure(schema.Error());
    }
    if (schema.Value() != expectedSchema) {
        return Result<RecordReader>::Failure(ErrorCode::TypeMismatch);
    }
    auto version = archive.ReadU32();
    if (!version) {
        return Result<RecordReader>::Failure(version.Error());
    }
    if (version.Value() < minimum || version.Value() > maximum) {
        return Result<RecordReader>::Failure(ErrorCode::Unsupported);
    }
    auto fieldCount = archive.ReadU32();
    if (!fieldCount) {
        return Result<RecordReader>::Failure(fieldCount.Error());
    }
    auto payloadBytes = archive.ReadU64();
    if (!payloadBytes) {
        return Result<RecordReader>::Failure(payloadBytes.Error());
    }
    if (fieldCount.Value() > limits.maxFields) {
        return Result<RecordReader>::Failure(ErrorCode::CapacityExceeded);
    }
    if (payloadBytes.Value() != archive.Remaining() ||
        static_cast<uint64>(fieldCount.Value()) * FieldHeaderBytes > payloadBytes.Value()) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }
    // 외부 count만 믿고 큰 index를 먼저 할당하지 않는다. 최소 header bytes와 작업 한도를 확인한다.
    auto spent = archive.Spend(fieldCount.Value());
    if (!spent) {
        return Result<RecordReader>::Failure(spent.Error());
    }
    RecordReader result(allocator, Move(archive), version.Value());
    auto reserved = result.fields_.TryReserve(fieldCount.Value());
    if (!reserved) {
        return Result<RecordReader>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < fieldCount.Value(); ++index) {
        auto id = result.archive_.ReadGuid();
        if (!id) {
            return Result<RecordReader>::Failure(id.Error());
        }
        auto type = result.archive_.ReadU32();
        if (!type) {
            return Result<RecordReader>::Failure(type.Error());
        }
        auto byteCount = result.archive_.ReadU64();
        if (!byteCount) {
            return Result<RecordReader>::Failure(byteCount.Error());
        }
        if (IsZero(id.Value()) || type.Value() == 0) {
            return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
        }
        if (byteCount.Value() > limits.archive.maxValueBytes) {
            return Result<RecordReader>::Failure(ErrorCode::CapacityExceeded);
        }
        auto payload = result.archive_.ReadRaw(static_cast<usize>(byteCount.Value()));
        if (!payload) {
            return Result<RecordReader>::Failure(payload.Error());
        }

        for (usize previous = 0; previous < result.fields_.Size(); ++previous) {
            spent = result.archive_.Spend(1);
            if (!spent) {
                return Result<RecordReader>::Failure(spent.Error());
            }
            if (result.fields_[previous].id == id.Value()) {
                return Result<RecordReader>::Failure(ErrorCode::AlreadyExists);
            }
        }
        if (!result.fields_.TryPushBack(FieldView{id.Value(), type.Value(), payload.Value()})) {
            platform::Fatal("Record index reservation invariant failed");
        }
    }
    if (!result.archive_.AtEnd()) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }
    return Result<RecordReader>::Success(Move(result));
}

Result<FieldView> RecordReader::Next() noexcept {
    auto charged = archive_.Charge(1);
    if (!charged) {
        return Result<FieldView>::Failure(charged.Error());
    }
    if (next_ == fields_.Size()) {
        return Result<FieldView>::Failure(ErrorCode::NotFound);
    }
    return Result<FieldView>::Success(fields_[next_++]);
}

Result<FieldView> RecordReader::Find(Guid id) noexcept {
    auto charged = archive_.Charge(1);
    if (!charged) {
        return Result<FieldView>::Failure(charged.Error());
    }
    if (IsZero(id)) {
        return Result<FieldView>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < fields_.Size(); ++index) {
        auto spent = archive_.Spend(1);
        if (!spent) {
            return Result<FieldView>::Failure(spent.Error());
        }
        if (fields_[index].id == id) {
            return Result<FieldView>::Success(fields_[index]);
        }
    }
    return Result<FieldView>::Failure(ErrorCode::NotFound);
}

usize RecordReader::Position() const noexcept {
    return next_;
}

usize RecordReader::FieldCount() const noexcept {
    return fields_.Size();
}

uint32 RecordReader::SchemaVersion() const noexcept {
    return schemaVersion_;
}

uint64 RecordReader::WorkUsed() const noexcept {
    return archive_.WorkUsed();
}

} // namespace rocks
