#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Base/PlacementNew.h>
#include <Core/FileError.h>
#include <Platform/Clock.h>

// 스냅샷이다(Docs/Design/TraceSystem.ko.md 8절 "스냅샷"). 순서: 머리 → 스키마 전체 → 정의 저장소 →
// 최근 기록 고리 → 동기 표지. writer 잠금은 제한 시간 안에서만 시도한다.
namespace rocks {

using detail::TraceRecentCopy;
using detail::TraceRecordShape;
using detail::TraceSessionState;
using detail::TraceTypeEntry;
using platform::AtomicLoad;
using platform::MemoryOrder;

namespace {

// deadline(MonotonicTicks)까지 잠금을 시도한다. 제한 시간 0이면 한 번만 시도한다. 스냅샷은 드물고
// 짧게 기다리므로 양보하며 다시 시도한다.
bool TryLockUntil(platform::Mutex& mutex, uint64 deadline) noexcept {
    for (;;) {
        if (mutex.TryLock()) {
            return true;
        }

        if (platform::MonotonicTicks() >= deadline) {
            return false;
        }

        platform::YieldThread();
    }
}

// 잠금을 얻은 뒤 범위를 벗어날 때 푼다.
class AdoptedLock {
public:
    explicit AdoptedLock(platform::Mutex& mutex) noexcept : mutex_(mutex) {
    }

    ~AdoptedLock() noexcept {
        mutex_.Unlock();
    }

