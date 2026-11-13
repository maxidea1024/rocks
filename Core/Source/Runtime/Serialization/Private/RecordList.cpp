#include <Serialization/RecordList.h>

#include <Base/Endian.h>

#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

constexpr uint8 Magic[] = {'R', 'O', 'C', 'K', 'S', 'R', 'E', 'C'};
constexpr usize RecordHeaderBytes = 44;
constexpr usize LengthBytes = 8;
constexpr usize CountBytes = 4;
constexpr usize MinimumItemBytes = LengthBytes + RecordHeaderBytes; // 52

// 항목 envelope: magic과 길이 일관성(44 + payloadBytes == 크기). schema·필드는 RecordReader가 본다.
bool IsRecordEnvelope(Span<const uint8> record) noexcept {
    if (record.Size() < RecordHeaderBytes) {
        return false;
    }

    for (usize index = 0; index < sizeof(Magic); ++index) {
        if (record.Data()[index] != Magic[index]) {
            return false;
        }
    }

    const uint64 payloadBytes = LoadLittleEndian<uint64>(record.Data() + 36);
    return payloadBytes == static_cast<uint64>(record.Size() - RecordHeaderBytes);
}

} // namespace

RecordListBuilder::RecordListBuilder(Allocator& allocator, RecordListLimits limits) noexcept
    : limits_(limits), items_(allocator) {
}

