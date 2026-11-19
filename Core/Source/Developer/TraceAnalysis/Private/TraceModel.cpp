#include <TraceAnalysis/TraceModel.h>

#include <Base/Endian.h>

namespace rocks {

namespace {

using Format = TraceStandardFormat;

// 표준 형의 스키마 id다. 형 id는 등록 순서로 정해지므로 이름으로 찾는다(없으면 InvalidTypeId).
struct TypeIds {
    uint16 channelInfo = TraceFormat::ChannelInfoTypeId;
    uint16 threadInfo = TraceFormat::ThreadInfoTypeId;
    uint16 loss = TraceFormat::LossTypeId;
    uint16 definitionLoss = TraceFormat::DefinitionLossTypeId;
    uint16 scopeName = TraceFormat::InvalidTypeId;
    uint16 scopeBatch = TraceFormat::InvalidTypeId;
    uint16 counterDefinition = TraceFormat::InvalidTypeId;
    uint16 counterInteger = TraceFormat::InvalidTypeId;
    uint16 counterFloat = TraceFormat::InvalidTypeId;
    uint16 mark = TraceFormat::InvalidTypeId;
    uint16 task = TraceFormat::InvalidTypeId;
    uint16 gpuQueue = TraceFormat::InvalidTypeId;
    uint16 gpuCalibration = TraceFormat::InvalidTypeId;
    uint16 gpuScope = TraceFormat::InvalidTypeId;
    uint16 gpuFrame = TraceFormat::InvalidTypeId;
    uint16 snapshotSync = TraceFormat::InvalidTypeId;
    uint16 stats = TraceFormat::InvalidTypeId;
    uint16 logSpec = TraceFormat::InvalidTypeId;
    uint16 logMessage = TraceFormat::InvalidTypeId;
};

uint16 FindType(const TraceStreamDecoder& decoder, const char* name) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(name));
    return schema != nullptr ? schema->typeId : TraceFormat::InvalidTypeId;
}

TypeIds ResolveTypes(const TraceStreamDecoder& decoder) noexcept {
    TypeIds ids;
    ids.scopeName = FindType(decoder, Format::ScopeNameType);
    ids.scopeBatch = FindType(decoder, Format::CpuScopeBatchType);
    ids.counterDefinition = FindType(decoder, Format::CounterDefinitionType);
    ids.counterInteger = FindType(decoder, Format::CounterIntegerType);
    ids.counterFloat = FindType(decoder, Format::CounterFloatType);
    ids.mark = FindType(decoder, Format::MarkType);
    ids.task = FindType(decoder, Format::TaskType);
    ids.gpuQueue = FindType(decoder, Format::GpuQueueType);
    ids.gpuCalibration = FindType(decoder, Format::GpuCalibrationType);
    ids.gpuScope = FindType(decoder, Format::GpuScopeType);
    ids.gpuFrame = FindType(decoder, Format::GpuFrameType);
    ids.snapshotSync = FindType(decoder, "Trace.SnapshotSync");
    ids.stats = FindType(decoder, Format::StatsType);
    ids.logSpec = FindType(decoder, Format::LogMessageSpecType);
    ids.logMessage = FindType(decoder, Format::LogMessageType);
    return ids;
}

StringView TextOf(Span<const uint8> bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

// 고정부가 offset + bytes를 덮는지다. 스키마가 다른(예: 옛) 기록을 잘못 읽지 않게 한다.
bool Covers(const DecodedRecord& record, usize offset, usize bytes) noexcept {
    return record.fixed.Size() >= offset + bytes;
}

} // namespace

// 순서 번호를 받은 기록(task·카운터)이다. 스레드 스트림들에서 모아 순서 번호로 병합한다.
struct TraceModel::OrderedEvent {
    uint64 order = 0;
    uint64 tick = 0;
    uint64 first = 0;
    uint64 second = 0;
    // 0 task 사건(kind = 사건 종류), 1 정수 카운터, 2 실수 카운터.
    uint8 source = 0;
    uint8 kind = 0;
    StringView text;
};

uint64 TraceTicksToNanoseconds(uint64 ticks, uint64 frequency) noexcept {
    constexpr uint64 NanosecondsPerSecond = 1000000000;
    if (frequency == 0) {
        return 0;
    }

    return ticks / frequency * NanosecondsPerSecond +
           ticks % frequency * NanosecondsPerSecond / frequency;
}

