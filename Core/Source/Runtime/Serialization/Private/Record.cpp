#include <Serialization/Record.h>
#include <Base/Endian.h>
#include <Core/Algorithms.h>

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

bool IdLess(Guid left, Guid right) noexcept {
    return left.high != right.high ? left.high < right.high : left.low < right.low;
}

// n의 2진 자릿수(0이면 0)다. 이진 탐색 비교 횟수와 heap 정렬 비용의 상한에 쓴다.
uint64 BitWidth(uint64 value) noexcept {
    uint64 bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1;
    }

    return bits;
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
    StoreLittleEndian<uint64>(header, id.high);
    StoreLittleEndian<uint64>(header + 8, id.low);
    StoreLittleEndian<uint32>(header + 16, type);
    StoreLittleEndian<uint64>(header + 20, static_cast<uint64>(encoded.Size()));
    auto appendedResult = payload_.AppendReserved(header, encoded);
    if (!appendedResult) {
        return appendedResult;
    }

    if (!ids_.TryPushBack(id)) {
        platform::Fatal("Record field reservation invariant failed");
    }

    return Result<void>::Success();
}

namespace {

// envelope header(44 byte)를 채운다.
void StoreHeader(uint8* header, Guid schema, uint32 version, uint32 fieldCount,
                 uint64 payloadBytes) noexcept {
    for (usize index = 0; index < sizeof(Magic); ++index) {
        header[index] = Magic[index];
    }

    StoreLittleEndian<uint32>(header + 8, 1);
    StoreLittleEndian<uint64>(header + 12, schema.high);
    StoreLittleEndian<uint64>(header + 20, schema.low);
    StoreLittleEndian<uint32>(header + 28, version);
    StoreLittleEndian<uint32>(header + 32, fieldCount);
    StoreLittleEndian<uint64>(header + 36, payloadBytes);
}

} // namespace

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
    StoreHeader(header, schema_, schemaVersion_, static_cast<uint32>(ids_.Size()),
                static_cast<uint64>(payload_.ByteCount()));

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

