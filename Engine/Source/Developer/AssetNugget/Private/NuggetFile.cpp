#include <AssetNugget/NuggetFile.h>

#include <Base/Endian.h>
#include <Compression/CompressedBlocks.h>

// `.rnug` v2 파일이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 6 — NuggetFile.h).
namespace rocks {

namespace {

constexpr uint8 FileMagic[4] = {'R', 'N', 'U', 'G'};
constexpr usize MagicBytes = 4;
constexpr usize VersionAt = 4;
constexpr usize RecordBytesAt = 8;
constexpr usize RecordHashAt = 16;
static_assert(RecordHashAt + Sha256Digest::Bytes == NuggetFileHeadBytes,
              "nugget file head layout must be 48 bytes");

// 조각 해시 표의 byte 수다(⌈storedSize / 1 MiB⌉ × 32 — 1 MiB 미만이면 표가 없다).
uint64 ChunkTableBytes(uint64 storedSize) noexcept {
    if (storedSize < NuggetChunkBytes) {
        return 0;
    }

    return ((storedSize + NuggetChunkBytes - 1) / NuggetChunkBytes) * Sha256Digest::Bytes;
}

// stored의 조각 해시 표를 out에 쓴다(1 MiB 미만이면 비운다).
Result<void> MakeChunkTable(Span<const uint8> stored, Array<uint8>& out) noexcept {
    out.Clear();
    const uint64 bytes = ChunkTableBytes(stored.Size());
    if (bytes == 0) {
        return Result<void>::Success();
    }

    if (auto reserved = out.TryReserve(static_cast<usize>(bytes)); !reserved) {
        return reserved;
    }

    for (uint64 at = 0; at < stored.Size(); at += NuggetChunkBytes) {
        const uint64 left = stored.Size() - at;
        const usize chunk = static_cast<usize>(left < NuggetChunkBytes ? left : NuggetChunkBytes);
        const Sha256Digest digest = Sha256::Hash(stored.Slice(static_cast<usize>(at), chunk));
        (void)out.TryAppend(digest.bytes);
    }

    return Result<void>::Success();
}

} // namespace

Result<void> NuggetMemoryReader::ReadAt(uint64 offset, Span<uint8> out) noexcept {
    if (offset > bytes_.Size() || out.Size() > bytes_.Size() - offset) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < out.Size(); ++index) {
        out[index] = bytes_[static_cast<usize>(offset) + index];
    }

    return Result<void>::Success();
}

Result<NuggetFilePlan> PlanNuggetFile(Allocator& allocator, StringView bucket,
                                      const Sha256Digest& key, Span<const NuggetStoredValue> values,
                                      Span<const NuggetInput> inputs,
                                      Span<const uint8> meta) noexcept {
    using Out = NuggetFilePlan;
    NuggetFilePlan plan(allocator);
    NuggetRecord& record = plan.record;
    Result<void> step = record.nameSpace.TryAssign(NuggetNamespace);
    if (step) {
        step = record.bucket.TryAssign(bucket);
    }

    if (step) {
        step = record.values.TryReserve(values.Size());
    }

    if (step) {
        step = plan.tables.TryReserve(values.Size());
    }

    if (step) {
        step = record.inputs.TryReserve(inputs.Size());
    }

    if (step) {
        step = record.meta.TryAppend(meta);
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    record.key = key;
    for (const NuggetInput& input : inputs) {
        NuggetInput copy(allocator);
        if (auto assigned = copy.bucket.TryAssign(input.bucket.View()); !assigned) {
            return Result<Out>::Failure(assigned.Error());
        }

        copy.key = input.key;
        copy.digest = input.digest;
        (void)record.inputs.TryPushBack(Move(copy));
    }

    uint64 total = 0;
    for (const NuggetStoredValue& value : values) {
        NuggetValueRow row;
        row.id = value.id;
        row.encoding = value.encoding;
        row.rawSize = value.rawSize;
        row.storedSize = value.stored.Size();
        row.indexLength = value.indexLength;
        row.rawHash = value.rawHash;
        row.indexHash = value.indexHash;
        row.contentHash = Sha256::Hash(value.stored);
        Array<uint8> table(allocator);
        if (auto made = MakeChunkTable(value.stored, table); !made) {
            return Result<Out>::Failure(made.Error());
        }

        if (!table.IsEmpty()) {
            row.chunkTableHash = Sha256::Hash(table.View());
        }

        total += row.storedSize + table.Size();
        (void)record.values.TryPushBack(row);
        (void)plan.tables.TryPushBack(Move(table));
    }

    auto bytes = WriteNuggetRecord(allocator, record);
    if (!bytes) {
        return Result<Out>::Failure(bytes.Error());
    }

    if (auto reserved = plan.head.TryResize(NuggetFileHeadBytes); !reserved) {
        return Result<Out>::Failure(reserved.Error());
    }

    for (usize index = 0; index < MagicBytes; ++index) {
        plan.head[index] = FileMagic[index];
    }

    StoreLittleEndian<uint32>(plan.head.Data() + VersionAt, NuggetFileVersion);
    StoreLittleEndian<uint64>(plan.head.Data() + RecordBytesAt, bytes.Value().Size());
    const Sha256Digest recordHash = Sha256::Hash(bytes.Value().View());
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        plan.head[RecordHashAt + index] = recordHash.bytes[index];
    }

    if (auto appended = plan.head.TryAppend(bytes.Value().View()); !appended) {
        return Result<Out>::Failure(appended.Error());
    }

    plan.fileBytes = plan.head.Size() + total;
    return Result<Out>::Success(Move(plan));
}