TraceModel::TraceModel(Allocator& allocator) noexcept
    : allocator_(&allocator), decoder_(allocator), threads_(allocator), tasks_(allocator),
      edges_(allocator), counters_(allocator), channelLosses_(allocator), gpuQueues_(allocator),
      gpuScopes_(allocator), gpuFrames_(allocator), logSpecs_(allocator), logs_(allocator),
      names_(allocator) {
}

const TraceModelLogSpec* TraceModel::FindLogSpec(uint64 specId) const noexcept {
    usize low = 0;
    usize high = logSpecs_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (logSpecs_[middle].specId < specId) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < logSpecs_.Size() && logSpecs_[low].specId == specId ? &logSpecs_[low] : nullptr;
}

StringView TraceModel::LogCategoryOf(const TraceModelLog& log) const noexcept {
    const TraceModelLogSpec* spec = FindLogSpec(log.specId);
    return spec != nullptr ? spec->category : log.inlineCategory;
}

StringView TraceModel::ScopeName(uint32 id) const noexcept {
    for (const NameEntry& entry : names_) {
        if (entry.id == id) {
            return entry.name;
        }
    }

    return {};
}

TraceModelThread* TraceModel::FindThread(uint32 threadId) noexcept {
    for (TraceModelThread& thread : threads_) {
        if (thread.threadId == threadId) {
            return &thread;
        }
    }

    auto added = threads_.TryEmplaceBack(*allocator_);
    if (!added) {
        return nullptr;
    }

    added.Value()->threadId = threadId;
    return added.Value();
}

TraceModelTask* TraceModel::FindOrAddTask(uint64 id) noexcept {
    for (TraceModelTask& task : tasks_) {
        if (task.id == id) {
            return &task;
        }
    }

    TraceModelTask task;
    task.id = id;
    if (!tasks_.TryPushBack(task)) {
        return nullptr;
    }

    return &tasks_[tasks_.Size() - 1];
}

void TraceModel::AddChannelLoss(uint16 channel, uint64 count) noexcept {
    for (TraceModelChannelLoss& entry : channelLosses_) {
        if (entry.channelId == channel) {
            entry.lost += count;
            return;
        }
    }

    TraceModelChannelLoss entry;
    entry.channelId = channel;
    entry.lost = count;
    // 채널별 표를 늘리지 못하면 그 채널 몫은 합계(summary)에만 남는다.
    auto added = channelLosses_.TryPushBack(entry);
    (void)added;
}

TraceModelGpuQueue* TraceModel::FindOrAddGpuQueue(uint16 id) noexcept {
    for (TraceModelGpuQueue& queue : gpuQueues_) {
        if (queue.id == id) {
            return &queue;
        }
    }

    TraceModelGpuQueue queue;
    queue.id = id;
    if (!gpuQueues_.TryPushBack(queue)) {
        return nullptr;
    }

    return &gpuQueues_[gpuQueues_.Size() - 1];
}

void TraceModel::AddCaptureStats(const DecodedRecord& record) noexcept {
    // 스레드 스트림을 차례로 읽으므로 표본은 tick 순이 아니다. 가장 이른·늦은 표본을 tick으로
    // 고른다.
    TraceModelCaptureStats& stats = captureStats_;
    const uint64 tick = ReadFixed<uint64>(record, Format::StatsTickOffset);
    const uint64 output = ReadFixed<uint64>(record, Format::StatsOutputBytesOffset);
    const uint64 peak = ReadFixed<uint64>(record, Format::StatsPoolPeakOffset);
    if (stats.samples == 0 || tick < stats.firstTick) {
        stats.firstTick = tick;
        stats.firstOutputBytes = output;
    }

    if (stats.samples == 0 || tick >= stats.lastTick) {
        stats.lastTick = tick;
        stats.lastOutputBytes = output;
        stats.poolBytesLimit = ReadFixed<uint64>(record, Format::StatsPoolLimitOffset);
        stats.definitionStoreBytes = ReadFixed<uint64>(record, Format::StatsDefinitionStoreOffset);
        stats.lostRecords = ReadFixed<uint64>(record, Format::StatsLostRecordsOffset);
        stats.definitionRecordsDropped =
            ReadFixed<uint64>(record, Format::StatsDefinitionsDroppedOffset);
        stats.recentDroppedBytes = ReadFixed<uint64>(record, Format::StatsRecentDroppedOffset);
    }

    stats.poolBytesPeak = peak > stats.poolBytesPeak ? peak : stats.poolBytesPeak;
    ++stats.samples;
}

