#include "NuggetInternal.h"

#include <AssetNugget/NuggetRecord.h>

#include <Base/Endian.h>
#include <Serialization/Record.h>
#include <Serialization/RecordList.h>

// 에셋 너겟 Record다(Docs/Design/AssetSystem.ko.md 9.1절 결정 5 — NuggetRecord.h).
namespace rocks {

namespace {

constexpr usize U32Bytes = 4;
constexpr usize U64Bytes = 8;
constexpr usize DigestBytes = Sha256Digest::Bytes;
// 행 안 칸 위치다(Rockpile 5.2절 — ValueId u32, encoding u8, reserved u8×3, 크기 셋, 해시 넷).
constexpr usize RowEncodingAt = 4;
constexpr usize RowReservedAt = 5;
constexpr usize RowReservedBytes = 3;
constexpr usize RowRawSizeAt = 8;
constexpr usize RowStoredSizeAt = 16;
constexpr usize RowIndexLengthAt = 24;
constexpr usize RowRawHashAt = 32;
constexpr usize RowContentHashAt = 64;
constexpr usize RowIndexHashAt = 96;
constexpr usize RowChunkTableHashAt = 128;
static_assert(RowChunkTableHashAt + DigestBytes == NuggetValueRowBytes,
              "nugget value row layout must be 160 bytes");

// 필드 수(1–6)·소비 입력 항목 필드 수(1–3)다.
constexpr uint32 RecordFieldCount = 6;
constexpr uint32 InputFieldCount = 3;

bool IsZero(const Sha256Digest& digest) noexcept {
    return digest == Sha256Digest{};
}

Span<const uint8> BytesOf(StringView text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount());
}

bool IsLower(char c) noexcept {
    return c >= 'a' && c <= 'z';
}

bool IsDigit(char c) noexcept {
    return c >= '0' && c <= '9';
}

void EncodeRow(const NuggetValueRow& row, uint8 (&out)[NuggetValueRowBytes]) noexcept {
    for (uint8& byte : out) {
        byte = 0;
    }

    StoreLittleEndian<uint32>(out, row.id);
    out[RowEncodingAt] = static_cast<uint8>(row.encoding);
    StoreLittleEndian<uint64>(out + RowRawSizeAt, row.rawSize);
    StoreLittleEndian<uint64>(out + RowStoredSizeAt, row.storedSize);
    StoreLittleEndian<uint64>(out + RowIndexLengthAt, row.indexLength);
    const Sha256Digest* hashes[] = {&row.rawHash, &row.contentHash, &row.indexHash,
                                    &row.chunkTableHash};
    const usize offsets[] = {RowRawHashAt, RowContentHashAt, RowIndexHashAt, RowChunkTableHashAt};
    for (usize index = 0; index < 4; ++index) {
        for (usize at = 0; at < DigestBytes; ++at) {
            out[offsets[index] + at] = hashes[index]->bytes[at];
        }
    }
}

// 행 하나를 읽는다. reserved가 0이 아니면 false다(표현 밖 비트를 두지 않는다).
bool DecodeRow(Span<const uint8> bytes, NuggetValueRow& row) noexcept {
    for (usize at = 0; at < RowReservedBytes; ++at) {
        if (bytes[RowReservedAt + at] != 0) {
            return false;
        }
    }

    row.id = LoadLittleEndian<uint32>(bytes.Data());
    row.encoding = static_cast<NuggetEncoding>(bytes[RowEncodingAt]);
    row.rawSize = LoadLittleEndian<uint64>(bytes.Data() + RowRawSizeAt);
    row.storedSize = LoadLittleEndian<uint64>(bytes.Data() + RowStoredSizeAt);
    row.indexLength = LoadLittleEndian<uint64>(bytes.Data() + RowIndexLengthAt);
    Sha256Digest* hashes[] = {&row.rawHash, &row.contentHash, &row.indexHash, &row.chunkTableHash};
    const usize offsets[] = {RowRawHashAt, RowContentHashAt, RowIndexHashAt, RowChunkTableHashAt};
    for (usize index = 0; index < 4; ++index) {
        for (usize at = 0; at < DigestBytes; ++at) {
            hashes[index]->bytes[at] = bytes[offsets[index] + at];
        }
    }

    return true;
}

// RCBK 헤더 크기와 블록 표 항목 크기다(Compression 9.1절 — indexLength = 48 + 8n).
constexpr uint64 RcbkHeaderBytes = 48;
constexpr uint64 RcbkBlockEntryBytes = 8;

bool ValidRow(const NuggetValueRow& row) noexcept {
    const uint8 encoding = static_cast<uint8>(row.encoding);
    if (encoding > static_cast<uint8>(NuggetEncoding::Codec)) {
        return false;
    }

    if (row.rawSize > NuggetValueMaxBytes || row.storedSize > NuggetValueMaxBytes) {
        return false;
    }

    // 색인은 Rcbk만 가진다.
    if (row.encoding != NuggetEncoding::Rcbk && (row.indexLength != 0 || !IsZero(row.indexHash))) {
        return false;
    }

    if (row.encoding == NuggetEncoding::Identity &&
        (row.storedSize != row.rawSize || !(row.contentHash == row.rawHash))) {
        return false;
    }

    if (row.indexLength > row.storedSize) {
        return false;
    }

    // Rcbk 색인은 헤더 + 블록 표(48 + 8n)이고 IndexHash가 있다.
    if (row.encoding == NuggetEncoding::Rcbk &&
        (row.indexLength < RcbkHeaderBytes ||
         (row.indexLength - RcbkHeaderBytes) % RcbkBlockEntryBytes != 0 || IsZero(row.indexHash))) {
        return false;
    }

    // 조각 해시 표는 storedSize ≥ 1 MiB인 값만 가진다(표 해시는 0이 아니다).
    return (row.storedSize >= NuggetChunkBytes) != IsZero(row.chunkTableHash);
}

} // namespace