Result<void> RecordListBuilder::AddItem(Span<const uint8> builtRecord) noexcept {
    if (!IsRecordEnvelope(builtRecord)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // items_는 완성된 payload(u32 count + 항목들)다. 첫 항목에서 count 자리를 함께 만든다.
    const usize header = items_.IsEmpty() ? CountBytes : 0;
    const usize payload = items_.IsEmpty() ? CountBytes : items_.Size();
    if (count_ >= limits_.maxItems || builtRecord.Size() > limits_.maxItemBytes ||
        payload > limits_.maxPayloadBytes ||
        LengthBytes + builtRecord.Size() > limits_.maxPayloadBytes - payload) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 한 번에 늘린다(기하 성장 — 항목마다 전체를 다시 복사하지 않는다). 실패하면 아무것도 바뀌지
    // 않는다.
    auto appended = items_.TryAppendUninitialized(header + LengthBytes + builtRecord.Size());
    if (!appended) {
        return Result<void>::Failure(appended.Error());
    }

    uint8* output = appended.Value().Data();
    if (header != 0) {
        StoreLittleEndian<uint32>(output, 0);
        output += header;
    }

    StoreLittleEndian<uint64>(output, static_cast<uint64>(builtRecord.Size()));
    platform::MemoryCopy(output + LengthBytes, builtRecord.Data(), builtRecord.Size());
    ++count_;
    StoreLittleEndian<uint32>(items_.Data(), count_);
    return Result<void>::Success();
}

Result<void> RecordListBuilder::Finish(RecordBuilder& parent, Guid fieldId) noexcept {
    // 부모 AddField가 깊게 복사하므로 별도 사본을 만들지 않는다. 빈 목록은 count 0 하나다(그것도
    // 못 담는 한도면 reader와 같이 거부한다).
    if (items_.IsEmpty() && CountBytes > limits_.maxPayloadBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    const uint8 empty[CountBytes] = {0, 0, 0, 0};
    const Span<const uint8> payload =
        items_.IsEmpty() ? Span<const uint8>(empty, CountBytes) : items_.View();
    return parent.AddField(fieldId, WireType::RecordList, payload);
}

usize RecordListBuilder::ItemCount() const noexcept {
    return count_;
}

Result<RecordListWriter> RecordListWriter::Begin(ArchiveWriter& out, uint32 count,
                                                 RecordListLimits limits) noexcept {
    // count 자리 + 항목 최소 크기 합이 payload 한도 안이어야 한다(빈 목록도 count 4 byte).
    if (count > limits.maxItems || limits.maxPayloadBytes < CountBytes ||
        count > (limits.maxPayloadBytes - CountBytes) / MinimumItemBytes) {
        return Result<RecordListWriter>::Failure(ErrorCode::CapacityExceeded);
    }

    const usize start = out.ByteCount();
    if (auto written = out.WriteU32(count); !written) {
        return Result<RecordListWriter>::Failure(written.Error());
    }

    return Result<RecordListWriter>::Success(RecordListWriter(out, start, count, limits));
}

Result<void> RecordListWriter::BeginItem() noexcept {
    if (item_ != NoItem || written_ >= count_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize position = out_->ByteCount();
    if (auto written = out_->WriteU64(0); !written) {
        return written;
    }

    item_ = position;
    return Result<void>::Success();
}

Result<void> RecordListWriter::EndItem() noexcept {
    // 열린 항목이 없거나 out이 길이 자리보다 짧으면(호출자가 out을 바꿨다) 버퍼를 읽지 않는다.
    if (item_ == NoItem || out_->ByteCount() < item_ + LengthBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize itemBytes = out_->ByteCount() - item_ - LengthBytes;
    if (itemBytes > limits_.maxItemBytes || out_->ByteCount() - start_ > limits_.maxPayloadBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 호출자가 쓴 항목이 Record 하나인지 확인한다(reader와 같은 envelope 검사).
    const Span<const uint8> item(out_->View().Data() + item_ + LengthBytes, itemBytes);
    if (!IsRecordEnvelope(item)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint8 length[LengthBytes];
    StoreLittleEndian<uint64>(length, static_cast<uint64>(itemBytes));
    if (auto patched = out_->Patch(item_, length); !patched) {
        return patched;
    }

    item_ = NoItem;
    ++written_;
    return Result<void>::Success();
}

Result<void> RecordListWriter::End() const noexcept {
    if (item_ != NoItem || written_ != count_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (out_->ByteCount() - start_ > limits_.maxPayloadBytes) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

usize RecordListWriter::ItemCount() const noexcept {
    return written_;
}

RecordListReader::RecordListReader(Allocator& allocator) noexcept
    : allocator_(&allocator), items_(allocator) {
}

Result<RecordListReader> RecordListReader::Open(Allocator& allocator, Span<const uint8> payload,
                                                Guid itemSchema, uint32 minimum, uint32 maximum,
                                                RecordLimits itemLimits,
                                                RecordListLimits listLimits) noexcept {
    if (itemSchema == Guid{} || minimum == 0 || maximum < minimum) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
    }

    // 최상위 목록: 새 예산을 만든다(목록과 모든 항목 reader가 공유한다).
    auto budget = SharedPtr<ArchiveBudget>::TryMake(allocator, itemLimits.archive.maxOperations,
                                                    itemLimits.archive.maxWork);
    if (!budget) {
        return Result<RecordListReader>::Failure(budget.Error());
    }

    return OpenShared(allocator, payload, itemSchema, minimum, maximum, itemLimits, listLimits,
                      Move(budget.Value()), false);
}

Result<RecordListReader> RecordListReader::OpenField(const RecordReader& parent,
                                                     const FieldView& field, Guid itemSchema,
                                                     uint32 minimum, uint32 maximum,
                                                     RecordListLimits listLimits) noexcept {
    if (!parent.archive_.shared_) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument); // 이동된 parent
    }

    // 여는 호출도 operation을 소비한다(부모 트리의 예산).
    if (auto charged = parent.archive_.shared_->Charge(1); !charged) {
        return Result<RecordListReader>::Failure(charged.Error());
    }

    if (field.wireType != static_cast<uint32>(WireType::RecordList)) {
        return Result<RecordListReader>::Failure(ErrorCode::TypeMismatch);
    }

    if (!parent.Contains(field.payload)) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
    }

    if (itemSchema == Guid{} || minimum == 0 || maximum < minimum) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
    }

    // 부모 Record가 이 payload byte를 이미 청구했으므로 색인만 청구한다(OpenRecordField와 같다).
    return OpenShared(parent.fields_.GetAllocator(), field.payload, itemSchema, minimum, maximum,
                      parent.limits_, listLimits, parent.archive_.shared_, true);
}

Result<RecordListReader>
RecordListReader::OpenShared(Allocator& allocator, Span<const uint8> payload, Guid itemSchema,
                             uint32 minimum, uint32 maximum, RecordLimits itemLimits,
                             RecordListLimits listLimits, SharedPtr<ArchiveBudget> budget,
                             bool indexOnly) noexcept {
    RecordListReader result(allocator);
    result.schema_ = itemSchema;
    result.minimum_ = minimum;
    result.maximum_ = maximum;
    result.limits_ = itemLimits;
    result.budget_ = budget;
    if (payload.Size() > listLimits.maxPayloadBytes) {
        return Result<RecordListReader>::Failure(ErrorCode::CapacityExceeded);
    }

    // 목록 자체의 크기 한도는 목록 한도에서 온다(항목 Record 한도와 분리). operation·work는 공유
    // 예산이 판정한다.
    ArchiveLimits listArchive = itemLimits.archive;
    listArchive.maxTotalBytes = listLimits.maxPayloadBytes;
    listArchive.maxValueBytes = listLimits.maxItemBytes;
    auto created = ArchiveReader::CreateShared(payload, listArchive, Move(budget));
    if (!created) {
        return Result<RecordListReader>::Failure(created.Error());
    }

    ArchiveReader& archive = created.Value();
    auto count = archive.ReadU32();
    if (!count) {
        return Result<RecordListReader>::Failure(count.Error());
    }

    if (count.Value() > listLimits.maxItems) {
        return Result<RecordListReader>::Failure(ErrorCode::CapacityExceeded);
    }

    // 외부 count만 믿고 큰 index를 먼저 할당하지 않는다: 남은 byte가 항목 최소 크기만큼 있어야
    // 한다.
    if (count.Value() > archive.Remaining() / MinimumItemBytes) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto spent = archive.Spend(count.Value()); !spent) {
        return Result<RecordListReader>::Failure(spent.Error());
    }

    if (auto reserved = result.items_.TryReserve(count.Value()); !reserved) {
        return Result<RecordListReader>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        auto length = archive.ReadU64();
        if (!length) {
            return Result<RecordListReader>::Failure(length.Error());
        }

        if (length.Value() > listLimits.maxItemBytes) {
            return Result<RecordListReader>::Failure(ErrorCode::CapacityExceeded);
        }

        if (length.Value() < RecordHeaderBytes || length.Value() > archive.Remaining()) {
            return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
        }

        Span<const uint8> item;
        if (indexOnly) {
            // 부모가 이 범위를 이미 청구했다. operation 하나만 청구하고 경계만 확인해 건너뛴다.
            if (auto charged = archive.Charge(1); !charged) {
                return Result<RecordListReader>::Failure(charged.Error());
            }

            const auto count = static_cast<usize>(length.Value());
            item = Span<const uint8>(archive.source_.Data() + archive.position_, count);
            archive.position_ += count;
        } else {
            auto read = archive.ReadRaw(static_cast<usize>(length.Value()));
            if (!read) {
                return Result<RecordListReader>::Failure(read.Error());
            }

            item = read.Value();
        }

        if (!IsRecordEnvelope(item)) {
            return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
        }

        if (!result.items_.TryPushBack(item)) {
            platform::Fatal("RecordListReader index reservation invariant failed");
        }
    }

    if (!archive.AtEnd()) {
        return Result<RecordListReader>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<RecordListReader>::Success(Move(result));
}

usize RecordListReader::ItemCount() const noexcept {
    return items_.Size();
}

Result<RecordReader> RecordListReader::OpenItem(usize index) noexcept {
    return OpenItem(index, limits_.maxFields);
}

Result<RecordReader> RecordListReader::OpenItem(usize index, uint32 maxFields) noexcept {
    if (!budget_) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument); // 이동된 reader
    }

    // 실패한 호출도 operation을 소비한다(범위 밖 index를 반복해도 공짜가 아니다).
    if (auto charged = budget_->Charge(1); !charged) {
        return Result<RecordReader>::Failure(charged.Error());
    }

    if (index >= items_.Size()) {
        return Result<RecordReader>::Failure(ErrorCode::InvalidArgument);
    }

    // 항목 byte는 목록을 열 때(단독 Open) 또는 부모 Record가(OpenField) 이미 청구했다. 항목
    // Record는 header·TLV 색인만 청구한다.
    RecordLimits limits = limits_;
    limits.maxFields = maxFields;
    return RecordReader::OpenWith(*allocator_, items_[index], schema_, minimum_, maximum_, limits,
                                  budget_, true);
}

Result<Span<const uint8>> RecordListReader::ItemBytes(usize index) const noexcept {
    if (index >= items_.Size()) {
        return Result<Span<const uint8>>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<Span<const uint8>>::Success(items_[index]);
}

uint64 RecordListReader::OperationsUsed() const noexcept {
    return budget_ ? budget_->OperationsUsed() : 0;
}

uint64 RecordListReader::WorkUsed() const noexcept {
    return budget_ ? budget_->WorkUsed() : 0;
}

} // namespace rocks