Result<RecordWriter> RecordWriter::Begin(ArchiveWriter& out, Guid schema, uint32 schemaVersion,
                                         RecordLimits limits) noexcept {
    if (IsZero(schema) || schemaVersion == 0) {
        return Result<RecordWriter>::Failure(ErrorCode::InvalidArgument);
    }

    if (limits.archive.maxTotalBytes < HeaderBytes) {
        return Result<RecordWriter>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize start = out.ByteCount();
    uint8 header[HeaderBytes];
    StoreHeader(header, schema, schemaVersion, 0, 0);
    if (auto written = out.WriteRaw(header); !written) {
        return Result<RecordWriter>::Failure(written.Error());
    }

    return Result<RecordWriter>::Success(RecordWriter(out, start, limits));
}

Result<void> RecordWriter::BeginField(Guid id) noexcept {
    if (field_ != NoField || IsZero(id)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (count_ >= limits_.maxFields) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 header[FieldHeaderBytes] = {};
    StoreLittleEndian<uint64>(header, id.high);
    StoreLittleEndian<uint64>(header + 8, id.low);
    const usize position = out_->ByteCount();
    if (auto written = out_->WriteRaw(header); !written) {
        return written;
    }

    field_ = position;
    return Result<void>::Success();
}

Result<void> RecordWriter::EndField(uint32 wireType) noexcept {
    if (field_ == NoField || wireType == 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize valueBytes = out_->ByteCount() - field_ - FieldHeaderBytes;
    if (valueBytes > limits_.archive.maxValueBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 tail[12];
    StoreLittleEndian<uint32>(tail, wireType);
    StoreLittleEndian<uint64>(tail + 4, static_cast<uint64>(valueBytes));
    if (auto patched = out_->Patch(field_ + 16, tail); !patched) {
        return patched;
    }

    field_ = NoField;
    ++count_;
    return Result<void>::Success();
}

Result<void> RecordWriter::End() noexcept {
    if (field_ != NoField) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize total = out_->ByteCount() - start_;
    if (total > limits_.archive.maxTotalBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 counts[12];
    StoreLittleEndian<uint32>(counts, count_);
    StoreLittleEndian<uint64>(counts + 4, static_cast<uint64>(total - HeaderBytes));
    return out_->Patch(start_ + 32, counts);
}

usize RecordWriter::FieldCount() const noexcept {
    return count_;
}

RecordReader::RecordReader(Allocator& allocator, ArchiveReader&& archive, uint32 version,
                           RecordLimits limits) noexcept
    : archive_(Move(archive)), fields_(allocator), byId_(allocator), schemaVersion_(version),
      limits_(limits) {
}

RecordReader::RecordReader(RecordReader&& other) noexcept
    : archive_(Move(other.archive_)), fields_(Move(other.fields_)), byId_(Move(other.byId_)),
      schemaVersion_(other.schemaVersion_), next_(other.next_), limits_(other.limits_) {
    other.schemaVersion_ = 0;
    other.next_ = 0;
}

RecordReader& RecordReader::operator=(RecordReader&& other) noexcept {
    if (this != &other) {
        archive_ = Move(other.archive_);
        fields_ = Move(other.fields_);
        byId_ = Move(other.byId_);
        schemaVersion_ = other.schemaVersion_;
        next_ = other.next_;
        limits_ = other.limits_;
        other.schemaVersion_ = 0;
        other.next_ = 0;
    }

    return *this;
}

Result<RecordReader> RecordReader::Open(Allocator& allocator, Span<const uint8> source,
                                        Guid expectedSchema, uint32 minimum, uint32 maximum,
                                        RecordLimits limits) noexcept {
    return OpenWith(allocator, source, expectedSchema, minimum, maximum, limits,
                    SharedPtr<ArchiveBudget>());
}

Result<RecordReader> RecordReader::Open(Allocator& allocator, Span<const uint8> source,
                                        Guid expectedSchema, uint32 minimum, uint32 maximum,
                                        RecordLimits limits,
                                        SharedPtr<ArchiveBudget> budget) noexcept {
    if (!budget) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    return OpenWith(allocator, source, expectedSchema, minimum, maximum, limits, Move(budget));
}

Result<RecordReader> RecordReader::OpenWith(Allocator& allocator, Span<const uint8> source,
                                            Guid expectedSchema, uint32 minimum, uint32 maximum,
                                            RecordLimits limits, SharedPtr<ArchiveBudget> shared,
                                            bool indexOnly) noexcept {
    if (IsZero(expectedSchema) || minimum == 0 || maximum < minimum) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    // 이 Record 트리의 누적 예산(값·중첩 목록·항목 reader가 공유한다).
    if (!shared) {
        auto made = SharedPtr<ArchiveBudget>::TryMake(allocator, limits.archive.maxOperations,
                                                      limits.archive.maxWork);
        if (!made) {
            return Result<RecordReader>::Failure(made.Error());
        }

        shared = Move(made.Value());
    }

    auto sourceReader = ArchiveReader::CreateShared(source, limits.archive, Move(shared));
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

    // 버전 0은 어떤 스키마에서도 쓰지 않는 잘못된 값이다(범위 밖 Unsupported와 구분한다).
    if (version.Value() == 0) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
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

    RecordReader result(allocator, Move(archive), version.Value(), limits);
    auto reserved = result.fields_.TryReserve(fieldCount.Value());
    if (!reserved) {
        return Result<RecordReader>::Failure(reserved.Error());
    }

    reserved = result.byId_.TryReserve(fieldCount.Value());
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

        Span<const uint8> payload;
        if (indexOnly) {
            // 바깥 reader가 이 범위를 이미 검사·청구했다. 경계만 확인하고 건너뛴다.
            auto charged = result.archive_.Charge(1);
            if (!charged) {
                return Result<RecordReader>::Failure(charged.Error());
            }

            const auto count = static_cast<usize>(byteCount.Value());
            if (count > result.archive_.Remaining()) {
                return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
            }

            payload = Span<const uint8>(result.archive_.source_.Data() + result.archive_.position_,
                                        count);
            result.archive_.position_ += count;
        } else {
            auto read = result.archive_.ReadRaw(static_cast<usize>(byteCount.Value()));
            if (!read) {
                return Result<RecordReader>::Failure(read.Error());
            }

            payload = read.Value();
        }

        if (!result.fields_.TryPushBack(FieldView{id.Value(), type.Value(), payload}) ||
            !result.byId_.TryPushBack(index)) {
            platform::Fatal("Record index reservation invariant failed");
        }
    }

    if (!result.archive_.AtEnd()) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    // 중복 id: id 순으로 정렬한 index에서 이웃만 비교한다(O(n log n)). heap 정렬 비교 횟수의 상한
    // 2·n·bitwidth(n)을 정렬 전에 청구한다. 같은 index는 Find의 이진 탐색에도 쓴다.
    const uint64 count = result.fields_.Size();
    spent = result.archive_.Spend(2 * count * BitWidth(count) + count);
    if (!spent) {
        return Result<RecordReader>::Failure(spent.Error());
    }

    const FieldView* fields = result.fields_.Data();
    algo::Sort(result.byId_.View(), [fields](uint32 left, uint32 right) noexcept {
        return IdLess(fields[left].id, fields[right].id);
    });
    for (usize index = 1; index < result.byId_.Size(); ++index) {
        if (fields[result.byId_[index - 1]].id == fields[result.byId_[index]].id) {
            return Result<RecordReader>::Failure(ErrorCode::AlreadyExists);
        }
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

    // 이진 탐색 비교 횟수의 상한을 먼저 청구한다(필드 수와 무관하게 log 비용).
    auto spent = archive_.Spend(BitWidth(fields_.Size()) + 1);
    if (!spent) {
        return Result<FieldView>::Failure(spent.Error());
    }

    const FieldView* fields = fields_.Data();
    const usize at = algo::LowerBound(
        Span<const uint32>(byId_.View()), id,
        [fields](uint32 index, Guid key) noexcept { return IdLess(fields[index].id, key); });
    if (at == byId_.Size() || fields[byId_[at]].id != id) {
        return Result<FieldView>::Failure(ErrorCode::NotFound);
    }

    return Result<FieldView>::Success(fields[byId_[at]]);
}

bool RecordReader::Contains(Span<const uint8> range) const noexcept {
    const auto begin = reinterpret_cast<usize>(archive_.source_.Data());
    const auto end = begin + archive_.source_.Size();
    const auto start = reinterpret_cast<usize>(range.Data());
    return archive_.source_.Size() != 0 && start >= begin && start <= end &&
           range.Size() <= end - start;
}

Result<ArchiveReader> RecordReader::OpenValue(const FieldView& field) const noexcept {
    if (!archive_.shared_) {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument); // 이동된 reader
    }

    // 예산은 공유 객체라 const reader에서도 청구한다(호출도 operation을 소비한다).
    auto charged = archive_.shared_->Charge(1);
    if (!charged) {
        return Result<ArchiveReader>::Failure(charged.Error());
    }

    if (!Contains(field.payload)) {
        return Result<ArchiveReader>::Failure(ErrorCode::InvalidArgument);
    }

    return ArchiveReader::CreateShared(field.payload, limits_.archive, archive_.shared_);
}

Result<RecordReader> RecordReader::OpenRecordField(Allocator& allocator, const FieldView& field,
                                                   Guid expectedSchema, uint32 minimum,
                                                   uint32 maximum,
                                                   uint32 maxFields) const noexcept {
    if (!archive_.shared_) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument); // 이동된 reader
    }

    auto charged = archive_.shared_->Charge(1);
    if (!charged) {
        return Result<RecordReader>::Failure(charged.Error());
    }

    if (field.wireType != static_cast<uint32>(WireType::Record)) {
        return Result<RecordReader>::Failure(ErrorCode::TypeMismatch);
    }

    if (!Contains(field.payload)) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    RecordLimits nested = limits_;
    nested.maxFields = maxFields;
    return OpenWith(allocator, field.payload, expectedSchema, minimum, maximum, nested,
                    archive_.shared_, true);
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

uint64 RecordReader::OperationsUsed() const noexcept {
    return archive_.OperationsUsed();
}

uint64 RecordReader::WorkUsed() const noexcept {
    return archive_.WorkUsed();
}

} // namespace rocks