bool IsValidNuggetValueRow(const NuggetValueRow& row) noexcept {
    return ValidRow(row);
}

bool IsValidNuggetBucket(StringView bucket) noexcept {
    if (bucket.IsEmpty() || bucket.ByteCount() > NuggetNameMaxBytes || !IsLower(bucket.Data()[0])) {
        return false;
    }

    for (usize index = 1; index < bucket.ByteCount(); ++index) {
        const char c = bucket.Data()[index];
        if (!IsLower(c) && !IsDigit(c) && c != '_') {
            return false;
        }
    }

    return true;
}

bool IsValidNuggetNamespace(StringView name) noexcept {
    if (name.IsEmpty() || name.ByteCount() > NuggetNameMaxBytes) {
        return false;
    }

    const char first = name.Data()[0];
    if (!IsLower(first) && !IsDigit(first)) {
        return false;
    }

    for (usize index = 1; index < name.ByteCount(); ++index) {
        const char c = name.Data()[index];
        if (!IsLower(c) && !IsDigit(c) && c != '.' && c != '_' && c != '-') {
            return false;
        }
    }

    return true;
}

int32 CompareNuggetInputs(const NuggetInput& left, const NuggetInput& right) noexcept {
    const StringView a = left.bucket.View();
    const StringView b = right.bucket.View();
    // 정규 인코딩은 u64 LE 길이 + byte다. bucket은 64 byte 이하라 길이의 첫 byte만 0이 아니므로
    // 인코딩 byte 사전순은 길이 수 비교 → 내용 비교와 같다.
    if (a.ByteCount() != b.ByteCount()) {
        return a.ByteCount() < b.ByteCount() ? -1 : 1;
    }

    if (const int32 bucket = nugget_detail::CompareBytes(BytesOf(a), BytesOf(b)); bucket != 0) {
        return bucket;
    }

    if (const int32 key = nugget_detail::CompareBytes(left.key.bytes, right.key.bytes); key != 0) {
        return key;
    }

    return nugget_detail::CompareBytes(left.digest.bytes, right.digest.bytes);
}

