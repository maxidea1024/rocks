#include "TraceTestWriter.h"
#include <Base/Endian.h>

#include <TestSupport/Test.h>
#include <Core/Diagnostics/Correlation.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogSystem.h>
#include <TraceAnalysis/TraceModel.h>
#include <TraceAnalysis/TraceReport.h>
#include <TraceCapture/Tests/TraceCaptureTestSupport.h>
#include <TraceCapture/TraceCounter.h>
#include <TraceCapture/TraceContext.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceLogSink.h>
#include <TraceCapture/TraceTask.h>

// TR4 분석 시험이다(Docs/Design/TraceSystem.ko.md 10절). 입력은 tick·순서·형 id를 정한 writer로
// 만들어 golden report와 byte 단위로 비교하고, 실제 세션 capture로 결정성과 손상 입력(OB03)을
// 확인한다.
using namespace rocks;
namespace trace_analysis_tests_file {

using namespace rocks::tracetest;

namespace {

using Format = TraceStandardFormat;

// 표준 형 스키마를 쓴다. base부터 형 id를 매기고, reversed면 등록 순서를 뒤집는다(두 빌드가 다른
// 형 id를 매겨도 이름으로 같은 스키마를 해석하는지, OB06).
struct StandardIds {
    uint16 scopeName;
    uint16 scopeBatch;
    uint16 task;
    uint16 counterDefinition;
    uint16 counterInteger;
    uint16 mark;
    uint16 gpuQueue;
    uint16 gpuCalibration;
    uint16 gpuScope;
    uint16 gpuFrame;
};

StandardIds WriteSchemas(TraceTestWriter& writer, uint16 base, bool reversed) noexcept {
    constexpr TestField ChannelInfo[] = {
        {"channel", TraceFieldType::UInt16, TraceFormat::ChannelInfoIdOffset},
        {"readOnly", TraceFieldType::Bool, TraceFormat::ChannelInfoReadOnlyOffset},
        {"enabled", TraceFieldType::Bool, TraceFormat::ChannelInfoEnabledOffset},
        {"name", TraceFieldType::String, TraceFormat::VariableFieldOffset},
        {"description", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField ThreadInfo[] = {
        {"thread", TraceFieldType::UInt32, TraceFormat::ThreadInfoIdOffset},
        {"osThread", TraceFieldType::UInt64, TraceFormat::ThreadInfoOsIdOffset},
        {"sortOrder", TraceFieldType::Int32, TraceFormat::ThreadInfoSortOrderOffset},
        {"name", TraceFieldType::String, TraceFormat::VariableFieldOffset},
        {"group", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField Loss[] = {
        {"startTick", TraceFieldType::UInt64, TraceFormat::LossStartTickOffset},
        {"endTick", TraceFieldType::UInt64, TraceFormat::LossEndTickOffset},
        {"channels", TraceFieldType::UInt16Array, TraceFormat::VariableFieldOffset},
        {"counts", TraceFieldType::UInt64Array, TraceFormat::VariableFieldOffset}};
    constexpr TestField ScopeName[] = {
        {"id", TraceFieldType::UInt32, Format::ScopeNameIdOffset},
        {"truncated", TraceFieldType::Bool, Format::ScopeNameTruncatedOffset},
        {"name", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField Batch[] = {
        {"events", TraceFieldType::UInt8Array, TraceFormat::VariableFieldOffset}};
    constexpr TestField Task[] = {
        {"tick", TraceFieldType::UInt64, Format::TaskTickOffset},
        {"task", TraceFieldType::Id, Format::TaskIdOffset},
        {"related", TraceFieldType::Id, Format::TaskRelatedOffset},
        {"kind", TraceFieldType::UInt8, Format::TaskKindOffset},
        {"error", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField CounterDefinition[] = {
        {"id", TraceFieldType::UInt32, Format::CounterDefinitionIdOffset},
        {"kind", TraceFieldType::UInt8, Format::CounterDefinitionKindOffset},
        {"unit", TraceFieldType::UInt8, Format::CounterDefinitionUnitOffset},
        {"mode", TraceFieldType::UInt8, Format::CounterDefinitionModeOffset},
        {"name", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField CounterInteger[] = {
        {"tick", TraceFieldType::UInt64, Format::CounterValueTickOffset},
        {"id", TraceFieldType::UInt32, Format::CounterValueIdOffset},
        {"value", TraceFieldType::Int64, Format::CounterValueOffset}};
    constexpr TestField Mark[] = {
        {"tick", TraceFieldType::UInt64, Format::MarkTickOffset},
        {"site", TraceFieldType::UInt32, Format::MarkSiteOffset},
        {"arguments", TraceFieldType::UInt8Array, TraceFormat::VariableFieldOffset}};
    constexpr TestField GpuQueue[] = {
        {"queue", TraceFieldType::UInt16, Format::GpuQueueIdOffset},
        {"kind", TraceFieldType::UInt8, Format::GpuQueueKindOffset},
        {"timing", TraceFieldType::Bool, Format::GpuQueueTimingOffset},
        {"name", TraceFieldType::String, TraceFormat::VariableFieldOffset}};
    constexpr TestField GpuCalibration[] = {
        {"queue", TraceFieldType::UInt16, Format::GpuCalibrationQueueOffset},
        {"gpuTicks", TraceFieldType::UInt64, Format::GpuCalibrationGpuTicksOffset},
        {"cpuTick", TraceFieldType::UInt64, Format::GpuCalibrationCpuTickOffset},
        {"gpuFrequency", TraceFieldType::UInt64, Format::GpuCalibrationFrequencyOffset},
        {"uncertaintyNs", TraceFieldType::UInt64, Format::GpuCalibrationUncertaintyOffset}};
    constexpr TestField GpuScope[] = {
        {"queue", TraceFieldType::UInt16, Format::GpuScopeQueueOffset},
        {"name", TraceFieldType::UInt32, Format::GpuScopeNameOffset},
        {"depth", TraceFieldType::UInt32, Format::GpuScopeDepthOffset},
        {"frame", TraceFieldType::UInt64, Format::GpuScopeFrameOffset},
        {"begin", TraceFieldType::UInt64, Format::GpuScopeBeginOffset},
        {"end", TraceFieldType::UInt64, Format::GpuScopeEndOffset}};
    constexpr TestField GpuFrame[] = {
        {"queue", TraceFieldType::UInt16, Format::GpuFrameQueueOffset},
        {"frame", TraceFieldType::UInt64, Format::GpuFrameNumberOffset},
        {"begin", TraceFieldType::UInt64, Format::GpuFrameBeginOffset},
        {"end", TraceFieldType::UInt64, Format::GpuFrameEndOffset}};

    writer.Schema(TraceFormat::ChannelInfoTypeId, "Trace.ChannelInfo", TraceFormat::NoChannelId,
                  TraceFormat::TypeFlagDefinition, TraceFormat::ChannelInfoFixedBytes, ChannelInfo);
    writer.Schema(TraceFormat::ThreadInfoTypeId, "Trace.ThreadInfo", TraceFormat::NoChannelId,
                  TraceFormat::TypeFlagDefinition, TraceFormat::ThreadInfoFixedBytes, ThreadInfo);
    writer.Schema(TraceFormat::LossTypeId, "Trace.Loss", TraceFormat::NoChannelId, 0,
                  TraceFormat::LossFixedBytes, Loss);

    constexpr uint16 SlotCount = 10;
    uint16 slots[SlotCount];
    for (uint16 index = 0; index < SlotCount; ++index) {
        slots[index] = static_cast<uint16>(reversed ? base + SlotCount - 1 - index : base + index);
    }

    const StandardIds ids = {slots[0], slots[1], slots[2], slots[3], slots[4],
                             slots[5], slots[6], slots[7], slots[8], slots[9]};
    writer.Schema(ids.scopeName, Format::ScopeNameType, 0, TraceFormat::TypeFlagDefinition,
                  Format::ScopeNameFixedBytes, ScopeName);
    writer.Schema(ids.scopeBatch, Format::CpuScopeBatchType, 0, 0, 0, Batch);
    writer.Schema(ids.task, Format::TaskType, 1, TraceFormat::TypeFlagOrdered,
                  Format::TaskFixedBytes, Task);
    writer.Schema(ids.counterDefinition, Format::CounterDefinitionType, 2,
                  TraceFormat::TypeFlagDefinition, Format::CounterDefinitionFixedBytes,
                  CounterDefinition);
    writer.Schema(ids.counterInteger, Format::CounterIntegerType, 2, TraceFormat::TypeFlagOrdered,
                  Format::CounterValueFixedBytes, CounterInteger);
    writer.Schema(ids.mark, Format::MarkType, 3, 0, Format::MarkFixedBytes, Mark);
    writer.Schema(ids.gpuQueue, Format::GpuQueueType, 4, TraceFormat::TypeFlagDefinition,
                  Format::GpuQueueFixedBytes, GpuQueue);
    writer.Schema(ids.gpuCalibration, Format::GpuCalibrationType, 4,
                  TraceFormat::TypeFlagDefinition, Format::GpuCalibrationFixedBytes,
                  GpuCalibration);
    writer.Schema(ids.gpuScope, Format::GpuScopeType, 4, 0, Format::GpuScopeFixedBytes, GpuScope);
    writer.Schema(ids.gpuFrame, Format::GpuFrameType, 4, 0, Format::GpuFrameFixedBytes, GpuFrame);
    return ids;
}

Span<const uint8> Text(const char* text) noexcept {
    return Span<const uint8>(reinterpret_cast<const uint8*>(text), platform::CStringLength(text));
}

// CPU 구간 배치 내용을 만든다(varint, 배치 첫 사건은 절대 tick).
struct BatchBuilder {
    uint8 bytes[Format::ScopeBatchBytes] = {};
    usize size = 0;
    uint64 last = 0;

    void Varint(uint64 value) noexcept {
        while (value >= 0x80) {
            bytes[size++] = static_cast<uint8>(value | 0x80);
            value >>= 7;
        }

        bytes[size++] = static_cast<uint8>(value);
    }

    BatchBuilder& Begin(uint64 tick, uint32 name) noexcept {
        Varint(((tick - last) << Format::ScopeTagBits) | Format::ScopeBeginTag);
        Varint(name);
        last = tick;
        return *this;
    }

    BatchBuilder& End(uint64 tick) noexcept {
        Varint((tick - last) << Format::ScopeTagBits);
        last = tick;
        return *this;
    }
};

void WriteBatch(TraceTestWriter& writer, uint16 stream, uint16 type,
                const BatchBuilder& batch) noexcept {
    const Span<const uint8> parts[] = {Span<const uint8>(batch.bytes, batch.size)};
    writer.Record(stream, type, {}, parts);
}

void WriteLoss(TraceTestWriter& writer, uint16 stream, uint64 start, uint64 end, uint16 channel,
               uint64 count) noexcept {
    uint8 channels[2];
    uint8 counts[8];
    StoreLittleEndian<uint16>(channels, channel);
    StoreLittleEndian<uint64>(counts, count);
    const Span<const uint8> parts[] = {Span<const uint8>(channels, 2),
                                       Span<const uint8>(counts, 8)};
    const auto fixed = FixedBytes<TraceFormat::LossFixedBytes>()
                           .U64(TraceFormat::LossStartTickOffset, start)
                           .U64(TraceFormat::LossEndTickOffset, end);
    writer.Record(stream, TraceFormat::LossTypeId, fixed.View(), parts);
}

void WriteTask(TraceTestWriter& writer, const StandardIds& ids, uint16 stream, uint64 order,
               uint64 tick, uint64 task, uint8 kind, uint64 related = 0,
               const char* error = "") noexcept {
    const auto fixed = FixedBytes<Format::TaskFixedBytes>()
                           .U64(Format::TaskTickOffset, tick)
                           .U64(Format::TaskIdOffset, task)
                           .U64(Format::TaskRelatedOffset, related)
                           .U8(Format::TaskKindOffset, kind);
    const Span<const uint8> parts[] = {Text(error)};
    writer.Record(stream, ids.task, fixed.View(), parts, order);
}

void WriteName(TraceTestWriter& writer, const StandardIds& ids, uint32 id,
               const char* name) noexcept {
    const auto fixed = FixedBytes<Format::ScopeNameFixedBytes>().U32(Format::ScopeNameIdOffset, id);
    const Span<const uint8> parts[] = {Text(name)};
    writer.Record(TraceFormat::DefinitionStreamId, ids.scopeName, fixed.View(), parts);
}

// GPU queue 정의를 쓴다.
void WriteGpuQueue(TraceTestWriter& writer, const StandardIds& ids, uint16 queue, uint8 kind,
                   bool timing, const char* name) noexcept {
    const auto fixed = FixedBytes<Format::GpuQueueFixedBytes>()
                           .U16(Format::GpuQueueIdOffset, queue)
                           .U8(Format::GpuQueueKindOffset, kind)
                           .U8(Format::GpuQueueTimingOffset, timing ? 1 : 0);
    const Span<const uint8> parts[] = {Text(name)};
    writer.Record(TraceFormat::DefinitionStreamId, ids.gpuQueue, fixed.View(), parts);
}

void WriteGpuCalibration(TraceTestWriter& writer, const StandardIds& ids, uint16 queue,
                         uint64 cpuTick, uint64 uncertainty) noexcept {
    const auto fixed = FixedBytes<Format::GpuCalibrationFixedBytes>()
                           .U16(Format::GpuCalibrationQueueOffset, queue)
                           .U64(Format::GpuCalibrationGpuTicksOffset, cpuTick * 1000)
                           .U64(Format::GpuCalibrationCpuTickOffset, cpuTick)
                           .U64(Format::GpuCalibrationFrequencyOffset, 1000000000)
                           .U64(Format::GpuCalibrationUncertaintyOffset, uncertainty);
    writer.Record(TraceFormat::DefinitionStreamId, ids.gpuCalibration, fixed.View(), {});
}

void WriteGpuScope(TraceTestWriter& writer, const StandardIds& ids, uint16 stream, uint32 name,
                   uint32 depth, uint64 frame, uint64 begin, uint64 end) noexcept {
    const auto fixed = FixedBytes<Format::GpuScopeFixedBytes>()
                           .U16(Format::GpuScopeQueueOffset, 0)
                           .U32(Format::GpuScopeNameOffset, name)
                           .U32(Format::GpuScopeDepthOffset, depth)
                           .U64(Format::GpuScopeFrameOffset, frame)
                           .U64(Format::GpuScopeBeginOffset, begin)
                           .U64(Format::GpuScopeEndOffset, end);
    writer.Record(stream, ids.gpuScope, fixed.View(), {});
}

void WriteGpuFrame(TraceTestWriter& writer, const StandardIds& ids, uint16 stream, uint64 frame,
                   uint64 begin, uint64 end) noexcept {
    const auto fixed = FixedBytes<Format::GpuFrameFixedBytes>()
                           .U16(Format::GpuFrameQueueOffset, 0)
                           .U64(Format::GpuFrameNumberOffset, frame)
                           .U64(Format::GpuFrameBeginOffset, begin)
                           .U64(Format::GpuFrameEndOffset, end);
    writer.Record(stream, ids.gpuFrame, fixed.View(), {});
}

// golden 입력이다. 1 tick = 1µs(주파수 10^6).
// - 스레드 1 "Main": Frame[1000, 2000]{Update 300, Update 200}, Frame[3000, 4000]{Update 100},
//   손실(cpu 5개), 끝 없는 Frame(스트림 끝), 표지 2개, 카운터 100 → 300(순서 번호 8이 빠짐).
// - 스레드 2 "Worker.0": 시작 없는 끝 1개, Job 200·50, 손실(task 2개)로 끝을 잃은 Job, Job 100.
// - task: 1 → 2, 1 → 3(실패), 4(취소). 임계 경로 1 → 2.
// - GPU queue 0 "Graphics"(GPU 시간 있음, calibration 2개, 오차 250·400ns): 프레임 1 [1000, 1800],
//   프레임 2 [3000, 3500], 구간 GpuPass 300·100, 중첩 Update 100. queue 1 "Compute"는 GPU 시간
//   없음(구간 없음).
Array<uint8> BuildGoldenInput(Allocator& allocator, uint16 base, bool reversed) noexcept {
    TraceTestWriter writer(allocator, 1000000);
    const StandardIds ids = WriteSchemas(writer, base, reversed);
    constexpr uint16 Definitions = TraceFormat::DefinitionStreamId;
    constexpr uint16 Main = TraceFormat::FirstThreadStreamId;
    constexpr uint16 Worker = TraceFormat::FirstThreadStreamId + 1;

    const char* channelNames[] = {"cpu", "task"};
    for (uint16 channel = 0; channel < 2; ++channel) {
        const auto fixed = FixedBytes<TraceFormat::ChannelInfoFixedBytes>().U16(
            TraceFormat::ChannelInfoIdOffset, channel);
        const Span<const uint8> parts[] = {Text(channelNames[channel]), Text("")};
        writer.Record(Definitions, TraceFormat::ChannelInfoTypeId, fixed.View(), parts);
    }

    const char* threadNames[] = {"Main", "Worker.0"};
    const char* groups[] = {"Game", "Workers"};
    for (uint32 thread = 1; thread <= 2; ++thread) {
        const auto fixed = FixedBytes<TraceFormat::ThreadInfoFixedBytes>()
                               .U32(TraceFormat::ThreadInfoIdOffset, thread)
                               .U64(TraceFormat::ThreadInfoOsIdOffset, thread * 100)
                               .U32(TraceFormat::ThreadInfoSortOrderOffset, (thread - 1) * 10);
        const Span<const uint8> parts[] = {Text(threadNames[thread - 1]), Text(groups[thread - 1])};
        writer.Record(Definitions, TraceFormat::ThreadInfoTypeId, fixed.View(), parts);
    }

    WriteName(writer, ids, 1, "Frame");
    WriteName(writer, ids, 2, "Update");
    WriteName(writer, ids, 3, "Job");
    WriteName(writer, ids, 4, "GpuPass");
    WriteGpuQueue(writer, ids, 0, 0, true, "Graphics");
    WriteGpuQueue(writer, ids, 1, 1, false, "Compute");
    WriteGpuCalibration(writer, ids, 0, 900, 250);
    WriteGpuCalibration(writer, ids, 0, 2900, 400);
    {
        const auto fixed = FixedBytes<Format::CounterDefinitionFixedBytes>()
                               .U32(Format::CounterDefinitionIdOffset, 1)
                               .U8(Format::CounterDefinitionUnitOffset, 1);
        const Span<const uint8> parts[] = {Text("memory.used")};
        writer.Record(Definitions, ids.counterDefinition, fixed.View(), parts);
    }

    WriteBatch(
        writer, Main, ids.scopeBatch,
        BatchBuilder().Begin(1000, 1).Begin(1100, 2).End(1400).Begin(1500, 2).End(1700).End(2000));
    WriteBatch(writer, Main, ids.scopeBatch,
               BatchBuilder().Begin(3000, 1).Begin(3100, 2).End(3200).End(4000));
    WriteLoss(writer, Main, 4100, 4200, 0, 5);
    WriteBatch(writer, Main, ids.scopeBatch, BatchBuilder().Begin(4300, 1));
    for (int mark = 0; mark < 2; ++mark) {
        const auto fixed = FixedBytes<Format::MarkFixedBytes>().U64(Format::MarkTickOffset, 4400);
        const Span<const uint8> parts[] = {Span<const uint8>()};
        writer.Record(Main, ids.mark, fixed.View(), parts);
    }

    WriteTask(writer, ids, Main, 0, 10, 1, Format::TaskCreated);
    WriteTask(writer, ids, Main, 1, 11, 1, Format::TaskReady);
    WriteTask(writer, ids, Main, 2, 12, 2, Format::TaskCreated);
    WriteTask(writer, ids, Main, 3, 13, 2, Format::TaskDependencyAdded, 1);
    WriteTask(writer, ids, Main, 4, 14, 3, Format::TaskCreated);
    WriteTask(writer, ids, Main, 5, 15, 3, Format::TaskDependencyAdded, 1);
    WriteTask(writer, ids, Main, 6, 16, 4, Format::TaskCreated);
    const uint64 counterValues[] = {100, 300};
    for (const uint64 value : counterValues) {
        const auto fixed = FixedBytes<Format::CounterValueFixedBytes>()
                               .U64(Format::CounterValueTickOffset, value / 5)
                               .U32(Format::CounterValueIdOffset, 1)
                               .U64(Format::CounterValueOffset, value);
        writer.Record(Main, ids.counterInteger, fixed.View(), {}, value == 100 ? 7 : 9);
    }

    WriteBatch(writer, Worker, ids.scopeBatch,
               BatchBuilder().End(50).Begin(100, 3).End(300).Begin(400, 3).End(450));
    WriteBatch(writer, Worker, ids.scopeBatch, BatchBuilder().Begin(500, 3));
    WriteLoss(writer, Worker, 550, 560, 1, 2);
    WriteBatch(writer, Worker, ids.scopeBatch, BatchBuilder().Begin(600, 3).End(700));
    WriteTask(writer, ids, Worker, 10, 100, 1, Format::TaskStarted);
    WriteTask(writer, ids, Worker, 11, 300, 1, Format::TaskFinished);
    WriteTask(writer, ids, Worker, 12, 310, 1, Format::TaskCompleted);
    WriteTask(writer, ids, Worker, 13, 311, 2, Format::TaskReady);
    WriteTask(writer, ids, Worker, 14, 400, 2, Format::TaskStarted);
    WriteTask(writer, ids, Worker, 15, 600, 2, Format::TaskFinished);
    WriteTask(writer, ids, Worker, 16, 610, 2, Format::TaskCompleted);
    WriteTask(writer, ids, Worker, 17, 312, 3, Format::TaskReady);
    WriteTask(writer, ids, Worker, 18, 320, 3, Format::TaskStarted);
    WriteTask(writer, ids, Worker, 19, 330, 3, Format::TaskFinished);
    WriteTask(writer, ids, Worker, 20, 340, 3, Format::TaskCompleted, 0, "InvalidArgument");
    WriteTask(writer, ids, Worker, 21, 350, 4, Format::TaskCancelled);
    // GPU 기록은 완료 통지를 받은 스레드(여기서는 Main)의 스트림에 있다. 순서와 무관하게 시작 tick
    // 순으로 모은다.
    WriteGpuScope(writer, ids, Main, 2, 1, 1, 1150, 1250);
    WriteGpuScope(writer, ids, Main, 4, 0, 1, 1100, 1400);
    WriteGpuFrame(writer, ids, Main, 1, 1000, 1800);
    WriteGpuScope(writer, ids, Main, 4, 0, 2, 3100, 3200);
    WriteGpuFrame(writer, ids, Main, 2, 3000, 3500);
    return writer.Finish();
}

bool Report(Allocator& allocator, Span<const uint8> bytes, Array<uint8>& json) noexcept {
    TraceModel model(allocator);
    return model.Build(bytes) && WriteTraceReportJson(model, json);
}

StringView View(const Array<uint8>& bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

constexpr const char* GoldenReport = R"({
  "format": "rocks-trace-report",
  "version": 4,
  "session": {
    "guidHigh": "0x0000000000001234",
    "guidLow": "0x0000000000005678",
    "tickFrequency": 1000000
  },
  "summary": {
    "truncated": false,
    "definitionsIncomplete": false,
    "attachedMidSession": false,
    "attachOrderNumber": 0,
    "lostRecords": 7,
    "lossRanges": 2,
    "definitionsDropped": 0,
    "missingScopeNames": 0,
    "truncatedStreams": 0,
    "undecodableStreams": 0,
    "orphanScopeEnds": 1,
    "unendedScopes": 2,
    "orderGaps": 1,
    "marks": 2,
    "snapshot": false,
    "snapshotIncomplete": false,
    "recentDroppedChunks": 0,
    "recentDroppedBytes": 0,
    "snapshotSkippedChunks": 0,
    "channelLoss": [
      {
        "channel": 0,
        "name": "cpu",
        "lost": 5
      },
      {
        "channel": 1,
        "name": "task",
        "lost": 2
      }
    ]
  },
  "threads": [
    {
      "id": 1,
      "name": "Main",
      "group": "Game",
      "sortOrder": 0,
      "lostRecords": 5,
      "orphanEnds": 0,
      "complete": true,
      "scopes": [
        {
          "name": "Frame",
          "count": 2,
          "unended": 1,
          "totalNs": 2000000,
          "p50Ns": 1000000,
          "p95Ns": 1000000,
          "p99Ns": 1000000
        },
        {
          "name": "Update",
          "count": 3,
          "unended": 0,
          "totalNs": 600000,
          "p50Ns": 200000,
          "p95Ns": 300000,
          "p99Ns": 300000
        }
      ]
    },
    {
      "id": 2,
      "name": "Worker.0",
      "group": "Workers",
      "sortOrder": 10,
      "lostRecords": 2,
      "orphanEnds": 1,
      "complete": true,
      "scopes": [
        {
          "name": "Job",
          "count": 3,
          "unended": 1,
          "totalNs": 350000,
          "p50Ns": 100000,
          "p95Ns": 200000,
          "p99Ns": 200000
        }
      ]
    }
  ],
  "tasks": {
    "count": 4,
    "executed": 3,
    "failed": 1,
    "cancelled": 1,
    "edges": 2,
    "criticalPath": {
      "tasks": [
        1,
        2
      ],
      "executionNs": 400000,
      "spanNs": 600000
    }
  },
  "counters": [
    {
      "name": "memory.used",
      "kind": 0,
      "unit": 1,
      "samples": 2,
      "last": 300
    }
  ],
  "gpu": [
    {
      "id": 0,
      "name": "Graphics",
      "kind": 0,
      "timing": true,
      "calibrations": 2,
      "maxUncertaintyNs": 400,
      "frames": {
        "count": 2,
        "totalNs": 1300000,
        "p50Ns": 500000,
        "p95Ns": 800000,
        "p99Ns": 800000
      },
      "scopes": [
        {
          "name": "GpuPass",
          "count": 2,
          "totalNs": 400000,
          "p50Ns": 100000,
          "p95Ns": 300000,
          "p99Ns": 300000
        },
        {
          "name": "Update",
          "count": 1,
          "totalNs": 100000,
          "p50Ns": 100000,
          "p95Ns": 100000,
          "p99Ns": 100000
        }
      ]
    },
    {
      "id": 1,
      "name": "Compute",
      "kind": 1,
      "timing": false,
      "calibrations": 0,
      "maxUncertaintyNs": 0,
      "frames": {
        "count": 0,
        "totalNs": 0,
        "p50Ns": 0,
        "p95Ns": 0,
        "p99Ns": 0
      },
      "scopes": []
    }
  ],
  "capture": {
    "samples": 0,
    "poolBytesPeak": 0,
    "poolBytesLimit": 0,
    "definitionStoreBytes": 0,
    "lostRecords": 0,
    "definitionRecordsDropped": 0,
    "recentDroppedBytes": 0,
    "outputBytes": 0,
    "spanNs": 0
  }
}
)";

} // namespace

// golden 입력의 report가 정해 둔 byte와 같다. 손실을 사이에 둔 구간(끝 없음), 시작 없는 끝, 순서
// 번호 구멍, 순서 번호로 병합한 task 수명·임계 경로, 카운터가 모두 들어 있다.
TEST(TraceAnalysisReportMatchesGolden) {
    SystemAllocator allocator;
    {
        Array<uint8> input = BuildGoldenInput(allocator, 16, false);
        Array<uint8> json(allocator);
        REQUIRE(Report(allocator, input.View(), json));
        CHECK(View(json) == StringView(GoldenReport));
    }
}

// 다른 빌드가 형 id를 다르게 매겨도(등록 순서가 다름) 이름으로 스키마를 찾으므로 report가
// 같다(OB06).
TEST(TraceAnalysisSchemaIsResolvedByName) {
    SystemAllocator allocator;
    {
        Array<uint8> first = BuildGoldenInput(allocator, 16, false);
        Array<uint8> second = BuildGoldenInput(allocator, 40, true);
        Array<uint8> firstJson(allocator);
        Array<uint8> secondJson(allocator);
        REQUIRE(Report(allocator, first.View(), firstJson));
        REQUIRE(Report(allocator, second.View(), secondJson));
        CHECK(View(firstJson) == View(secondJson));
    }
}

// 끝이 잘린 파일은 어느 위치에서 잘려도 멈추거나 범위를 넘지 않고 분석된다(OB03). 머리보다 짧으면
// 입력 오류다.
TEST(TraceAnalysisToleratesTruncationAtEveryByte) {
    SystemAllocator allocator;
    {
        Array<uint8> input = BuildGoldenInput(allocator, 16, false);
        for (usize cut = 0; cut < input.Size(); ++cut) {
            TraceModel model(allocator);
            const auto built = model.Build(input.View().Left(cut));
            if (cut < TraceFormat::FileHeaderBytes) {
                CHECK(!built && built.Error() == ErrorCode::InvalidArgument);
                continue;
            }

            REQUIRE(built);
            Array<uint8> json(allocator);
            CHECK(WriteTraceReportJson(model, json));
        }
    }
}

// 이름 정의가 빠진 구간은 "<unknown:id>"로 남고 세어진다. 모르는 형 id를 만난 스트림은 그 앞까지만
// 읽고 읽지 못한 스트림으로 센다(정의 누락 입력, OB03).
TEST(TraceAnalysisReportsMissingDefinitions) {
    SystemAllocator allocator;
    {
        TraceTestWriter writer(allocator, 1000000, TraceFormat::HeaderFlagDefinitionsIncomplete);
        const StandardIds ids = WriteSchemas(writer, 16, false);
        WriteBatch(writer, TraceFormat::FirstThreadStreamId, ids.scopeBatch,
                   BatchBuilder().Begin(10, 7).End(20));
        writer.Record(TraceFormat::FirstThreadStreamId + 1, 99, {});
        Array<uint8> input = writer.Finish();

        TraceModel model(allocator);
        REQUIRE(model.Build(input.View()));
        CHECK(model.Summary().definitionsIncomplete);
        CHECK(model.Summary().missingScopeNames == 1);
        CHECK(model.Summary().undecodableStreams == 1);
        Array<uint8> json(allocator);
        REQUIRE(WriteTraceReportJson(model, json));
        CHECK(View(json).Contains(StringView("\"name\": \"<unknown:7>\"")));
        CHECK(View(json).Contains(StringView("\"complete\": false")));
    }
}

namespace {

TraceScopeName AnalysisOuter{"Analysis.Outer"};
TraceScopeName AnalysisInner{"Analysis.Inner"};
TraceCounter AnalysisCounter{"analysis.counter", TraceCounterKind::Integer, TraceCounterUnit::Count,
                             TraceCounterMode::Always};

} // namespace

// 실제 세션 capture: 출력이 세션 중간에 붙어 앞부분이 없고(시작 없는 끝·앞 순서 번호 없음), 같은
// 입력의 report는 늘 같은 byte다(결정성).
TEST(TraceAnalysisHandlesMidSessionAttachDeterministically) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        REQUIRE(session.Start(settings));
        REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
        REQUIRE(session.SetChannelEnabled(TraceCounterChannel, true));
        {
            CpuScope outer(AnalysisOuter);
            // 안쪽 구간으로 배치를 채워 바깥 시작을 출력이 없을 때 내보낸다.
            for (uint32 index = 0; index < 100; ++index) {
                CpuScope inner(AnalysisInner);
            }

            AnalysisCounter.Set(1);
            CHECK(session.Pump());
            REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
            AnalysisCounter.Set(2);
        }

        {
            CpuScope after(AnalysisOuter);
        }

        session.Stop();

        Array<uint8> first(allocator);
        Array<uint8> second(allocator);
        REQUIRE(Report(allocator, output.bytes.View(), first));
        REQUIRE(Report(allocator, output.bytes.View(), second));
        CHECK(View(first) == View(second));

        TraceModel model(allocator);
        REQUIRE(model.Build(output.bytes.View()));
        CHECK(model.Summary().attachedMidSession);
        CHECK(model.Summary().orphanScopeEnds >= 1);
        CHECK(!model.Summary().truncated);
        REQUIRE(model.Counters().Size() == 1);
        CHECK(model.Counters()[0].samples == 1);
        CHECK(static_cast<int64>(model.Counters()[0].lastBits) == 2);
        bool sawAfter = false;
        for (const TraceModelThread& thread : model.Threads()) {
            for (const TraceModelScope& scope : thread.scopes) {
                sawAfter = sawAfter || (scope.ended && model.ScopeName(scope.nameId) ==
                                                           StringView("Analysis.Outer"));
            }
        }

        CHECK(sawAfter);
    }
}

namespace {

constexpr LogCategory LogAnalysisTests{"Tests.Analysis"};
constexpr LogCategory LogTraceWriterTests{"Trace.Writer"};

} // namespace

// TraceLogSink가 채널 "log"가 켜진 동안의 기록을 남기고, 분석이 순서·스레드·상관 id·spec을
// 되살린다. 채널이 꺼진 동안의 기록과 trace 자신의 카테고리("Trace.*")는 남지 않는다. spec은
// EventId당 한 번이다.
TEST(TraceAnalysisReadsLogRecords) {
    SystemAllocator allocator;
    MemoryOutput output(allocator);
    TraceSession session;
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    REQUIRE(session.Start(settings));
    REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
    LogSystemConfig config;
    config.mode = LogMode::Manual;
    config.globalMinimum = LogLevel::Trace;
    TraceLogSink sink;
    LogSystem logs(allocator, config);
    REQUIRE(logs.AddSink(sink));
    REQUIRE(logs.Start());
    {
        LogInstallScope installed(logs);
        // 앞선 시험이 설치 밖에서 버린 낮은 레벨 기록이 있으면 설치가 그 수를 알리는 기록 하나를
        // 남긴다(Core/Diagnostics/Log.h). 세는 구간 전에 비운다(채널이 꺼져 있어 trace에 남지
        // 않는다).
        (void)logs.Pump();
        Log::Info(LogAnalysisTests, "before channel {Value}", 0);
        CHECK(logs.Pump() == 1);
        REQUIRE(session.SetChannelEnabled(TraceLogChannel, true));
        {
            CorrelationScope request(CorrelationIds{0, 0, 0, 0, 55});
            for (uint32 index = 0; index < 3; ++index) {
                Log::Info(LogAnalysisTests, "value {Value}", index);
            }
        }

        Log::Warning(LogAnalysisTests, "warned {Value}", 7);
        Log::Warning(LogTraceWriterTests, "skipped {Value}", 1);
        CHECK(logs.Pump() == 5);
    }

    logs.Stop();
    session.Stop();
    CHECK(sink.Lost() == 0);

    TraceModel model(allocator);
    REQUIRE(model.Build(output.bytes.View()));
    REQUIRE(model.Logs().Size() == 4);
    CHECK(model.LogSpecs().Size() == 2);
    const uint64 thread = platform::CurrentThreadId();
    for (usize index = 0; index < 3; ++index) {
        const TraceModelLog& log = model.Logs()[index];
        CHECK(log.thread == thread);
        CHECK(log.level == static_cast<uint8>(LogLevel::Info));
        CHECK(log.correlation[4] == 55);
        CHECK(index == 0 || log.tick >= model.Logs()[index - 1].tick);
    }

    CHECK(model.Logs()[0].message == StringView("value 0"));
    CHECK(model.Logs()[2].message == StringView("value 2"));
    const TraceModelLog& warning = model.Logs()[3];
    CHECK(warning.message == StringView("warned 7") && warning.correlation[4] == 0);
    const TraceModelLogSpec* spec = model.FindLogSpec(warning.specId);
    REQUIRE(spec != nullptr);
    CHECK(spec->category == StringView("Tests.Analysis"));
    CHECK(spec->templateText == StringView("warned {Value}"));

    Array<uint8> report(allocator);
    REQUIRE(Report(allocator, output.bytes.View(), report));
    CHECK(View(report).Contains("\"Warning\": 1", StringComparison::CaseSensitive) ||
          View(report).Contains("\"Warning\":1", StringComparison::CaseSensitive));
    CHECK(View(report).Contains("warned 7", StringComparison::CaseSensitive));
}
} // namespace trace_analysis_tests_file

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

namespace trace_analysis_tests_file {

namespace {

constexpr usize TruncationProbeBytes = MaxTraceLogMessageBytes + 64;
constexpr uint32 SpecFloodCount = TraceLogSpecTableLimit + 64;
constexpr uint64 SpecFloodBaseId = 0x1000;
// 넘침 시험이 펌프하는 간격(기록 수)이다(블록이 쌓여 기록을 잃지 않게).
constexpr uint32 SpecFloodPumpInterval = 256;
// 정의 기록 상한을 넘는 템플릿 크기다.
constexpr usize OversizedTemplateBytes = TraceFormat::MaxDefinitionRecordBytes + 64;
char oversizedTemplate[OversizedTemplateBytes];

// sink에 바로 넣는 기록이다(LogSystem 없이 EventId·위치를 정해 넣는다).
LogEvent DirectEvent(StringView message, uint64 eventId, uint32 line) noexcept {
    LogEvent event;
    event.level = LogLevel::Info;
    event.category = "Tests.Analysis";
    event.messageTemplate = "direct {Value}";
    event.message = message;
    event.eventId = eventId;
    event.location.file = "Direct.cpp";
    event.location.line = line;
    event.threadId = platform::CurrentThreadId();
    return event;
}

struct LiveSession {
    explicit LiveSession(Allocator& allocator, usize poolMaxBytes = 0) noexcept
        : output(allocator) {
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        if (poolMaxBytes != 0) {
            settings.poolMaxBytes = poolMaxBytes;
        }

        REQUIRE(session.Start(settings));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(TraceLogChannel, true));
    }

    MemoryOutput output;
    TraceSession session;
};

} // namespace

// 같은 템플릿이라도 호출 지점(카테고리·위치)이 다르면 spec이 따로이고, 기록은 자기 spec으로 찾는다.
// 중복 표가 차면 정의를 더 쓰지 않고 기록에 카테고리·템플릿을 싣는다(분석은 둘 다 읽는다). 세션이
// 바뀌면 spec을 다시 쓴다. 긴 메시지는 UTF-8 경계에서 1024 byte로 자른다. 초기화 뒤 sink는 할당하지
// 않는다.
TEST(TraceLogSinkSpecsTruncationAndSessions) {
    SystemAllocator allocator;
    TraceLogSink sink;
    {
        LiveSession live(allocator);
        // 같은 EventId, 다른 위치 → spec 둘.
        REQUIRE(sink.Write(DirectEvent("first site", 7, 10)));
        REQUIRE(sink.Write(DirectEvent("second site", 7, 20)));
        REQUIRE(sink.Write(DirectEvent("first site again", 7, 10)));
        // 마지막에 다중 byte 문자가 1024 byte 경계를 넘는 메시지.
        char text[TruncationProbeBytes];
        for (usize index = 0; index < sizeof(text); ++index) {
            text[index] = 'a';
        }

        const char han[] = "\xED\x95\x9C"; // "한"
        for (usize index = 0; index < 3; ++index) {
            text[MaxTraceLogMessageBytes - 1 + index] = han[index];
        }

        const uint64 news = test::OperatorNewCalls();
        REQUIRE(sink.Write(DirectEvent(StringView(text, sizeof(text)), 8, 30)));
        CHECK(test::OperatorNewCalls() == news);
        // 중복 표 넘침: 서로 다른 호출 지점을 상한보다 많이 넣는다.
        for (uint32 index = 0; index < SpecFloodCount; ++index) {
            REQUIRE(sink.Write(DirectEvent("flood", SpecFloodBaseId + index, index)));
            if ((index + 1) % SpecFloodPumpInterval == 0) {
                CHECK(live.session.Pump());
            }
        }

        CHECK(sink.SpecTableOverflows() + sink.SpecWriteFailures() != 0);
        live.session.Stop();

        TraceModel model(allocator);
        REQUIRE(model.Build(live.output.bytes.View()));
        REQUIRE(model.Logs().Size() == 4 + SpecFloodCount);
        CHECK(model.Logs()[0].specId == model.Logs()[2].specId);
        CHECK(model.Logs()[0].specId != model.Logs()[1].specId);
        const TraceModelLogSpec* second = model.FindLogSpec(model.Logs()[1].specId);
        REQUIRE(second != nullptr);
        CHECK(second->line == 20);
        const TraceModelLog& cut = model.Logs()[3];
        CHECK(cut.truncated);
        CHECK(cut.message.ByteCount() == MaxTraceLogMessageBytes - 1);
        CHECK(cut.message.IsValidUtf8());
        // 넘친 뒤의 기록도 카테고리를 잃지 않는다(정의 또는 기록에 실린 값).
        bool categories = true;
        for (const TraceModelLog& log : model.Logs()) {
            categories = categories && model.LogCategoryOf(log) == StringView("Tests.Analysis");
        }

        CHECK(categories);
    }

    // 세션이 바뀌면 같은 호출 지점의 spec을 새 출력에 다시 쓴다.
    {
        LiveSession live(allocator);
        REQUIRE(sink.Write(DirectEvent("next session", 7, 10)));
        live.session.Stop();
        TraceModel model(allocator);
        REQUIRE(model.Build(live.output.bytes.View()));
        REQUIRE(model.Logs().Size() == 1);
        CHECK(model.FindLogSpec(model.Logs()[0].specId) != nullptr);
    }
}

// 기록 버퍼가 없으면 기다리지 않고 버리고 Lost()로 센다(작은 풀, 펌프 없음).
TEST(TraceLogSinkCountsLostRecords) {
    SystemAllocator allocator;
    TraceLogSink sink;
    constexpr uint32 FloodRecords = 20000;
    // 풀 상한의 최소 단위(한 페이지)다.
    LiveSession live(allocator, TraceBlockLayout::PageBytes);
    for (uint32 index = 0; index < FloodRecords && sink.Lost() == 0; ++index) {
        REQUIRE(sink.Write(DirectEvent("lost probe message with some padding text", 9, 40)));
    }

    CHECK(sink.Lost() != 0);
    live.session.Stop();
}

// 재진입 표시가 켜진 스레드(Write 안)에서 sink를 직접 불러도 기록하지 않는다. trace 자신의
// 카테고리도 남기지 않는다. 표시를 벗어난 뒤의 직접 호출은 기록한다.
TEST(TraceLogSinkBlocksReentrantDirectWrite) {
    SystemAllocator allocator;
    TraceLogSink sink;
    LiveSession live(allocator);
    {
        detail::TraceLogWriteScope inside;
        REQUIRE(sink.Write(DirectEvent("reentrant", 11, 50)));
        {
            detail::TraceLogWriteScope nested;
        }

        CHECK(detail::inTraceLogWrite);
    }

    CHECK(!detail::inTraceLogWrite);
    LogEvent own = DirectEvent("trace own", 12, 51);
    own.category = "Trace.Writer";
    REQUIRE(sink.Write(own));
    REQUIRE(sink.Write(DirectEvent("outside", 11, 52)));
    live.session.Stop();
    TraceModel model(allocator);
    REQUIRE(model.Build(live.output.bytes.View()));
    REQUIRE(model.Logs().Size() == 1);
    CHECK(model.Logs()[0].message == StringView("outside"));
}

// spec id는 위치 문자열의 내용으로 정한다. 같은 내용의 파일·함수 문자열이 다른 주소에 있어도(여러
// TU의 inline 인스턴스) 같은 spec이다.
TEST(TraceLogSinkSpecIdUsesLocationText) {
    SystemAllocator allocator;
    TraceLogSink sink;
    LiveSession live(allocator);
    char fileA[] = "Shared.h";
    char fileB[] = "Shared.h";
    char functionA[] = "SharedInline";
    char functionB[] = "SharedInline";
    LogEvent first = DirectEvent("copy a", 13, 60);
    first.location.file = fileA;
    first.location.function = functionA;
    LogEvent second = DirectEvent("copy b", 13, 60);
    second.location.file = fileB;
    second.location.function = functionB;
    REQUIRE(sink.Write(first));
    REQUIRE(sink.Write(second));
    live.session.Stop();
    TraceModel model(allocator);
    REQUIRE(model.Build(live.output.bytes.View()));
    REQUIRE(model.Logs().Size() == 2);
    CHECK(model.Logs()[0].specId == model.Logs()[1].specId);
    CHECK(model.LogSpecs().Size() == 1);
}

// 정의 기록 상한을 넘는 spec은 정의를 시도하지 않고 기록에 동봉한다. 동봉한 템플릿은
// MaxTraceLogMessageBytes에서 잘리고 잘림 비트가 켜진다. 세션의 "버린 정의" 수는 늘지 않고 sink만
// 센다. 동봉한 기록에는 위치가 없다(spec 정의 없음).
TEST(TraceLogSinkInlinesOversizedSpec) {
    SystemAllocator allocator;
    TraceLogSink sink;
    LiveSession live(allocator);
    for (usize index = 0; index < OversizedTemplateBytes; ++index) {
        oversizedTemplate[index] = 't';
    }

    LogEvent event = DirectEvent("oversized", 14, 70);
    event.messageTemplate = StringView(oversizedTemplate, OversizedTemplateBytes);
    constexpr uint32 Writes = 3;
    for (uint32 index = 0; index < Writes; ++index) {
        REQUIRE(sink.Write(event));
    }

    CHECK(sink.SpecWriteFailures() == Writes);
    CHECK(live.session.Stats().definitionRecordsDropped == 0);
    live.session.Stop();
    TraceModel model(allocator);
    REQUIRE(model.Build(live.output.bytes.View()));
    REQUIRE(model.Logs().Size() == Writes);
    for (const TraceModelLog& log : model.Logs()) {
        CHECK(log.templateTruncated && !log.truncated);
        CHECK(log.inlineTemplate.ByteCount() == MaxTraceLogMessageBytes);
        CHECK(model.FindLogSpec(log.specId) == nullptr);
        CHECK(model.LogCategoryOf(log) == StringView("Tests.Analysis"));
    }
}

// task 실행 구간은 trace 없이도 저장한 상관 id를 복원하고 내린다. 빈 정보는 아무것도 쌓지 않는다.
TEST(TraceTaskScopeRestoresCorrelationWithoutTrace) {
    {
        const TraceTaskInfo empty;
        TraceTaskScope scope(empty);
        CHECK(CorrelationDepth() == 0);
    }

    TraceTaskInfo info;
    {
        CorrelationScope request(CorrelationIds{0, 0, 0, 0, 77});
        info = TraceCreateTask();
    }

    REQUIRE(info.context.valid);
    CHECK(!info.id.IsTraced());
    CHECK(CorrelationDepth() == 0);
    {
        TraceTaskScope scope(info);
        CorrelationIds ids;
        CHECK(CurrentCorrelation(ids) && ids.request == 77);
        CHECK(CorrelationDepth() == 1);
    }

    CHECK(CorrelationDepth() == 0);
}

// 공개 CorrelationScope로 쌓은 값이 trace 문맥 정의 기록에 같게 나간다(관찰자 경로).
TEST(TraceContextRecordsMatchPublicCorrelationScope) {
    SystemAllocator allocator;
    MemoryOutput output(allocator);
    TraceSession session;
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    REQUIRE(session.Start(settings));
    REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
    REQUIRE(session.SetChannelEnabled(TraceContextChannel, true));
    {
        CorrelationScope request(CorrelationIds{3, 0, 0, 0, 99});
        const CorrelationEntry* top = detail::TopCorrelationEntry();
        REQUIRE(top != nullptr);
        CHECK(top->ext[CorrelationPushExtension] != 0);
    }

    session.Stop();
    TraceStreamDecoder decoder(allocator);
    REQUIRE(decoder.Decode(output.bytes.View()));
    const DecodedSchema* schema = decoder.FindSchema(StringView("Trace.ContextDefinition"));
    REQUIRE(schema != nullptr);
    Array<DecodedRecord> records(allocator);
    (void)decoder.DecodeRecordsPartial(TraceFormat::DefinitionStreamId, records);
    bool found = false;
    for (const DecodedRecord& record : records) {
        if (record.typeId != schema->typeId ||
            record.fixed.Size() < TraceStandardFormat::ContextDefinitionFixedBytes) {
            continue;
        }

        const uint8* values =
            record.fixed.Data() + TraceStandardFormat::ContextDefinitionValuesOffset;
        constexpr usize RequestIndex = 4;
        found = found || (LoadLittleEndian<uint64>(values) == 3 &&
                          LoadLittleEndian<uint64>(values + RequestIndex * sizeof(uint64)) == 99);
    }

    CHECK(found);
}
} // namespace trace_analysis_tests_file