Result<void> AppendNuggetFile(const NuggetFilePlan& plan, Span<const NuggetStoredValue> values,
                              Array<uint8>& out) noexcept {
    if (values.Size() != plan.record.values.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < values.Size(); ++index) {
        if (values[index].stored.Size() != plan.record.values[index].storedSize) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    if (auto reserved = out.TryReserve(out.Size() + static_cast<usize>(plan.fileBytes));
        !reserved) {
        return reserved;
    }

    (void)out.TryAppend(plan.head.View());
    for (const NuggetStoredValue& value : values) {
        (void)out.TryAppend(value.stored);
    }

    for (const Array<uint8>& table : plan.tables) {
        (void)out.TryAppend(table.View());
    }

    return Result<void>::Success();
}

Result<NuggetFileView> OpenNuggetFile(Allocator& allocator, NuggetRangeReader& reader) noexcept {
    using Out = NuggetFileView;
    auto corrupt = [] { return Result<Out>::Failure(ErrorCode::InvalidArgument); };
    const uint64 size = reader.Size();
    if (size < NuggetFileHeadBytes) {
        return corrupt();
    }

    uint8 head[NuggetFileHeadBytes];
    if (auto read = reader.ReadAt(0, head); !read) {
        return Result<Out>::Failure(read.Error());
    }

    for (usize index = 0; index < MagicBytes; ++index) {
        if (head[index] != FileMagic[index]) {
            return corrupt();
        }
    }

    if (LoadLittleEndian<uint32>(head + VersionAt) != NuggetFileVersion) {
        return Result<Out>::Failure(ErrorCode::Unsupported);
    }

    const uint64 recordBytes = LoadLittleEndian<uint64>(head + RecordBytesAt);
    if (recordBytes > NuggetRecordMaxBytes || recordBytes > size - NuggetFileHeadBytes) {
        return corrupt();
    }

    Array<uint8> bytes(allocator);
    if (auto resized = bytes.TryResize(static_cast<usize>(recordBytes)); !resized) {
        return Result<Out>::Failure(resized.Error());
    }

    if (auto read = reader.ReadAt(NuggetFileHeadBytes, bytes.View()); !read) {
        return Result<Out>::Failure(read.Error());
    }

    Sha256Digest expected;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        expected.bytes[index] = head[RecordHashAt + index];
    }

    if (!(Sha256::Hash(bytes.View()) == expected)) {
        return corrupt();
    }

    // 더 새 Record schema 버전(Unsupported)은 손상이 아니다 — 그대로 돌려준다(읽는 쪽은 지우지
    // 않는다).
    auto record = ReadNuggetRecord(allocator, bytes.View());
    if (!record) {
        const ErrorCode error = record.Error();
        return Result<Out>::Failure(error == ErrorCode::OutOfMemory ||
                                            error == ErrorCode::Unsupported
                                        ? error
                                        : ErrorCode::InvalidArgument);
    }

    NuggetFileView view(allocator);
    view.record = Move(record.Value());
    view.digest = NuggetRecordDigest(view.record);
    const usize count = view.record.values.Size();
    Result<void> step = view.offsets.TryReserve(count);
    if (step) {
        step = view.tables.TryReserve(count);
    }

    if (!step) {
        return Result<Out>::Failure(step.Error());
    }

    // 부분 합이 파일 크기와 정확히 같아야 한다(값 크기는 행마다 16 GiB 이하 — 합이 넘치지 않는다).
    uint64 at = NuggetFileHeadBytes + recordBytes;
    for (const NuggetValueRow& row : view.record.values) {
        (void)view.offsets.TryPushBack(at);
        at += row.storedSize;
    }

    uint64 tablesAt = at;
    for (const NuggetValueRow& row : view.record.values) {
        at += ChunkTableBytes(row.storedSize);
    }

    if (at != size) {
        return corrupt();
    }

    for (const NuggetValueRow& row : view.record.values) {
        Array<uint8> table(allocator);
        const uint64 tableBytes = ChunkTableBytes(row.storedSize);
        if (auto resized = table.TryResize(static_cast<usize>(tableBytes)); !resized) {
            return Result<Out>::Failure(resized.Error());
        }

        if (tableBytes != 0) {
            if (auto read = reader.ReadAt(tablesAt, table.View()); !read) {
                return Result<Out>::Failure(read.Error());
            }

            if (!(Sha256::Hash(table.View()) == row.chunkTableHash)) {
                return corrupt();
            }
        }

        tablesAt += tableBytes;
        (void)view.tables.TryPushBack(Move(table));
    }

    return Result<Out>::Success(Move(view));
}

