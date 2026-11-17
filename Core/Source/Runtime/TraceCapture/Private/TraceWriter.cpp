#include "TraceSessionInternal.h"

#include <Base/Endian.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

// writer다(Docs/Design/TraceSystem.ko.md 8절). 스키마·정의·스레드 스트림을 덩어리로 나눠 출력에
// 보내고, 정의 덩어리의 사본을 정의 저장소에 둔다. 모든 함수는 writerMutex를 잡은 채 부른다.
namespace rocks::detail {

using platform::AtomicLoad;

namespace {

void StoreChunkHeader(uint8* header, uint16 sizeAndFlags, uint16 stream) noexcept {
    StoreLittleEndian<uint16>(header, sizeAndFlags);
    StoreLittleEndian<uint16>(header + sizeof(uint16), stream);
}

uint16 ThreadStreamId(uint32 threadId) noexcept {
    return static_cast<uint16>(TraceFormat::FirstThreadStreamId + threadId - 1);
}

} // namespace

Result<void> TraceSessionState::InitializeWriter() noexcept {
    auto bound = TryGetCompressBound(CompressionFormat::Lz4, TraceFormat::MaxChunkPayloadBytes);
    if (!bound) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    // 정상 경로에서 writer 버퍼가 자라지 않도록 미리 잡는다. 정의 덩어리는 정의 버퍼 전체가 한 번에
    // 비워져도 들어가는 크기다(덩어리 머리·압축 실패 여유 포함).
    const usize chunkOverhead = TraceFormat::ChunkHeaderBytes + TraceFormat::ChunkRawSizeBytes;
    const usize batchBytes =
        settings.definitionBufferBytes +
        (settings.definitionBufferBytes / TraceFormat::MaxChunkPayloadBytes + 2) * chunkOverhead;
    if (!staging.TryReserve(TraceFormat::MaxChunkPayloadBytes) ||
        !compressed.TryResize(bound.Value()) ||
        !outputBuffer.TryReserve(OutputFlushBytes + TraceFormat::MaxChunkPayloadBytes +
                                 chunkOverhead) ||
        !definitionBatch.TryReserve(batchBytes)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    auto compression = CompressionSettings::TryDefault(CompressionFormat::Lz4);
    if (!compression) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    auto created = Compressor::TryCreate(compression.Value(), *allocator);
    if (!created) {
        return Result<void>::Failure(created.Error() == ErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::DependencyFailed);
    }

    compressor = Move(created.Value());
    return Result<void>::Success();
}

bool TraceSessionState::HasActiveOutput() const noexcept {
    for (const TraceOutput& output : outputs) {
        if (output.id != 0 && !output.failed) {
            return true;
        }
    }

    return false;
}

// 원본 내용이 임계값을 넘으면 LZ4 raw block으로 압축해 본다. 줄지 않으면 원본 그대로 쓴다.
void TraceSessionState::AppendChunk(Array<uint8>& out, uint16 stream,
                                    Span<const uint8> payload) noexcept {
    uint8 header[TraceFormat::ChunkHeaderBytes + TraceFormat::ChunkRawSizeBytes];
    if (payload.Size() > TraceFormat::CompressThresholdBytes) {
        auto packed = compressor.TryCompress(payload, compressed.View());
        if (packed && packed.Value() + TraceFormat::ChunkRawSizeBytes < payload.Size()) {
            const usize stored = packed.Value() + TraceFormat::ChunkRawSizeBytes;
            StoreChunkHeader(header, static_cast<uint16>(stored | TraceFormat::ChunkCompressedBit),
                             stream);
            StoreLittleEndian<uint16>(header + TraceFormat::ChunkHeaderBytes,
                                      static_cast<uint16>(payload.Size()));
            if (!out.TryAppend(Span<const uint8>(header, sizeof(header))) ||
                !out.TryAppend(Span<const uint8>(compressed.Data(), packed.Value()))) {
                ++writerDroppedChunks;
            }

            return;
        }
    }

    StoreChunkHeader(header, static_cast<uint16>(payload.Size()), stream);
    if (!out.TryAppend(Span<const uint8>(header, TraceFormat::ChunkHeaderBytes)) ||
        !out.TryAppend(payload)) {
        ++writerDroppedChunks;
    }
}

// 같은 스트림의 byte를 staging에 모으고, 덩어리 하나가 차면 out에 낸다. 스트림을 바꾸기 전에
// FinishStream으로 남은 내용을 낸다.
void TraceSessionState::AppendStream(Array<uint8>& out, uint16 stream,
                                     Span<const uint8> bytes) noexcept {
    while (!bytes.IsEmpty()) {
        const usize space = TraceFormat::MaxChunkPayloadBytes - staging.Size();
        const usize take = bytes.Size() < space ? bytes.Size() : space;
        if (!staging.TryAppend(bytes.Left(take))) {
            ++writerDroppedChunks;
        }

        bytes = bytes.RightChop(take);
        if (staging.Size() == TraceFormat::MaxChunkPayloadBytes) {
            AppendChunk(out, stream, staging.View());
            staging.Clear();
        }
    }
}

void TraceSessionState::FinishStream(Array<uint8>& out, uint16 stream) noexcept {
    if (!staging.IsEmpty()) {
        AppendChunk(out, stream, staging.View());
        staging.Clear();
    }

    if (&out == &outputBuffer && outputBuffer.Size() >= OutputFlushBytes) {
        FlushOutputBuffer();
    }
}

bool TraceSessionState::WriteOutput(TraceOutput& output, Span<const uint8> bytes) noexcept {
    if (output.failed || bytes.IsEmpty()) {
        return !output.failed;
    }

    bool written;
    if (output.file != nullptr) {
        const platform::FileTransfer transfer = output.file->WriteAll(bytes.Data(), bytes.Size());
        written = transfer.status && transfer.bytes == bytes.Size();
    } else {
        written = static_cast<bool>(output.write(output.context, bytes));
    }

    if (!written) {
        output.failed = true;
        ++outputFailures;
        return false;
    }

    outputBytes += bytes.Size();
    return true;
}

void TraceSessionState::FlushOutputBuffer() noexcept {
    if (outputBuffer.IsEmpty()) {
        return;
    }

    for (TraceOutput& output : outputs) {
        if (output.id != 0) {
            WriteOutput(output, outputBuffer.View());
        }
    }

    outputBuffer.Clear();
}

// 새로 등록된 형의 스키마를 스키마 스트림으로 낸다. 출력이 없어도 낸 것으로 친다(붙을 때 표 전체를
// 받는다).
void TraceSessionState::EmitSchemas(bool hasOutputs) noexcept {
    platform::LockGuard lock(registryMutex);
    if (hasOutputs) {
        for (usize index = schemasEmitted; index < types.Size(); ++index) {
            const TraceTypeEntry& entry = types[index];
            if (entry.present) {
                AppendStream(outputBuffer, TraceFormat::SchemaStreamId,
                             schemaBytes.View().Mid(entry.schemaOffset, entry.schemaBytes));
            }
        }

        FinishStream(outputBuffer, TraceFormat::SchemaStreamId);
    }

    schemasEmitted = types.Size();
}

// 정의 버퍼를 비워 정의 스트림 덩어리를 만든다. 덩어리는 출력으로 보내고, 저장소 상한 안이면 사본을
// 저장소에 둔다(4.3절). 쓰는 중인 정의가 있어 끝까지 비우지 못하면 false다.
bool TraceSessionState::DrainDefinitions(bool hasOutputs) noexcept {
    definitionBatch.Clear();
    uint64 records = 0;
    const bool complete = definitions.Drain([&](Span<const uint8> record) {
        AppendStream(definitionBatch, TraceFormat::DefinitionStreamId, record);
        ++records;
    });

    // 정의 버퍼가 차서 버린 정의가 늘었으면 writer가 DefinitionLoss 기록을 낸다.
    const uint64 dropped = definitions.Dropped();
    if (dropped != definitionDropsReported) {
        uint8 fixed[TraceFormat::DefinitionLossFixedBytes];
        StoreLittleEndian<uint64>(fixed + TraceFormat::DefinitionLossTickOffset,
                                  platform::MonotonicTicks());
        StoreLittleEndian<uint64>(fixed + TraceFormat::DefinitionLossCountOffset, dropped);
        TraceRecordShape shape;
        shape.typeId = TraceFormat::DefinitionLossTypeId;
        shape.fixed = fixed;
        shape.fixedBytes = sizeof(fixed);
        uint8 record[sizeof(uint16) + sizeof(fixed)];
        EncodeTraceRecord(record, shape);
        AppendStream(definitionBatch, TraceFormat::DefinitionStreamId,
                     Span<const uint8>(record, sizeof(record)));
        ++records;
        definitionDropsReported = dropped;
    }

    FinishStream(definitionBatch, TraceFormat::DefinitionStreamId);
    if (definitionBatch.IsEmpty()) {
        return complete;
    }

    if (hasOutputs) {
        if (!outputBuffer.TryAppend(definitionBatch.View())) {
            ++writerDroppedChunks;
        }

        if (outputBuffer.Size() >= OutputFlushBytes) {
            FlushOutputBuffer();
        }
    }

    // 덩어리 묶음 단위로 저장해, 저장소에는 늘 온전한 기록만 남는다.
    const usize limit = settings.definitionStoreMaxBytes;
    if (definitionStore.Size() <= limit &&
        definitionBatch.Size() <= limit - definitionStore.Size() &&
        definitionStore.TryAppend(definitionBatch.View())) {
        return complete;
    }

    definitionStoreOverflowed = true;
    definitionRecordsNotStored += records;
    return complete;
}

// 스레드 하나의 공개된 기록을 target(이번 펌프 시작 때 읽은 published)까지 보낸다. 다 읽은 은퇴
// 블록은 풀로 돌려준다.
void TraceSessionState::DrainThread(TraceThreadState& thread, bool hasOutputs) noexcept {
    if (thread.done) {
        return;
    }

    if (thread.readBlock == nullptr) {
        thread.readBlock = AtomicLoad<MemoryOrder::Acquire>(&thread.firstBlock);
        if (thread.readBlock == nullptr) {
            // 블록을 한 번도 받지 못하고(모두 손실) 끝난 스레드다. 손실 기록이 첫 블록이 된다.
            if (AtomicLoad<MemoryOrder::Acquire>(&thread.exited) == 0 || !thread.lossActive ||
                !WriteThreadLoss(pool, thread)) {
                return;
            }

            thread.target = AtomicLoad<MemoryOrder::Relaxed>(&thread.published);
            thread.readBlock = AtomicLoad<MemoryOrder::Relaxed>(&thread.firstBlock);
        }
    }

    const uint16 stream = ThreadStreamId(thread.threadId);
    bool lossFlushed = false;
    for (;;) {
        TraceBlock* block = thread.readBlock;
        // next를 acquire로 읽으면 다음 블록의 startTotal도 보인다(생산자가 연결 전에 썼다).
        TraceBlock* next = AtomicLoad<MemoryOrder::Acquire>(&block->header.next);
        uint64 end = next != nullptr ? next->header.startTotal : thread.target;
        if (end > thread.target) {
            end = thread.target;
        }

        if (end > thread.consumed) {
            if (hasOutputs) {
                const usize offset = static_cast<usize>(thread.consumed - block->header.startTotal);
                AppendStream(outputBuffer, stream,
                             Span<const uint8>(block->payload + offset,
                                               static_cast<usize>(end - thread.consumed)));
            }

            thread.consumed = end;
        }

        if (next != nullptr && thread.consumed == next->header.startTotal) {
            pool.Free(block);
            thread.readBlock = next;
            continue;
        }

        // 손실 상태로 끝난 스레드는 손실 기록을 쓸 수 없으므로 writer가 대신 쓴다. 스레드는 끝났고
        // exited를 acquire로 읽었으므로 생산자 칸을 writer가 써도 겹치지 않는다. 이 스레드의 은퇴
        // 블록을 방금 돌려줬으니 새 블록을 받을 수 있는 경우가 많다.
        if (!lossFlushed && thread.consumed == thread.target &&
            AtomicLoad<MemoryOrder::Acquire>(&thread.exited) != 0 && thread.lossActive) {
            lossFlushed = true;
            if (WriteThreadLoss(pool, thread)) {
                thread.target = AtomicLoad<MemoryOrder::Relaxed>(&thread.published);
                continue;
            }
        }

        break;
    }

    FinishStream(outputBuffer, stream);

    // 끝난 스레드의 마지막 블록은 다음 블록이 생기지 않으므로 exited를 보고 회수한다. exited를
    // acquire로 읽은 뒤의 published가 마지막 값이다.
    if (AtomicLoad<MemoryOrder::Acquire>(&thread.exited) != 0 && !thread.lossActive &&
        thread.consumed == AtomicLoad<MemoryOrder::Acquire>(&thread.published) &&
        AtomicLoad<MemoryOrder::Acquire>(&thread.readBlock->header.next) == nullptr) {
        pool.Free(thread.readBlock);
        thread.readBlock = nullptr;
        thread.done = true;
    }
}

void TraceSessionState::Pump() noexcept {
    // 1) 스레드마다 공개 위치를 먼저 읽는다. 그 앞의 기록이 쓰는 스키마·정의는 이 시점 전에
    // 준비됐으므로
    //    2)·3)에서 함께 나간다.
    TraceThreadState* head = AtomicLoad<MemoryOrder::Acquire>(&threads);
    for (TraceThreadState* thread = head; thread != nullptr; thread = thread->next) {
        thread->target = AtomicLoad<MemoryOrder::Acquire>(&thread->published);
    }

    const bool hasOutputs = HasActiveOutput();
    EmitSchemas(hasOutputs);
    // 2) 정의를 끝까지 비우지 못했으면(다른 스레드가 쓰는 중) 그 정의보다 기록이 먼저 나가지 않도록
    //    스레드 기록을 다음 펌프로 미룬다.
    const bool definitionsComplete = DrainDefinitions(hasOutputs);
    if (definitionsComplete) {
        for (TraceThreadState* thread = head; thread != nullptr; thread = thread->next) {
            DrainThread(*thread, hasOutputs);
        }
    }

    FlushOutputBuffer();
}

void TraceSessionState::FinalFlush() noexcept {
    Pump();
    FlushPendingLoss();
    Pump();
}

void TraceSessionState::CloseOutput(TraceOutput& output, bool& closeFailed) noexcept {
    if (output.file != nullptr) {
        if (!output.file->Close()) {
            closeFailed = true;
        }

        output.file->~File();
        auto freed = allocator->Deallocate(output.file);
        (void)freed;
    }

    output = TraceOutput{};
}

void TraceSessionState::CloseOutputs() noexcept {
    FlushOutputBuffer();
    for (TraceOutput& output : outputs) {
        if (output.id != 0) {
            bool closeFailed = false;
            CloseOutput(output, closeFailed);
            if (closeFailed) {
                ++outputFailures;
            }
        }
    }
}

// 새 출력에 머리 → 지금까지 낸 스키마 전체 → 정의 저장소를 쓴다. 펌프 사이(writerMutex 아래)에서만
// 붙으므로 각 스레드 스트림은 기록 경계에서 시작한다.
Result<TraceOutputId> TraceSessionState::AttachOutput(TraceOutput output) noexcept {
    TraceOutput* slot = nullptr;
    for (TraceOutput& candidate : outputs) {
        if (candidate.id == 0) {
            slot = &candidate;
            break;
        }
    }

    bool closeFailed = false;
    if (slot == nullptr) {
        CloseOutput(output, closeFailed);
        return Result<TraceOutputId>::Failure(ErrorCode::CapacityExceeded);
    }

    uint8 header[TraceFormat::FileHeaderBytes] = {};
    platform::MemoryCopy(header, TraceFormat::Magic, sizeof(TraceFormat::Magic));
    StoreLittleEndian<uint16>(header + TraceFormat::HeaderVersionOffset, TraceFormat::Version);
    StoreLittleEndian<uint16>(header + TraceFormat::HeaderBytesOffset,
                              static_cast<uint16>(TraceFormat::FileHeaderBytes));
    StoreLittleEndian<uint64>(header + TraceFormat::HeaderBaseTickOffset, baseTick);
    StoreLittleEndian<uint64>(header + TraceFormat::HeaderTickFrequencyOffset, tickFrequency);
    StoreLittleEndian<int64>(header + TraceFormat::HeaderStartUtcOffset, startUtcTicks);
    StoreLittleEndian<uint64>(header + TraceFormat::HeaderSessionGuidHighOffset, guid.high);
    StoreLittleEndian<uint64>(header + TraceFormat::HeaderSessionGuidLowOffset, guid.low);
    platform::MemoryCopy(header + TraceFormat::HeaderBuildIdOffset, buildId.bytes,
                         TraceFormat::HeaderBuildIdBytes);
    // 이 번호보다 작은 순서 번호의 기록은 이 출력에 없을 수 있다(분석기가 "앞부분 없음"으로 본다).
    StoreLittleEndian<uint64>(header + TraceFormat::HeaderAttachOrderOffset,
                              AtomicLoad<MemoryOrder::Relaxed>(&nextOrder));
    const bool incomplete = definitionStoreOverflowed || definitions.Dropped() != 0;
    StoreLittleEndian<uint32>(header + TraceFormat::HeaderFlagsOffset,
                              incomplete ? TraceFormat::HeaderFlagDefinitionsIncomplete : 0u);

    bool written = WriteOutput(output, Span<const uint8>(header, sizeof(header)));
    if (written) {
        // 펌프 끝에 비웠으므로 outputBuffer는 비어 있다. 이 출력만을 위한 덩어리를 거기서 만든다.
        {
            platform::LockGuard lock(registryMutex);
            for (usize index = 0; index < schemasEmitted; ++index) {
                const TraceTypeEntry& entry = types[index];
                if (entry.present) {
                    AppendStream(outputBuffer, TraceFormat::SchemaStreamId,
                                 schemaBytes.View().Mid(entry.schemaOffset, entry.schemaBytes));
                }
            }

            if (!staging.IsEmpty()) {
                AppendChunk(outputBuffer, TraceFormat::SchemaStreamId, staging.View());
                staging.Clear();
            }
        }

        written =
            WriteOutput(output, outputBuffer.View()) && WriteOutput(output, definitionStore.View());
        outputBuffer.Clear();
    }

    if (!written) {
        CloseOutput(output, closeFailed);
        return Result<TraceOutputId>::Failure(ErrorCode::DependencyFailed);
    }

    output.id = nextOutputId++;
    *slot = output;
    return Result<TraceOutputId>::Success(TraceOutputId{output.id});
}

Result<void> TraceSessionState::DetachOutput(TraceOutputId id) noexcept {
    TraceOutput* found = nullptr;
    for (TraceOutput& output : outputs) {
        if (id.value != 0 && output.id == id.value) {
            found = &output;
        }
    }

    if (found == nullptr) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    Pump();
    bool closeFailed = false;
    CloseOutput(*found, closeFailed);
    return closeFailed ? Result<void>::Failure(ErrorCode::DependencyFailed)
                       : Result<void>::Success();
}

void RunTraceWriter(void* context) noexcept {
    auto& session = *static_cast<TraceSessionState*>(context);
    for (;;) {
        {
            platform::LockGuard lock(session.wakeMutex);
            if (!session.stopRequested) {
                // 시한 만료·깨움·spurious wake 모두 한 번 펌프하고 다시 기다린다.
                const platform::WaitResult waited =
                    session.wake.Wait(session.wakeMutex, session.settings.writerPeriodMs);
                (void)waited;
            }

            if (session.stopRequested) {
                break;
            }
        }

        platform::LockGuard lock(session.writerMutex);
        session.Pump();
    }

    // 종료 순서 3)·4): 마지막 비우기와 출력 닫기. 5) join은 Stop이 한다.
    platform::LockGuard lock(session.writerMutex);
    session.FinalFlush();
    session.CloseOutputs();
}

} // namespace rocks::detail