Result<void> TraceModel::ReadDefinitions() noexcept {
    const TypeIds types = ResolveTypes(decoder_);
    Array<DecodedRecord> records(*allocator_);
    const TraceDecodeStatus status =
        decoder_.DecodeRecordsPartial(TraceFormat::DefinitionStreamId, records);
    if (status == TraceDecodeStatus::OutOfMemory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (status == TraceDecodeStatus::Truncated) {
        ++summary_.truncatedStreams;
    } else if (status == TraceDecodeStatus::UnknownType) {
        ++summary_.undecodableStreams;
    }

    for (const DecodedRecord& record : records) {
        if (record.typeId == types.threadInfo &&
            Covers(record, 0, TraceFormat::ThreadInfoFixedBytes)) {
            // 같은 스레드의 정보가 다시 나오면 나중 것이 이긴다.
            TraceModelThread* thread =
                FindThread(ReadFixed<uint32>(record, TraceFormat::ThreadInfoIdOffset));
            if (thread == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            thread->osThreadId = ReadFixed<uint64>(record, TraceFormat::ThreadInfoOsIdOffset);
            thread->sortOrder = static_cast<int32>(
                ReadFixed<uint32>(record, TraceFormat::ThreadInfoSortOrderOffset));
            if (record.variableCount >= 2) {
                thread->name = TextOf(record.variable[0]);
                thread->group = TextOf(record.variable[1]);
            }
        } else if (record.typeId == types.logSpec && record.variableCount == 4 &&
                   Covers(record, 0, Format::LogSpecFixedBytes)) {
            // 정의는 모은 뒤 spec id로 정렬하고 겹친 것은 처음 것만 남긴다(Build).
            {
                TraceModelLogSpec spec;
                spec.specId = ReadFixed<uint64>(record, Format::LogSpecIdOffset);
                spec.eventId = ReadFixed<uint64>(record, Format::LogSpecEventIdOffset);
                spec.line = ReadFixed<uint32>(record, Format::LogSpecLineOffset);
                spec.level = ReadFixedByte(record, Format::LogSpecLevelOffset);
                spec.category = TextOf(record.variable[0]);
                spec.templateText = TextOf(record.variable[1]);
                spec.file = TextOf(record.variable[2]);
                spec.function = TextOf(record.variable[3]);
                if (!logSpecs_.TryPushBack(spec)) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        } else if (record.typeId == types.scopeName && record.variableCount == 1 &&
                   Covers(record, 0, Format::ScopeNameFixedBytes)) {
            const NameEntry entry{ReadFixed<uint32>(record, Format::ScopeNameIdOffset),
                                  TextOf(record.variable[0])};
            if (!names_.TryPushBack(entry)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.snapshotSync &&
                   Covers(record, 0, TraceFormat::SnapshotSyncFixedBytes)) {
            summary_.recentDroppedChunks =
                ReadFixed<uint64>(record, TraceFormat::SnapshotSyncDroppedChunksOffset);
            summary_.recentDroppedBytes =
                ReadFixed<uint64>(record, TraceFormat::SnapshotSyncDroppedBytesOffset);
            summary_.snapshotSkippedChunks =
                ReadFixed<uint64>(record, TraceFormat::SnapshotSyncSkippedChunksOffset);
        } else if (record.typeId == types.counterDefinition && record.variableCount == 1 &&
                   Covers(record, 0, Format::CounterDefinitionFixedBytes)) {
            TraceModelCounter counter;
            counter.id = ReadFixed<uint32>(record, Format::CounterDefinitionIdOffset);
            counter.kind = ReadFixedByte(record, Format::CounterDefinitionKindOffset);
            counter.unit = ReadFixedByte(record, Format::CounterDefinitionUnitOffset);
            counter.name = TextOf(record.variable[0]);
            if (!counters_.TryPushBack(counter)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.channelInfo && record.variableCount >= 1 &&
                   Covers(record, 0, TraceFormat::ChannelInfoFixedBytes)) {
            const uint16 id = ReadFixed<uint16>(record, TraceFormat::ChannelInfoIdOffset);
            AddChannelLoss(id, 0);
            for (TraceModelChannelLoss& entry : channelLosses_) {
                if (entry.channelId == id) {
                    entry.name = TextOf(record.variable[0]);
                }
            }
        } else if (record.typeId == types.gpuQueue && record.variableCount == 1 &&
                   Covers(record, 0, Format::GpuQueueFixedBytes)) {
            // 세션마다 다시 쓰인 정의는 나중 것이 이긴다.
            TraceModelGpuQueue* queue =
                FindOrAddGpuQueue(ReadFixed<uint16>(record, Format::GpuQueueIdOffset));
            if (queue == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            queue->kind = ReadFixedByte(record, Format::GpuQueueKindOffset);
            queue->timing = ReadFixedByte(record, Format::GpuQueueTimingOffset) != 0;
            queue->name = TextOf(record.variable[0]);
        } else if (record.typeId == types.gpuCalibration &&
                   Covers(record, 0, Format::GpuCalibrationFixedBytes)) {
            TraceModelGpuQueue* queue =
                FindOrAddGpuQueue(ReadFixed<uint16>(record, Format::GpuCalibrationQueueOffset));
            if (queue == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            const uint64 uncertainty =
                ReadFixed<uint64>(record, Format::GpuCalibrationUncertaintyOffset);
            ++queue->calibrations;
            queue->maxUncertaintyNanoseconds = uncertainty > queue->maxUncertaintyNanoseconds
                                                   ? uncertainty
                                                   : queue->maxUncertaintyNanoseconds;
        } else if (record.typeId == types.definitionLoss &&
                   Covers(record, 0, TraceFormat::DefinitionLossFixedBytes)) {
            const uint64 dropped =
                ReadFixed<uint64>(record, TraceFormat::DefinitionLossCountOffset);
            summary_.definitionsDropped =
                dropped > summary_.definitionsDropped ? dropped : summary_.definitionsDropped;
        }
    }

    return Result<void>::Success();
}

Result<void> TraceModel::ReadThread(uint16 stream, Array<OrderedEvent>& ordered) noexcept {
    const TypeIds types = ResolveTypes(decoder_);
    TraceModelThread* thread =
        FindThread(static_cast<uint32>(stream - TraceFormat::FirstThreadStreamId + 1));
    if (thread == nullptr) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    Array<DecodedRecord> records(*allocator_);
    thread->status = decoder_.DecodeRecordsPartial(stream, records);
    if (thread->status == TraceDecodeStatus::OutOfMemory) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    if (thread->status == TraceDecodeStatus::Truncated) {
        ++summary_.truncatedStreams;
    } else if (thread->status == TraceDecodeStatus::UnknownType) {
        ++summary_.undecodableStreams;
    }

    // 열린 구간의 색인 스택이다. 손실 기록을 만나면 그 사이의 시작·끝을 알 수 없으므로 열린 구간을
    // 모두 "끝 없음"으로 닫는다(끝이 손실 뒤에 왔더라도 짝짓지 않는다 — 길이를 지어내지 않는다).
    constexpr usize MaxDepth = 256;
    usize stack[MaxDepth];
    usize depth = 0;
    auto closeOpen = [&]() {
        thread->unendedScopes += depth;
        depth = 0;
    };

    Array<DecodedScopeEvent> events(*allocator_);
    for (const DecodedRecord& record : records) {
        if (record.typeId == types.loss && record.variableCount == 2) {
            ++thread->lossRanges;
            const usize entries = record.variable[0].Size() / sizeof(uint16);
            for (usize index = 0;
                 index < entries && (index + 1) * sizeof(uint64) <= record.variable[1].Size();
                 ++index) {
                const uint64 count =
                    LoadLittleEndian<uint64>(record.variable[1].Data() + index * sizeof(uint64));
                thread->lostRecords += count;
                AddChannelLoss(
                    LoadLittleEndian<uint16>(record.variable[0].Data() + index * sizeof(uint16)),
                    count);
            }

            closeOpen();
        } else if (record.typeId == types.scopeBatch && record.variableCount == 1) {
            events.Clear();
            if (!DecodeScopeBatch(record.variable[0], events)) {
                // 배치가 손상됐다. 손실과 같이 열린 구간을 닫고 이 배치는 버린다.
                closeOpen();
                ++summary_.truncatedStreams;
                continue;
            }

            for (const DecodedScopeEvent& event : events) {
                if (event.begin) {
                    if (depth == MaxDepth) {
                        continue;
                    }

                    TraceModelScope scope;
                    scope.nameId = event.nameId;
                    scope.beginTick = event.tick;
                    scope.depth = static_cast<uint32>(depth);
                    stack[depth++] = thread->scopes.Size();
                    if (!thread->scopes.TryPushBack(scope)) {
                        return Result<void>::Failure(ErrorCode::OutOfMemory);
                    }
                } else if (depth == 0) {
                    ++thread->orphanEnds;
                } else {
                    TraceModelScope& scope = thread->scopes[stack[--depth]];
                    scope.endTick = event.tick;
                    scope.ended = true;
                }
            }
        } else if (record.typeId == types.task && record.hasOrder && record.variableCount == 1 &&
                   Covers(record, 0, Format::TaskFixedBytes)) {
            OrderedEvent event;
            event.order = record.order;
            event.tick = ReadFixed<uint64>(record, Format::TaskTickOffset);
            event.first = ReadFixed<uint64>(record, Format::TaskIdOffset);
            event.second = ReadFixed<uint64>(record, Format::TaskRelatedOffset);
            event.kind = ReadFixedByte(record, Format::TaskKindOffset);
            event.text = TextOf(record.variable[0]);
            if (!ordered.TryPushBack(event)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if ((record.typeId == types.counterInteger || record.typeId == types.counterFloat) &&
                   record.hasOrder && Covers(record, 0, Format::CounterValueFixedBytes)) {
            OrderedEvent event;
            event.order = record.order;
            event.tick = ReadFixed<uint64>(record, Format::CounterValueTickOffset);
            event.first = ReadFixed<uint32>(record, Format::CounterValueIdOffset);
            event.second = ReadFixed<uint64>(record, Format::CounterValueOffset);
            event.source = record.typeId == types.counterInteger ? 1 : 2;
            if (!ordered.TryPushBack(event)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.logMessage && record.hasOrder &&
                   record.variableCount == 3 && Covers(record, 0, Format::LogMessageFixedBytes)) {
            TraceModelLog log;
            log.order = record.order;
            log.tick = ReadFixed<uint64>(record, Format::LogMessageTickOffset);
            log.thread = ReadFixed<uint64>(record, Format::LogMessageThreadOffset);
            log.frame = ReadFixed<uint64>(record, Format::LogMessageFrameOffset);
            log.specId = ReadFixed<uint64>(record, Format::LogMessageSpecIdOffset);
            log.eventId = ReadFixed<uint64>(record, Format::LogMessageEventIdOffset);
            for (usize index = 0; index < Format::ContextValueCount; ++index) {
                log.correlation[index] = ReadFixed<uint64>(
                    record, Format::LogMessageCorrelationOffset + index * sizeof(uint64));
            }

            log.flags = ReadFixed<uint32>(record, Format::LogMessageFlagsOffset);
            log.level = ReadFixedByte(record, Format::LogMessageLevelOffset);
            const uint8 truncated = ReadFixedByte(record, Format::LogMessageTruncatedOffset);
            log.truncated = (truncated & Format::LogMessageTruncatedMessage) != 0;
            log.templateTruncated = (truncated & Format::LogMessageTruncatedTemplate) != 0;
            log.message = TextOf(record.variable[0]);
            log.inlineCategory = TextOf(record.variable[1]);
            log.inlineTemplate = TextOf(record.variable[2]);
            if (!logs_.TryPushBack(log)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.mark) {
            ++summary_.marks;
        } else if (record.typeId == types.gpuScope &&
                   Covers(record, 0, Format::GpuScopeFixedBytes)) {
            TraceModelGpuScope scope;
            scope.queue = ReadFixed<uint16>(record, Format::GpuScopeQueueOffset);
            scope.nameId = ReadFixed<uint32>(record, Format::GpuScopeNameOffset);
            scope.depth = ReadFixed<uint32>(record, Format::GpuScopeDepthOffset);
            scope.frame = ReadFixed<uint64>(record, Format::GpuScopeFrameOffset);
            scope.beginTick = ReadFixed<uint64>(record, Format::GpuScopeBeginOffset);
            scope.endTick = ReadFixed<uint64>(record, Format::GpuScopeEndOffset);
            if (!gpuScopes_.TryPushBack(scope) || FindOrAddGpuQueue(scope.queue) == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.gpuFrame &&
                   Covers(record, 0, Format::GpuFrameFixedBytes)) {
            TraceModelGpuFrame frame;
            frame.queue = ReadFixed<uint16>(record, Format::GpuFrameQueueOffset);
            frame.frame = ReadFixed<uint64>(record, Format::GpuFrameNumberOffset);
            frame.beginTick = ReadFixed<uint64>(record, Format::GpuFrameBeginOffset);
            frame.endTick = ReadFixed<uint64>(record, Format::GpuFrameEndOffset);
            if (!gpuFrames_.TryPushBack(frame) || FindOrAddGpuQueue(frame.queue) == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        } else if (record.typeId == types.stats && Covers(record, 0, Format::StatsFixedBytes)) {
            AddCaptureStats(record);
        }
    }

    // 스트림 끝(세션 종료·잘림)에서 열린 구간도 끝 없음이다.
    closeOpen();
    summary_.lostRecords += thread->lostRecords;
    summary_.lossRanges += thread->lossRanges;
    summary_.orphanScopeEnds += thread->orphanEnds;
    summary_.unendedScopes += thread->unendedScopes;
    return Result<void>::Success();
}

Result<void> TraceModel::MergeOrdered(Array<OrderedEvent>& ordered) noexcept {
    // 스레드 스트림들을 순서 번호로 합친다(UE TA Engine의 serial 병합과 같다).
    ordered.Sort([](const OrderedEvent& left, const OrderedEvent& right) noexcept {
        return left.order < right.order;
    });

    uint64 previous = 0;
    bool first = true;
    for (const OrderedEvent& event : ordered) {
        if (event.order >= summary_.attachOrderNumber) {
            const uint64 expected = first ? summary_.attachOrderNumber : previous + 1;
            if (event.order > expected) {
                summary_.orderGaps += event.order - expected;
            }

            previous = event.order;
            first = false;
        }

        if (event.source == 0) {
            // 기다림 사건의 기다리는 쪽이 task 밖(0)이면 task 수명이 아니다.
            if (event.first == 0) {
                continue;
            }

            TraceModelTask* task = FindOrAddTask(event.first);
            if (task == nullptr) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }

            switch (event.kind) {
            case Format::TaskCreated:
                task->createdTick = event.tick;
                break;
            case Format::TaskReady:
                task->readyTick = event.tick;
                break;
            case Format::TaskDependencyAdded:
                if (event.second != 0 &&
                    !edges_.TryPushBack(TraceModelEdge{event.first, event.second})) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }

                break;
            case Format::TaskStarted:
                task->startedTick = event.tick;
                break;
            case Format::TaskFinished:
                task->finishedTick = event.tick;
                break;
            case Format::TaskCompleted:
                task->completedTick = event.tick;
                task->completed = true;
                task->error = event.text;
                break;
            case Format::TaskCancelled:
                task->completedTick = event.tick;
                task->cancelled = true;
                break;
            default:
                // 기다림 사건은 task 밖 스레드(id 0)일 수 있다. 모델에는 task 수명만 둔다.
                break;
            }

            continue;
        }

        for (TraceModelCounter& counter : counters_) {
            if (counter.id == event.first) {
                ++counter.samples;
                counter.lastBits = event.second;
            }
        }
    }

    tasks_.Sort([](const TraceModelTask& left, const TraceModelTask& right) noexcept {
        return left.id < right.id;
    });
    edges_.Sort([](const TraceModelEdge& left, const TraceModelEdge& right) noexcept {
        return left.task != right.task ? left.task < right.task
                                       : left.prerequisite < right.prerequisite;
    });
    return Result<void>::Success();
}

Result<void> TraceModel::Build(Span<const uint8> bytes) noexcept {
    if (!decoder_.Decode(bytes)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const DecodedHeader& header = decoder_.Header();
    summary_.truncated = decoder_.Truncated();
    summary_.definitionsIncomplete =
        (header.flags & TraceFormat::HeaderFlagDefinitionsIncomplete) != 0;
    summary_.attachOrderNumber = header.attachOrderNumber;
    summary_.attachedMidSession = header.attachOrderNumber != 0;
    summary_.snapshot = (header.flags & TraceFormat::HeaderFlagSnapshot) != 0;
    summary_.snapshotIncomplete = (header.flags & TraceFormat::HeaderFlagSnapshotIncomplete) != 0;

    if (auto definitions = ReadDefinitions(); !definitions) {
        return definitions;
    }

    Array<OrderedEvent> ordered(*allocator_);
    for (const DecodedStream& stream : decoder_.Streams()) {
        if (stream.id < TraceFormat::FirstThreadStreamId) {
            continue;
        }

        if (auto read = ReadThread(stream.id, ordered); !read) {
            return read;
        }
    }

    if (auto merged = MergeOrdered(ordered); !merged) {
        return merged;
    }

    // 로그는 여러 스레드 스트림(로그 출력 스레드가 바뀔 수 있다)에서 모았으므로 순서 번호로 맞춘다.
    logs_.Sort([](const TraceModelLog& left, const TraceModelLog& right) noexcept {
        return left.order < right.order;
    });
    // spec은 spec id로 찾는다(이진 탐색). 같은 id(같은 호출 지점)의 정의는 내용이 같으므로 하나만
    // 남긴다.
    logSpecs_.Sort([](const TraceModelLogSpec& left, const TraceModelLogSpec& right) noexcept {
        return left.specId < right.specId;
    });
    usize kept = 0;
    for (usize index = 0; index < logSpecs_.Size(); ++index) {
        if (kept == 0 || logSpecs_[kept - 1].specId != logSpecs_[index].specId) {
            logSpecs_[kept++] = logSpecs_[index];
        }
    }

    (void)logSpecs_.TryResize(kept); // 축소는 실패하지 않는다

    // 이름 정의가 없는 구간 이름 id를 센다(같은 id는 한 번).
    Array<uint32> missing(*allocator_);
    for (const TraceModelThread& thread : threads_) {
        for (const TraceModelScope& scope : thread.scopes) {
            if (ScopeName(scope.nameId).IsEmpty() && !missing.Contains(scope.nameId) &&
                !missing.TryPushBack(scope.nameId)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    for (const TraceModelGpuScope& scope : gpuScopes_) {
        if (ScopeName(scope.nameId).IsEmpty() && !missing.Contains(scope.nameId) &&
            !missing.TryPushBack(scope.nameId)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    summary_.missingScopeNames = missing.Size();
    gpuQueues_.Sort([](const TraceModelGpuQueue& left, const TraceModelGpuQueue& right) noexcept {
        return left.id < right.id;
    });
    gpuScopes_.Sort([](const TraceModelGpuScope& left, const TraceModelGpuScope& right) noexcept {
        if (left.queue != right.queue) {
            return left.queue < right.queue;
        }

        return left.beginTick != right.beginTick ? left.beginTick < right.beginTick
                                                 : left.depth < right.depth;
    });
    gpuFrames_.Sort([](const TraceModelGpuFrame& left, const TraceModelGpuFrame& right) noexcept {
        return left.queue != right.queue ? left.queue < right.queue : left.frame < right.frame;
    });
    threads_.Sort([](const TraceModelThread& left, const TraceModelThread& right) noexcept {
        return left.threadId < right.threadId;
    });
    counters_.Sort([](const TraceModelCounter& left, const TraceModelCounter& right) noexcept {
        return left.id < right.id;
    });
    channelLosses_.Sort(
        [](const TraceModelChannelLoss& left, const TraceModelChannelLoss& right) noexcept {
            return left.channelId < right.channelId;
        });
    return Result<void>::Success();
}

Result<void> TraceModel::CriticalPath(Array<uint64>& out) const noexcept {
    const TraceModelTask* current = nullptr;
    for (const TraceModelTask& task : tasks_) {
        if (task.completedTick != 0 &&
            (current == nullptr || task.completedTick > current->completedTick)) {
            current = &task;
        }
    }

    Array<uint64> reversed(*allocator_);
    while (current != nullptr) {
        if (reversed.Contains(current->id) || !reversed.TryPushBack(current->id)) {
            break;
        }

        const TraceModelTask* next = nullptr;
        for (const TraceModelEdge& edge : edges_) {
            if (edge.task != current->id) {
                continue;
            }

            for (const TraceModelTask& task : tasks_) {
                if (task.id == edge.prerequisite && task.completedTick != 0 &&
                    (next == nullptr || task.completedTick > next->completedTick)) {
                    next = &task;
                }
            }
        }

        current = next;
    }

    for (usize index = reversed.Size(); index > 0; --index) {
        if (!out.TryPushBack(reversed[index - 1])) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