Result<void> ValidateNuggetRecord(const NuggetRecord& record) noexcept {
    auto invalid = [] { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    if (!IsValidNuggetNamespace(record.nameSpace.View()) ||
        !IsValidNuggetBucket(record.bucket.View()) || record.meta.Size() > NuggetMetaMaxBytes) {
        return invalid();
    }

    for (usize index = 0; index < record.values.Size(); ++index) {
        if (!ValidRow(record.values[index]) ||
            (index > 0 && record.values[index].id <= record.values[index - 1].id)) {
            return invalid();
        }
    }

    for (usize index = 0; index < record.inputs.Size(); ++index) {
        if (!IsValidNuggetBucket(record.inputs[index].bucket.View()) ||
            (index > 0 &&
             CompareNuggetInputs(record.inputs[index - 1], record.inputs[index]) >= 0)) {
            return invalid();
        }
    }

    return Result<void>::Success();
}

Result<Array<uint8>> WriteNuggetRecord(Allocator& allocator, const NuggetRecord& record) noexcept {
    using Out = Array<uint8>;
    if (auto valid = ValidateNuggetRecord(record); !valid) {
        return Result<Out>::Failure(valid.Error());
    }

    Array<uint8> rows(allocator);
    if (auto reserved = rows.TryReserve(record.values.Size() * NuggetValueRowBytes); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (const NuggetValueRow& row : record.values) {
        uint8 encoded[NuggetValueRowBytes];
        EncodeRow(row, encoded);
        // 자리는 위에서 잡았다.
        (void)rows.TryAppend(encoded);
    }

    RecordBuilder builder(allocator, NuggetRecordSchema, NuggetRecordSchemaVersion);
    Result<void> step = builder.AddField(NuggetRecordNamespaceField, WireType::String,
                                         BytesOf(record.nameSpace.View()));
    if (step) {
        step = builder.AddField(NuggetRecordBucketField, WireType::String,
                                BytesOf(record.bucket.View()));
    }

    if (step) {
        step = builder.AddField(NuggetRecordKeyField, WireType::Bytes, record.key.bytes);
    }

    if (step) {
        step = builder.AddField(NuggetRecordValuesField, WireType::Bytes, rows.View());
    }

    RecordListBuilder inputs(allocator);
    for (usize index = 0; step && index < record.inputs.Size(); ++index) {
        const NuggetInput& input = record.inputs[index];
        RecordBuilder item(allocator, NuggetInputSchema, NuggetRecordSchemaVersion);
        step =
            item.AddField(NuggetInputBucketField, WireType::String, BytesOf(input.bucket.View()));
        if (step) {
            step = item.AddField(NuggetInputKeyField, WireType::Bytes, input.key.bytes);
        }

        if (step) {
            step = item.AddField(NuggetInputDigestField, WireType::Bytes, input.digest.bytes);
        }

        if (step) {
            auto built = item.Build();
            step =
                built ? inputs.AddItem(built.Value().View()) : Result<void>::Failure(built.Error());
        }
    }

    if (step) {
        step = inputs.Finish(builder, NuggetRecordInputsField);
    }

    if (step) {
        step = builder.AddField(NuggetRecordMetaField, WireType::Bytes, record.meta.View());
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    auto built = builder.Build();
    if (!built) {
        return built;
    }

    if (built.Value().Size() > NuggetRecordMaxBytes) {
        return Result<Out>::Failure(ErrorCode::CapacityExceeded);
    }

    return built;
}

Result<NuggetRecord> ReadNuggetRecord(Allocator& allocator, Span<const uint8> bytes) noexcept {
    using Out = NuggetRecord;
    if (bytes.Size() > NuggetRecordMaxBytes) {
        return Result<Out>::Failure(ErrorCode::CapacityExceeded);
    }

    auto header = ReadRecordHeader(bytes);
    if (!header) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    if (!(header.Value().schema == NuggetRecordSchema)) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    if (header.Value().schemaVersion > NuggetRecordSchemaVersion) {
        return Result<Out>::Failure(ErrorCode::Unsupported);
    }

    auto reader =
        RecordReader::Open(allocator, bytes, NuggetRecordSchema, 1, NuggetRecordSchemaVersion);
    if (!reader) {
        return Result<Out>::Failure(reader.Error() == ErrorCode::OutOfMemory
                                        ? ErrorCode::OutOfMemory
                                        : ErrorCode::InvalidArgument);
    }

    NuggetRecord record(allocator);
    // 필드는 1–6이 정확히 그 순서로 있다(Record TLV는 순서를 강제하지 않으므로 여기서 본다).
    const WireType wires[RecordFieldCount] = {WireType::String,     WireType::String,
                                              WireType::Bytes,      WireType::Bytes,
                                              WireType::RecordList, WireType::Bytes};
    FieldView fields[RecordFieldCount];
    for (uint32 index = 0; index < RecordFieldCount; ++index) {
        auto field = reader.Value().Next();
        if (!field || !(field.Value().id == Guid{0, index + 1}) ||
            field.Value().wireType != static_cast<uint32>(wires[index])) {
            // Next는 할당하지 않는다(없음·예산 소진만 실패) — 형식 위반이다.
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        fields[index] = field.Value();
    }

    if (auto extra = reader.Value().Next(); extra) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    auto text = [](Span<const uint8> payload) noexcept {
        return StringView(reinterpret_cast<const char*>(payload.Data()), payload.Size());
    };
    Result<void> step = record.nameSpace.TryAssign(text(fields[0].payload));
    if (step) {
        step = record.bucket.TryAssign(text(fields[1].payload));
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    if (fields[2].payload.Size() != DigestBytes ||
        fields[3].payload.Size() % NuggetValueRowBytes != 0) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize at = 0; at < DigestBytes; ++at) {
        record.key.bytes[at] = fields[2].payload[at];
    }

    const usize rowCount = fields[3].payload.Size() / NuggetValueRowBytes;
    if (auto reserved = record.values.TryReserve(rowCount); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize index = 0; index < rowCount; ++index) {
        NuggetValueRow row;
        if (!DecodeRow(fields[3].payload.Slice(index * NuggetValueRowBytes, NuggetValueRowBytes),
                       row)) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        (void)record.values.TryPushBack(row);
    }

    auto items = RecordListReader::OpenField(reader.Value(), fields[4], NuggetInputSchema, 1,
                                             NuggetRecordSchemaVersion);
    if (!items) {
        return Result<Out>::Failure(items.Error() == ErrorCode::OutOfMemory
                                        ? ErrorCode::OutOfMemory
                                        : ErrorCode::InvalidArgument);
    }

    if (auto reserved = record.inputs.TryReserve(items.Value().ItemCount()); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize index = 0; index < items.Value().ItemCount(); ++index) {
        auto item = items.Value().OpenItem(index);
        if (!item) {
            return Result<Out>::Failure(item.Error() == ErrorCode::OutOfMemory
                                            ? ErrorCode::OutOfMemory
                                            : ErrorCode::InvalidArgument);
        }

        const WireType inputWires[InputFieldCount] = {WireType::String, WireType::Bytes,
                                                      WireType::Bytes};
        FieldView parts[InputFieldCount];
        for (uint32 at = 0; at < InputFieldCount; ++at) {
            auto field = item.Value().Next();
            if (!field || !(field.Value().id == Guid{0, at + 1}) ||
                field.Value().wireType != static_cast<uint32>(inputWires[at])) {
                return Result<Out>::Failure(ErrorCode::InvalidArgument);
            }

            parts[at] = field.Value();
        }

        if (auto extra = item.Value().Next(); extra || parts[1].payload.Size() != DigestBytes ||
                                              parts[2].payload.Size() != DigestBytes) {
            return Result<Out>::Failure(ErrorCode::InvalidArgument);
        }

        NuggetInput input(allocator);
        if (auto assigned = input.bucket.TryAssign(text(parts[0].payload)); !assigned) {
            return Result<Out>::Failure(assigned.Error());
        }

        for (usize at = 0; at < DigestBytes; ++at) {
            input.key.bytes[at] = parts[1].payload[at];
            input.digest.bytes[at] = parts[2].payload[at];
        }

        (void)record.inputs.TryPushBack(Move(input));
    }

    if (auto appended = record.meta.TryAppend(fields[5].payload); !appended) {
        return Result<Out>::Failure(appended.Error());
    }

    if (auto valid = ValidateNuggetRecord(record); !valid) {
        return Result<Out>::Failure(valid.Error());
    }

    return Result<Out>::Success(Move(record));
}

Sha256Digest NuggetRecordDigest(const NuggetRecord& record) noexcept {
    Sha256 hasher;
    auto text = [&hasher](StringView value) noexcept {
        uint8 length[U64Bytes];
        StoreLittleEndian<uint64>(length, value.ByteCount());
        hasher.Update(length);
        hasher.Update(BytesOf(value));
    };
    text(record.nameSpace.View());
    text(record.bucket.View());
    hasher.Update(record.key.bytes);
    uint8 count[U32Bytes];
    StoreLittleEndian<uint32>(count, static_cast<uint32>(record.values.Size()));
    hasher.Update(count);
    for (const NuggetValueRow& row : record.values) {
        uint8 head[U32Bytes + U64Bytes];
        StoreLittleEndian<uint32>(head, row.id);
        StoreLittleEndian<uint64>(head + U32Bytes, row.rawSize);
        hasher.Update(head);
        hasher.Update(row.rawHash.bytes);
    }

    StoreLittleEndian<uint32>(count, static_cast<uint32>(record.inputs.Size()));
    hasher.Update(count);
    for (const NuggetInput& input : record.inputs) {
        text(input.bucket.View());
        hasher.Update(input.key.bytes);
        hasher.Update(input.digest.bytes);
    }

    hasher.Update(Sha256::Hash(record.meta.View()).bytes);
    return hasher.Finish();
}

} // namespace rocks