    AdoptedLock(const AdoptedLock&) = delete;
    AdoptedLock& operator=(const AdoptedLock&) = delete;

private:
    platform::Mutex& mutex_;
};

// bytes를 압축하지 않은 덩어리들로 out에 붙인다(덩어리 하나의 원본 상한으로 나눈다). 스트림 내용은
// 덩어리를 이어 붙여 읽으므로 기록 경계와 덩어리 경계가 달라도 된다.
Result<void> AppendRawChunks(Array<uint8>& out, uint16 stream, Span<const uint8> bytes) noexcept {
    while (!bytes.IsEmpty()) {
        const usize take = bytes.Size() < TraceFormat::MaxChunkPayloadBytes
                               ? bytes.Size()
                               : TraceFormat::MaxChunkPayloadBytes;
        uint8 header[TraceFormat::ChunkHeaderBytes];
        StoreLittleEndian<uint16>(header, static_cast<uint16>(take));
        StoreLittleEndian<uint16>(header + sizeof(uint16), stream);
        if (auto appended = out.TryAppend(Span<const uint8>(header, sizeof(header))); !appended) {
            return appended;
        }

        if (auto appended = out.TryAppend(bytes.Left(take)); !appended) {
            return appended;
        }

        bytes = bytes.RightChop(take);
    }

    return Result<void>::Success();
}

// 스키마 기록을 모아 스키마 스트림 덩어리로 붙인다. registryMutex를 잡은 채 부른다. count는 붙일 형
// 표 항목 수다(완전 스냅샷은 지금까지 낸 것, 불완전 스냅샷은 등록된 것 전부).
Result<void> AppendSchemas(TraceSessionState& state, Array<uint8>& out, usize count) noexcept {
    Array<uint8> records(*state.allocator);
    for (usize index = 0; index < count; ++index) {
        const TraceTypeEntry& entry = state.types[index];
        if (entry.present) {
            if (auto appended = records.TryAppend(
                    state.schemaBytes.View().Mid(entry.schemaOffset, entry.schemaBytes));
                !appended) {
                return appended;
            }
        }
    }

    return AppendRawChunks(out, TraceFormat::SchemaStreamId, records.View());
}

// 동기 표지(Trace.SnapshotSync)를 정의 스트림 덩어리로 붙인다.
Result<void> AppendSync(Array<uint8>& out, uint64 order, const TraceSnapshotInfo& info) noexcept {
    uint8 fixed[TraceFormat::SnapshotSyncFixedBytes];
    StoreLittleEndian<uint64>(fixed + TraceFormat::SnapshotSyncTickOffset,
                              platform::MonotonicTicks());
    StoreLittleEndian<uint64>(fixed + TraceFormat::SnapshotSyncOrderOffset, order);
    StoreLittleEndian<uint64>(fixed + TraceFormat::SnapshotSyncDroppedChunksOffset,
                              info.droppedChunks);
    StoreLittleEndian<uint64>(fixed + TraceFormat::SnapshotSyncDroppedBytesOffset,
                              info.droppedBytes);
    StoreLittleEndian<uint64>(fixed + TraceFormat::SnapshotSyncSkippedChunksOffset,
                              info.skippedChunks);
    fixed[TraceFormat::SnapshotSyncCompleteOffset] = info.complete ? 1 : 0;
    TraceRecordShape shape;
    shape.typeId = TraceFormat::SnapshotSyncTypeId;
    shape.fixed = fixed;
    shape.fixedBytes = sizeof(fixed);
    uint8 record[sizeof(uint16) + sizeof(fixed)];
    detail::EncodeTraceRecord(record, shape);
    return AppendRawChunks(out, TraceFormat::DefinitionStreamId,
                           Span<const uint8>(record, sizeof(record)));
}

Result<void> AppendHeader(TraceSessionState& state, Array<uint8>& out, uint64 attachOrder,
                          uint32 flags) noexcept {
    uint8 header[TraceFormat::FileHeaderBytes];
    state.BuildFileHeader(header, attachOrder, flags);
    return out.TryAppend(Span<const uint8>(header, sizeof(header)));
}

// writer 잠금을 쥔 채 완전한 스냅샷을 만든다.
Result<TraceSnapshotInfo> BuildComplete(TraceSessionState& state, Array<uint8>& out) noexcept {
    // 지금까지 공개된 기록을 고리로 옮긴다.
    state.Pump();
    TraceSnapshotInfo info;
    info.complete = true;
    info.droppedChunks = state.recent.DroppedChunks();
    info.droppedBytes = state.recent.DroppedBytes();
    const uint64 order = AtomicLoad<MemoryOrder::Relaxed>(&state.nextOrder);
    uint32 flags = TraceFormat::HeaderFlagSnapshot;
    if (state.definitionStoreOverflowed || state.definitions.Dropped() != 0) {
        flags |= TraceFormat::HeaderFlagDefinitionsIncomplete;
    }

    // 고리가 넘친 적이 있으면 앞부분 기록이 없을 수 있다. 머리의 순서 번호 칸을 스냅샷 시점 번호로
    // 두어 분석기가 그보다 작은 순서 번호의 구멍을 손실로 세지 않게 한다.
    if (info.droppedChunks != 0) {
        flags |= TraceFormat::HeaderFlagRecentDropped;
    }

    if (auto header = AppendHeader(state, out, info.droppedChunks != 0 ? order : 0, flags);
        !header) {
        return Result<TraceSnapshotInfo>::Failure(header.Error());
    }

    {
        platform::LockGuard lock(state.registryMutex);
        if (auto schemas = AppendSchemas(state, out, state.schemasEmitted); !schemas) {
            return Result<TraceSnapshotInfo>::Failure(schemas.Error());
        }
    }

    if (auto store = out.TryAppend(state.definitionStore.View()); !store) {
        return Result<TraceSnapshotInfo>::Failure(store.Error());
    }

    auto copied = state.recent.AppendTo(out);
    if (!copied) {
        return Result<TraceSnapshotInfo>::Failure(copied.Error());
    }

    info.recentChunks = copied.Value().chunks;
    info.skippedChunks = copied.Value().skippedChunks;
    if (auto sync = AppendSync(out, order, info); !sync) {
        return Result<TraceSnapshotInfo>::Failure(sync.Error());
    }

    return Result<TraceSnapshotInfo>::Success(info);
}

// writer 잠금 없이 가진 것만 쓴다: 머리(불완전) → 스키마(등록 표 잠금을 얻었으면) → 동기 표지.
Result<TraceSnapshotInfo> BuildIncomplete(TraceSessionState& state, Array<uint8>& out,
                                          uint64 deadline) noexcept {
    TraceSnapshotInfo info;
    const uint64 order = AtomicLoad<MemoryOrder::Relaxed>(&state.nextOrder);
    if (auto header = AppendHeader(state, out, order,
                                   TraceFormat::HeaderFlagSnapshot |
                                       TraceFormat::HeaderFlagSnapshotIncomplete);
        !header) {
        return Result<TraceSnapshotInfo>::Failure(header.Error());
    }

    if (TryLockUntil(state.registryMutex, deadline)) {
        AdoptedLock lock(state.registryMutex);
        if (auto schemas = AppendSchemas(state, out, state.types.Size()); !schemas) {
            return Result<TraceSnapshotInfo>::Failure(schemas.Error());
        }
    }

    if (auto sync = AppendSync(out, order, info); !sync) {
        return Result<TraceSnapshotInfo>::Failure(sync.Error());
    }

    return Result<TraceSnapshotInfo>::Success(info);
}

Result<TraceSnapshotInfo> BuildSnapshot(TraceSessionState& state, Array<uint8>& out,
                                        uint32 lockTimeoutMs) noexcept {
    constexpr uint64 MillisecondsPerSecond = 1000;
    const uint64 deadline =
        platform::MonotonicTicks() + lockTimeoutMs * state.tickFrequency / MillisecondsPerSecond;
    if (TryLockUntil(state.writerMutex, deadline)) {
        AdoptedLock lock(state.writerMutex);
        return BuildComplete(state, out);
    }

    return BuildIncomplete(state, out, deadline);
}

} // namespace

Result<TraceSnapshotInfo> TraceSession::WriteSnapshot(TraceOutputWrite write, void* context,
                                                      uint32 lockTimeoutMs) noexcept {
    if (state_ == nullptr) {
        return Result<TraceSnapshotInfo>::Failure(ErrorCode::WrongContext);
    }

    if (write == nullptr) {
        return Result<TraceSnapshotInfo>::Failure(ErrorCode::InvalidArgument);
    }

    Array<uint8> bytes(*state_->allocator);
    auto built = BuildSnapshot(*state_, bytes, lockTimeoutMs);
    if (!built) {
        return built;
    }

    // 잠금 밖에서 넘긴다(콜백이 세션 API를 불러도 교착하지 않는다).
    if (auto written = write(context, bytes.View()); !written) {
        return Result<TraceSnapshotInfo>::Failure(written.Error());
    }

    TraceSnapshotInfo info = built.Value();
    info.bytes = bytes.Size();
    return Result<TraceSnapshotInfo>::Success(info);
}

Result<TraceSnapshotInfo> TraceSession::WriteSnapshot(const char* path,
                                                      uint32 lockTimeoutMs) noexcept {
    if (state_ == nullptr) {
        return Result<TraceSnapshotInfo>::Failure(ErrorCode::WrongContext);
    }

    if (path == nullptr || path[0] == '\0') {
        return Result<TraceSnapshotInfo>::Failure(ErrorCode::InvalidArgument);
    }

    Array<uint8> bytes(*state_->allocator);
    auto built = BuildSnapshot(*state_, bytes, lockTimeoutMs);
    if (!built) {
        return built;
    }

    platform::File file;
    platform::FileStatus status = file.Open(path, platform::FileOpenMode::CreateAlways);
    if (status) {
        const platform::FileTransfer transfer = file.WriteAll(bytes.Data(), bytes.Size());
        status = !transfer.status ? transfer.status
                 : transfer.bytes != bytes.Size()
                     ? platform::FileStatus{platform::FileCode::NoProgress, 0}
                     : platform::FileStatus{};
        const platform::FileStatus closed = file.Close();
        if (status && !closed) {
            status = closed;
        }
    }

    if (!status) {
        // 원본 파일 오류(OS 오류 포함)는 통계에 남긴다(writer가 잡고 있으면 건너뛴다 — 기다리지
        // 않는다). 로그는 실패를 확정하는 호출자가 남긴다.
        if (state_->writerMutex.TryLock()) {
            state_->lastOutputFileStatus = status;
            state_->writerMutex.Unlock();
        }

        return Result<TraceSnapshotInfo>::Failure(FileErrorCode(status));
    }

    TraceSnapshotInfo info = built.Value();
    info.bytes = bytes.Size();
    return Result<TraceSnapshotInfo>::Success(info);
}

} // namespace rocks
