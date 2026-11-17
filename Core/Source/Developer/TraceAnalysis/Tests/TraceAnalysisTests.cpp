#include "TraceTestWriter.h"

#include <TestSupport/Test.h>
#include <TraceAnalysis/TraceModel.h>
#include <TraceAnalysis/TraceReport.h>
#include <TraceCapture/Tests/TraceCaptureTestSupport.h>
#include <TraceCapture/TraceCounter.h>
#include <TraceCapture/TraceCpuScope.h>

// TR4 분석 시험이다(Docs/Design/TraceSystem.ko.md 10절). 입력은 tick·순서·형 id를 정한 writer로
// 만들어 golden report와 byte 단위로 비교하고, 실제 세션 capture로 결정성과 손상 입력(OB03)을
// 확인한다.
using namespace rocks;
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

    writer.Schema(TraceFormat::ChannelInfoTypeId, "Trace.ChannelInfo", TraceFormat::NoChannelId,
                  TraceFormat::TypeFlagDefinition, TraceFormat::ChannelInfoFixedBytes, ChannelInfo);
    writer.Schema(TraceFormat::ThreadInfoTypeId, "Trace.ThreadInfo", TraceFormat::NoChannelId,
                  TraceFormat::TypeFlagDefinition, TraceFormat::ThreadInfoFixedBytes, ThreadInfo);
    writer.Schema(TraceFormat::LossTypeId, "Trace.Loss", TraceFormat::NoChannelId, 0,
                  TraceFormat::LossFixedBytes, Loss);

    StandardIds ids;
    uint16 slots[6];
    for (uint16 index = 0; index < 6; ++index) {
        slots[index] = static_cast<uint16>(reversed ? base + 5 - index : base + index);
    }

    ids = {slots[0], slots[1], slots[2], slots[3], slots[4], slots[5]};
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

// golden 입력이다. 1 tick = 1µs(주파수 10^6).
// - 스레드 1 "Main": Frame[1000, 2000]{Update 300, Update 200}, Frame[3000, 4000]{Update 100},
//   손실(cpu 5개), 끝 없는 Frame(스트림 끝), 표지 2개, 카운터 100 → 300(순서 번호 8이 빠짐).
// - 스레드 2 "Worker.0": 시작 없는 끝 1개, Job 200·50, 손실(task 2개)로 끝을 잃은 Job, Job 100.
// - task: 1 → 2, 1 → 3(실패), 4(취소). 임계 경로 1 → 2.
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
  "version": 1,
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
  ]
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