Result<void> ReadNuggetValue(const NuggetFileView& view, NuggetRangeReader& reader, usize index,
                             Array<uint8>& out) noexcept {
    if (index >= view.record.values.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const NuggetValueRow& row = view.record.values[index];
    const usize start = out.Size();
    if (auto resized = out.TryResize(start + static_cast<usize>(row.storedSize)); !resized) {
        return resized;
    }

    const Span<uint8> target = out.View().Slice(start, static_cast<usize>(row.storedSize));
    if (auto read = reader.ReadAt(view.offsets[index], target); !read) {
        return read;
    }

    if (!(Sha256::Hash(Span<const uint8>(target.Data(), target.Size())) == row.contentHash)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> VerifyNuggetValue(const NuggetFileView& view, NuggetRangeReader& reader, usize index,
                               Span<uint8> buffer) noexcept {
    if (index >= view.record.values.Size() || buffer.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const NuggetValueRow& row = view.record.values[index];
    Sha256 hasher;
    uint64 done = 0;
    while (done < row.storedSize) {
        const uint64 left = row.storedSize - done;
        const usize step = left < buffer.Size() ? static_cast<usize>(left) : buffer.Size();
        const Span<uint8> target = buffer.Slice(0, step);
        if (auto read = reader.ReadAt(view.offsets[index] + done, target); !read) {
            return read;
        }

        hasher.Update(Span<const uint8>(target.Data(), target.Size()));
        done += step;
    }

    if (!(hasher.Finish() == row.contentHash)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> DecodeNuggetValue(Allocator& allocator, const NuggetValueRow& row,
                               Span<const uint8> stored, Array<uint8>& out) noexcept {
    auto corrupt = [] { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    // 검증되지 않은 행도 받는 공개 함수다 — 정규 형식 행 검사를 먼저 한다.
    if (!IsValidNuggetValueRow(row) || stored.Size() != row.storedSize) {
        return corrupt();
    }

    const usize start = out.Size();
    switch (row.encoding) {
    case NuggetEncoding::Identity:
        if (auto appended = out.TryAppend(stored); !appended) {
            return appended;
        }

        break;
    case NuggetEncoding::Rcbk: {
        // 색인 길이 ≤ storedSize = stored 크기는 행 검사가 보장한다.
        if (!(Sha256::Hash(stored.Slice(0, static_cast<usize>(row.indexLength))) ==
              row.indexHash)) {
            return corrupt();
        }

        auto reader = CompressedBlocksReader::TryOpen(stored, allocator);
        if (!reader) {
            return reader.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(reader.Error())
                                                            : corrupt();
        }

        if (reader.Value().RawSize() != row.rawSize ||
            reader.Value().IndexBytes() != row.indexLength) {
            return corrupt();
        }

        auto context = CompressedBlocksReadContext::TryCreate(reader.Value(), allocator);
        if (!context) {
            return Result<void>::Failure(context.Error());
        }

        if (auto resized = out.TryResize(start + static_cast<usize>(row.rawSize)); !resized) {
            return resized;
        }

        if (auto read =
                reader.Value().TryRead(0, out.View().Slice(start, static_cast<usize>(row.rawSize)),
                                       stored, context.Value());
            !read) {
            (void)out.TryResize(start);
            return read.Error() == ErrorCode::OutOfMemory ? Result<void>::Failure(read.Error())
                                                          : corrupt();
        }

        break;
    }
    default:
        // 사용자 등록 형식(Codec)은 계약이다(그 코덱이 없다 — 미적중).
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const Span<const uint8> raw(out.Data() + start, out.Size() - start);
    // 크기는 맞다(Identity는 행 검사가 storedSize = rawSize를, Rcbk는 RawSize 대조가 보장한다).
    if (!(Sha256::Hash(raw) == row.rawHash)) {
        (void)out.TryResize(start);
        return corrupt();
    }

    return Result<void>::Success();
}

} // namespace rocks
