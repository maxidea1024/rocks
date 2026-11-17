#include "TraceCaptureTestSupport.h"

#include <TestSupport/Test.h>
#include <TraceCapture/TraceContext.h>
#include <TraceCapture/TraceCounter.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceMark.h>

// TR2 표준 기록(CPU 구간·이름 표·카운터·표지·시간 띠·프레임·스레드 정보·문맥)의 시험이다. 순서는
// gate와 수동 펌프로만 맞춘다.
using namespace rocks;
using namespace rocks::tracetest;

namespace {

using Format = TraceStandardFormat;

TraceSessionSettings ManualSettings(Allocator& allocator) noexcept {
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    return settings;
}

StringView TextOf(Span<const uint8> bytes) noexcept {
    return StringView(reinterpret_cast<const char*>(bytes.Data()), bytes.Size());
}

uint16 TypeId(const TraceStreamDecoder& decoder, const char* name) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(name));
    return schema != nullptr ? schema->typeId : TraceFormat::InvalidTypeId;
}

// 정의 스트림에서 이름 id → 이름을 찾는다. 같은 이름의 정의 수를 sameName에 센다.
StringView ScopeName(const TraceStreamDecoder& decoder, const Array<DecodedRecord>& definitions,
                     uint32 id) noexcept {
    const uint16 type = TypeId(decoder, Format::ScopeNameType);
    for (const DecodedRecord& record : definitions) {
        if (record.typeId == type && ReadFixed<uint32>(record, Format::ScopeNameIdOffset) == id) {
            return TextOf(record.variable[0]);
        }
    }

    return {};
}

// 정의 스트림에서 이름이 name인 이름 정의의 수와 그 id다.
usize CountScopeNames(const TraceStreamDecoder& decoder, const Array<DecodedRecord>& definitions,
                      StringView name, uint32& id) noexcept {
    const uint16 type = TypeId(decoder, Format::ScopeNameType);
    usize count = 0;
    for (const DecodedRecord& record : definitions) {
        if (record.typeId == type && TextOf(record.variable[0]) == name) {
            id = ReadFixed<uint32>(record, Format::ScopeNameIdOffset);
            ++count;
        }
    }

    return count;
}

// 한 스레드 스트림의 CPU 구간 배치를 모두 풀어 사건 열로 만든다.
bool CollectScopeEvents(const TraceStreamDecoder& decoder, uint16 stream,
                        Array<DecodedScopeEvent>& events, usize& batches) noexcept {
    SystemAllocator scratch;
    bool decoded = true;
    {
        Array<DecodedRecord> records(scratch);
        if (!decoder.DecodeRecords(stream, records)) {
            return false;
        }

        const uint16 type = TypeId(decoder, Format::CpuScopeBatchType);
        batches = 0;
        for (const DecodedRecord& record : records) {
            if (record.typeId == type) {
                ++batches;
                decoded = decoded && DecodeScopeBatch(record.variable[0], events);
            }
        }
    }

    return decoded;
}

TraceScopeName OuterName{"Test.Outer"};
TraceScopeName InnerName{"Test.Inner"};
TraceScopeName SecondName{"Test.Second"};
TraceScopeName SharedStaticName{"Test.Shared.Static"};

void RunNested(uint32 count) noexcept {
    CpuScope outer(OuterName);
    for (uint32 index = 0; index < count; ++index) {
        CpuScope inner(InnerName);
        char name[32];
        const int length = snprintf(name, sizeof(name), "Test.Dynamic.%u", index % 3);
        CpuScope dynamic(StringView(name, static_cast<usize>(length)));
    }
}

} // namespace

// CPU 구간 배치(varint Δtick·이름 id)가 그대로 돌아오고, 같은 이름은 정적·동적 모두 id 하나다(T4).
// 깊이가 0으로 돌아올 때와 배치가 찰 때 이벤트를 내며, 시작 때 꺼져 있던 채널의 끝은 기록하지
// 않는다.
TEST(TraceCpuScopeBatchRoundTrips) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        {
            // 꺼진 채널에서 시작한 구간은 중간에 채널을 켜도 끝을 내지 않는다.
            CpuScope skipped(SecondName);
            REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
        }

        // 안쪽 한 번은 사건 4개(최소 6 byte)이므로 100번이면 배치 256 B를 적어도 두 번 넘긴다.
        constexpr uint32 Inner = 100;
        RunNested(Inner);
        {
            ROCKS_TRACE_CPU_SCOPE("Test.Macro");
        }

        CHECK(session.Stats().scopeNames == 6);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        const uint32 thread = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(thread != 0);
        Array<DecodedScopeEvent> events(allocator);
        usize batches = 0;
        REQUIRE(CollectScopeEvents(decoder, ThreadStream(thread), events, batches));
        // 안쪽 구간들은 배치 여러 개로 나뉘고, 매크로 구간은 깊이 0에서 따로 나간다.
        CHECK(batches >= 3);
        Array<DecodedScope> scopes(allocator);
        usize orphans = 0;
        REQUIRE(BuildScopes(events.View(), scopes, orphans));
        CHECK(orphans == 0);
        REQUIRE(scopes.Size() == 2 + 2 * Inner);

        uint32 innerId = 0;
        uint32 dynamicId = 0;
        CHECK(CountScopeNames(decoder, definitions, StringView("Test.Inner"), innerId) == 1);
        CHECK(CountScopeNames(decoder, definitions, StringView("Test.Dynamic.1"), dynamicId) == 1);
        CHECK(ScopeName(decoder, definitions, scopes[0].nameId) == StringView("Test.Outer"));
        CHECK(scopes[0].depth == 0);
        uint64 previous = 0;
        for (usize index = 0; index < scopes.Size(); ++index) {
            const DecodedScope& scope = scopes[index];
            CHECK(scope.ended);
            CHECK(scope.beginTick >= previous);
            CHECK(scope.endTick >= scope.beginTick);
            previous = scope.beginTick;
            if (index > 0 && index + 1 < scopes.Size()) {
                const bool inner = (index - 1) % 2 == 0;
                CHECK(scope.depth == (inner ? 1u : 2u));
                const uint32 dynamicIndex = static_cast<uint32>((index - 1) / 2) % 3;
                if (inner) {
                    CHECK(scope.nameId == innerId);
                } else if (dynamicIndex == 1) {
                    CHECK(scope.nameId == dynamicId);
                }
            }
        }

        CHECK(ScopeName(decoder, definitions, scopes[scopes.Size() - 1].nameId) ==
              StringView("Test.Macro"));
    }
}

namespace {

struct NameWorker {
    platform::ManualResetEvent* start = nullptr;
    uint64 osThreadId = 0;
};

void RunNameWorker(void* context) noexcept {
    auto& worker = *static_cast<NameWorker*>(context);
    worker.osThreadId = platform::CurrentThreadId();
    WaitGate(*worker.start);
    for (uint32 round = 0; round < 8; ++round) {
        CpuScope shared(SharedStaticName);
        CpuScope dynamic(StringView("Test.Shared.Dynamic"));
    }
}

} // namespace

// 여러 스레드가 같은 정적 이름 객체와 같은 동적 이름을 동시에 처음 쓴다. 누가 먼저 넣든 이름 정의는
// 하나이고 모든 스레드의 구간이 같은 id를 쓴다(경쟁과 무관한 판정).
TEST(TraceScopeNameIdsConvergeAcrossThreads) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));

        constexpr usize WorkerCount = 4;
        platform::ManualResetEvent start;
        NameWorker workers[WorkerCount];
        platform::Thread threads[WorkerCount];
        for (usize index = 0; index < WorkerCount; ++index) {
            workers[index].start = &start;
            REQUIRE(threads[index].Start(&RunNameWorker, &workers[index]) ==
                    platform::ThreadStatus::Success);
        }

        start.Signal();
        for (platform::Thread& thread : threads) {
            JoinThread(thread);
        }

        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        uint32 staticId = 0;
        uint32 dynamicId = 0;
        CHECK(CountScopeNames(decoder, definitions, StringView("Test.Shared.Static"), staticId) ==
              1);
        CHECK(CountScopeNames(decoder, definitions, StringView("Test.Shared.Dynamic"), dynamicId) ==
              1);
        CHECK(staticId != dynamicId);
        for (const NameWorker& worker : workers) {
            const uint32 thread = FindTraceThread(decoder, worker.osThreadId);
            REQUIRE(thread != 0);
            Array<DecodedScopeEvent> events(allocator);
            usize batches = 0;
            REQUIRE(CollectScopeEvents(decoder, ThreadStream(thread), events, batches));
            Array<DecodedScope> scopes(allocator);
            usize orphans = 0;
            REQUIRE(BuildScopes(events.View(), scopes, orphans));
            REQUIRE(scopes.Size() == 16);
            for (const DecodedScope& scope : scopes) {
                CHECK(scope.nameId == (scope.depth == 0 ? staticId : dynamicId));
                CHECK(scope.ended);
            }
        }
    }
}

namespace {

TraceChannel FillerChannel{"test.filler", "Records that fill the pool"};

struct FillerEvent {
    uint8 bytes[64];
};

constexpr TraceField FillerFields[] = {{"head", TraceFieldType::UInt64, 0}};
TraceEventType FillerType{"Test.Filler", FillerChannel, sizeof(FillerEvent), FillerFields};

} // namespace

// 구간의 시작은 기록됐지만 끝이 든 배치를 블록 풀 고갈로 잃으면, 분석은 그 구간을 "끝 없음"으로
// 두고 길이를 지어내지 않는다. 손실은 손실 기록(cpu 채널)으로 남는다. 펌프하지 않는 한 블록이
// 돌아오지 않으므로(gate) 고갈 시점이 결정적이다.
TEST(TraceCpuScopeLostEndIsReportedOpen) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings = ManualSettings(allocator);
        settings.poolMaxBytes = TraceBlockLayout::PageBytes;
        REQUIRE(session.Start(settings));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.RegisterChannel(FillerChannel, true));
        REQUIRE(session.RegisterEventType(FillerType));
        REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
        const uint16 cpuChannel = TraceCpuChannel.Id();
        {
            CpuScope outer(OuterName);
            // 안쪽 구간으로 배치를 여러 번 채워 바깥 시작이 든 배치를 먼저 낸다.
            for (uint32 index = 0; index < 200; ++index) {
                CpuScope inner(InnerName);
            }

            // 풀을 고갈시킨다. 그 뒤 이 스레드는 손실 상태라 다음 배치도 블록을 받지 못한다.
            TraceWriteResult filled = TraceWriteResult::Recorded;
            while (filled == TraceWriteResult::Recorded) {
                filled = TraceWrite(FillerType, FillerEvent{});
            }

            CHECK(filled == TraceWriteResult::Lost);
        }

        CHECK(session.Stats().lostRecords >= 2);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        const uint32 thread = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(thread != 0);
        Array<DecodedScopeEvent> events(allocator);
        usize batches = 0;
        REQUIRE(CollectScopeEvents(decoder, ThreadStream(thread), events, batches));
        Array<DecodedScope> scopes(allocator);
        usize orphans = 0;
        REQUIRE(BuildScopes(events.View(), scopes, orphans));
        REQUIRE(!scopes.IsEmpty());
        CHECK(scopes[0].depth == 0);
        CHECK(!scopes[0].ended);
        CHECK(scopes[0].endTick == 0);
        for (usize index = 1; index < scopes.Size(); ++index) {
            CHECK(scopes[index].depth == 1);
        }

        // 손실 기록에 cpu 채널의 손실이 있다.
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(thread), records));
        bool cpuLoss = false;
        for (const DecodedRecord& record : records) {
            if (record.typeId == TraceFormat::LossTypeId) {
                for (usize index = 0; index < record.variable[0].Size() / sizeof(uint16); ++index) {
                    if (LoadLittleEndian<uint16>(record.variable[0].Data() + index * 2) ==
                        cpuChannel) {
                        cpuLoss = true;
                    }
                }
            }
        }

        CHECK(cpuLoss);
    }
}

namespace {

struct RestoreWorker {
    TraceContextToken token;
    TraceContextValues seen;
    bool hadContext = false;
    bool emptyAfter = false;
    uint64 osThreadId = 0;
};

void RunRestoreWorker(void* context) noexcept {
    auto& worker = *static_cast<RestoreWorker*>(context);
    worker.osThreadId = platform::CurrentThreadId();
    {
        TraceContextScope restored(worker.token);
        worker.hadContext = TraceCurrentContext(worker.seen);
    }

    TraceContextValues after;
    worker.emptyAfter = !TraceCurrentContext(after);
}

} // namespace

// 저장한 문맥을 다른 스레드에서 복원한다. 복원은 정의를 다시 쓰지 않고 같은 id만 기록하며, 복원한
// 스레드의 현재 문맥이 저장한 값과 같다. 문맥 스택은 세션이 없어도 동작한다.
TEST(TraceContextRestoresAcrossThreads) {
    SystemAllocator allocator;
    {
        TraceContextValues outside;
        {
            TraceContextScope noSession(TraceContextValues{0, 0, 0, 0, 9});
            CHECK(TraceCurrentContext(outside));
            CHECK(outside.request == 9);
        }

        CHECK(!TraceCurrentContext(outside));

        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(TraceContextChannel, true));

        RestoreWorker worker;
        {
            TraceContextScope request(TraceContextValues{1, 0, 0, 0, 7});
            {
                TraceContextScope task(TraceContextValues{1, 3, 0, 0, 7});
                worker.token = TraceCaptureContext();
            }

            platform::Thread thread;
            REQUIRE(thread.Start(&RunRestoreWorker, &worker) == platform::ThreadStatus::Success);
            JoinThread(thread);
        }

        CHECK(worker.hadContext);
        CHECK(worker.seen.task == 3);
        CHECK(worker.seen.request == 7);
        CHECK(worker.emptyAfter);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        const uint16 definitionType = TypeId(decoder, Format::ContextDefinitionType);
        const uint16 pushType = TypeId(decoder, Format::ContextPushType);
        const uint16 popType = TypeId(decoder, Format::ContextPopType);
        uint32 taskContext = 0;
        usize definitionCount = 0;
        for (const DecodedRecord& record : definitions) {
            if (record.typeId == definitionType) {
                ++definitionCount;
                if (ReadFixed<uint64>(record, Format::ContextDefinitionValuesOffset + 8) == 3) {
                    taskContext = ReadFixed<uint32>(record, Format::ContextDefinitionIdOffset);
                }
            }
        }

        // 바깥 요청 문맥과 task 문맥, 두 정의뿐이다(복원은 정의를 다시 쓰지 않는다).
        CHECK(definitionCount == 2);
        REQUIRE(taskContext != 0);

        const uint32 workerThread = FindTraceThread(decoder, worker.osThreadId);
        REQUIRE(workerThread != 0);
        Array<DecodedRecord> workerRecords(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(workerThread), workerRecords));
        REQUIRE(workerRecords.Size() == 2);
        CHECK(workerRecords[0].typeId == pushType);
        CHECK(ReadFixed<uint32>(workerRecords[0], Format::ContextPushIdOffset) == taskContext);
        CHECK(workerRecords[1].typeId == popType);

        const uint32 mainThread = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(mainThread != 0);
        Array<DecodedRecord> mainRecords(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(mainThread), mainRecords));
        REQUIRE(mainRecords.Size() == 4);
        CHECK(mainRecords[0].typeId == pushType);
        CHECK(mainRecords[1].typeId == pushType);
        CHECK(ReadFixed<uint32>(mainRecords[1], Format::ContextPushIdOffset) == taskContext);
        CHECK(mainRecords[2].typeId == popType);
        CHECK(mainRecords[3].typeId == popType);
    }
}

namespace {

TraceCounter ChangeCounter{"test.counter.change", TraceCounterKind::Integer,
                           TraceCounterUnit::Bytes};
TraceCounter AlwaysCounter{"test.counter.always", TraceCounterKind::Integer,
                           TraceCounterUnit::Count, TraceCounterMode::Always};
TraceCounter FloatCounter{"test.counter.float", TraceCounterKind::Float, TraceCounterUnit::Percent,
                          TraceCounterMode::Always};

struct SpanEnder {
    TraceSpanId span;
};

void RunSpanEnder(void* context) noexcept {
    TraceEndSpan(static_cast<SpanEnder*>(context)->span);
}

// 정의 스트림에서 카운터 정의 이름 → id다.
uint32 CounterId(const TraceStreamDecoder& decoder, const Array<DecodedRecord>& definitions,
                 StringView name, usize& count) noexcept {
    const uint16 type = TypeId(decoder, Format::CounterDefinitionType);
    uint32 id = 0;
    count = 0;
    for (const DecodedRecord& record : definitions) {
        if (record.typeId == type && TextOf(record.variable[0]) == name) {
            id = ReadFixed<uint32>(record, Format::CounterDefinitionIdOffset);
            ++count;
        }
    }

    return id;
}

} // namespace

// 카운터(바뀔 때만·매번·실수, 채널이 나중에 켜지면 첫 값 전에 정의), 표지(호출 지점 정의와 인자),
// 시간 띠(다른 스레드에서 닫기, id로 짝), 프레임 경계를 기록하고 읽는다.
TEST(TraceCountersMarksSpansAndFramesRoundTrip) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));

        // 채널이 꺼져 있으면 값만 바뀌고 기록·정의는 없다.
        ChangeCounter.Set(1);
        CHECK(ChangeCounter.Value() == 1);
        REQUIRE(session.SetChannelEnabled(TraceCounterChannel, true));
        REQUIRE(session.SetChannelEnabled(TraceMarkChannel, true));
        REQUIRE(session.SetChannelEnabled(TraceSpanChannel, true));
        REQUIRE(session.SetChannelEnabled(TraceFrameChannel, true));

        ChangeCounter.Set(5);
        ChangeCounter.Set(5);
        ChangeCounter.Add(1);
        AlwaysCounter.Set(2);
        AlwaysCounter.Set(2);
        FloatCounter.SetFloat(0.5);
        FloatCounter.AddFloat(0.25);
        CHECK(ChangeCounter.Value() == 6);
        CHECK(FloatCounter.FloatValue() == 0.75);

        TraceBeginFrame(TraceFrameKind::Game, 10);
        ROCKS_TRACE_MARK("Loaded {} in {} ms ({})", "level", 12, true);
        TraceEndFrame(TraceFrameKind::Game, 10);

        SpanEnder ender;
        ender.span = TraceBeginSpan(StringView("Test.Span"));
        CHECK(ender.span.value != 0);
        platform::Thread thread;
        REQUIRE(thread.Start(&RunSpanEnder, &ender) == platform::ThreadStatus::Success);
        JoinThread(thread);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        usize changeDefinitions = 0;
        usize alwaysDefinitions = 0;
        usize floatDefinitions = 0;
        const uint32 changeId =
            CounterId(decoder, definitions, StringView("test.counter.change"), changeDefinitions);
        const uint32 alwaysId =
            CounterId(decoder, definitions, StringView("test.counter.always"), alwaysDefinitions);
        const uint32 floatId =
            CounterId(decoder, definitions, StringView("test.counter.float"), floatDefinitions);
        CHECK(changeDefinitions == 1);
        CHECK(alwaysDefinitions == 1);
        CHECK(floatDefinitions == 1);

        const uint32 thread0 = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(thread0 != 0);
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(thread0), records));
        const uint16 integerType = TypeId(decoder, Format::CounterIntegerType);
        const uint16 floatType = TypeId(decoder, Format::CounterFloatType);
        const uint16 markType = TypeId(decoder, Format::MarkType);
        const uint16 frameBegin = TypeId(decoder, Format::FrameBeginType);
        const uint16 frameEnd = TypeId(decoder, Format::FrameEndType);
        const uint16 spanBegin = TypeId(decoder, Format::SpanBeginType);
        Array<int64> changeValues(allocator);
        usize alwaysValues = 0;
        Array<uint64> floatBits(allocator);
        usize marks = 0;
        usize frames = 0;
        uint64 spanId = 0;
        uint32 spanName = 0;
        for (const DecodedRecord& record : records) {
            if (record.typeId == integerType) {
                CHECK(record.hasOrder);
                const uint32 id = ReadFixed<uint32>(record, Format::CounterValueIdOffset);
                const int64 value = ReadFixed<int64>(record, Format::CounterValueOffset);
                if (id == changeId) {
                    REQUIRE(changeValues.TryPushBack(value));
                } else if (id == alwaysId) {
                    CHECK(value == 2);
                    ++alwaysValues;
                }
            } else if (record.typeId == floatType) {
                CHECK(ReadFixed<uint32>(record, Format::CounterValueIdOffset) == floatId);
                REQUIRE(
                    floatBits.TryPushBack(ReadFixed<uint64>(record, Format::CounterValueOffset)));
            } else if (record.typeId == markType) {
                ++marks;
                // "level", 12, true → String·Int64·Bool tag 순서다.
                const Span<const uint8> arguments = record.variable[0];
                REQUIRE(arguments.Size() == 1 + 4 + 5 + 1 + 8 + 1 + 1);
                CHECK(arguments[0] == Format::MarkArgumentString);
                CHECK(TextOf(arguments.Mid(5, 5)) == StringView("level"));
                CHECK(arguments[10] == Format::MarkArgumentInt64);
                CHECK(LoadLittleEndian<uint64>(arguments.Data() + 11) == 12);
                CHECK(arguments[19] == Format::MarkArgumentBool);
                CHECK(arguments[20] == 1);
            } else if (record.typeId == frameBegin || record.typeId == frameEnd) {
                ++frames;
                CHECK(ReadFixed<uint64>(record, Format::FrameNumberOffset) == 10);
                CHECK(ReadFixedByte(record, Format::FrameKindOffset) == 0);
            } else if (record.typeId == spanBegin) {
                spanId = ReadFixed<uint64>(record, Format::SpanIdOffset);
                spanName = ReadFixed<uint32>(record, Format::SpanNameOffset);
            }
        }

        // 바뀔 때만: 5, 6(5를 다시 낸 것은 빠진다). 매번: 2, 2.
        REQUIRE(changeValues.Size() == 2);
        CHECK(changeValues[0] == 5);
        CHECK(changeValues[1] == 6);
        CHECK(alwaysValues == 2);
        REQUIRE(floatBits.Size() == 2);
        CHECK(floatBits[1] == platform::BitCast<uint64>(0.75));
        CHECK(marks == 1);
        CHECK(frames == 2);
        CHECK(spanId == ender.span.value);
        CHECK(ScopeName(decoder, definitions, spanName) == StringView("Test.Span"));

        // 표지 호출 지점 정의(형식 문자열·파일·줄)다.
        const uint16 siteType = TypeId(decoder, Format::MarkSiteType);
        bool sawSite = false;
        for (const DecodedRecord& record : definitions) {
            if (record.typeId == siteType) {
                sawSite = TextOf(record.variable[0]) == StringView("Loaded {} in {} ms ({})") &&
                          ReadFixed<uint32>(record, Format::MarkSiteLineOffset) != 0;
            }
        }

        CHECK(sawSite);

        // 시간 띠의 끝은 다른 스레드 스트림에 같은 id로 있다.
        const uint16 spanEnd = TypeId(decoder, Format::SpanEndType);
        bool sawEnd = false;
        for (const DecodedStream& stream : decoder.Streams()) {
            if (stream.id < TraceFormat::FirstThreadStreamId ||
                stream.id == ThreadStream(thread0)) {
                continue;
            }

            Array<DecodedRecord> other(allocator);
            REQUIRE(decoder.DecodeRecords(stream.id, other));
            for (const DecodedRecord& record : other) {
                if (record.typeId == spanEnd &&
                    ReadFixed<uint64>(record, Format::SpanIdOffset) == ender.span.value) {
                    sawEnd = true;
                }
            }
        }

        CHECK(sawEnd);
    }
}

namespace {

struct ChurnWorker {
    bool named = false;
    TraceWriteResult result = TraceWriteResult::Disabled;
};

void RunChurnWorker(void* context) noexcept {
    auto& worker = *static_cast<ChurnWorker*>(context);
    if (worker.named) {
        TraceSetThreadInfo(StringView("Test.Worker"), StringView("Test.Group"), 5);
    }

    worker.result = TraceWrite(FillerType, FillerEvent{});
}

} // namespace

// 스레드를 만들고 없애기를 반복해도 스레드 상태 칸은 writer가 다 비운 뒤 재사용되어 늘지 않는다.
// trace 스레드 id는 재사용하지 않는다. 스레드 정보(이름·묶음·정렬 순위)는 정의 기록으로 남는다.
TEST(TraceThreadSlotsAreReusedAndThreadInfoIsRecorded) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.RegisterChannel(FillerChannel, true));
        REQUIRE(session.RegisterEventType(FillerType));

        constexpr uint32 Rounds = 20;
        for (uint32 round = 0; round < Rounds; ++round) {
            ChurnWorker worker;
            worker.named = round == 0;
            platform::Thread thread;
            REQUIRE(thread.Start(&RunChurnWorker, &worker) == platform::ThreadStatus::Success);
            // Join은 TLS 정리(스레드 은퇴)까지 기다리고, 펌프가 남은 내용을 보낸 뒤 칸을 돌려준다.
            JoinThread(thread);
            CHECK(worker.result == TraceWriteResult::Recorded);
            CHECK(session.Pump());
            const TraceSessionStats stats = session.Stats();
            CHECK(stats.threadSlots == 1);
            CHECK(stats.threadCount == round + 1);
            CHECK(stats.poolBytesInUse == 0);
        }

        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        usize threadInfos = 0;
        bool sawNamed = false;
        for (const DecodedRecord& record : definitions) {
            if (record.typeId == TraceFormat::ThreadInfoTypeId) {
                ++threadInfos;
                if (TextOf(record.variable[0]) == StringView("Test.Worker")) {
                    sawNamed =
                        TextOf(record.variable[1]) == StringView("Test.Group") &&
                        ReadFixed<uint32>(record, TraceFormat::ThreadInfoSortOrderOffset) == 5;
                }
            }
        }

        CHECK(threadInfos == Rounds);
        CHECK(sawNamed);
        usize threadStreams = 0;
        for (const DecodedStream& stream : decoder.Streams()) {
            if (stream.id >= TraceFormat::FirstThreadStreamId) {
                ++threadStreams;
            }
        }

        CHECK(threadStreams == Rounds);
    }
}

// 이름 표 상한(1024 byte)을 넘는 동적 이름은 앞부분만 쓰고 이름 정의에 잘림을 표시한다(옛
// ProfileScope의 잘림 표시를 잇는다). 앞부분이 같은 두 긴 이름은 같은 id다.
TEST(TraceLongScopeNameIsTruncatedAndFlagged) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
        static char longName[2000];
        for (char& c : longName) {
            c = 'n';
        }

        {
            CpuScope first(StringView(longName, sizeof(longName)));
        }

        longName[sizeof(longName) - 1] = 'm';
        {
            CpuScope second(StringView(longName, sizeof(longName)));
        }

        CHECK(session.Stats().scopeNames == 1);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        const uint16 type = TypeId(decoder, Format::ScopeNameType);
        usize names = 0;
        for (const DecodedRecord& record : definitions) {
            if (record.typeId == type) {
                ++names;
                CHECK(ReadFixedByte(record, Format::ScopeNameTruncatedOffset) == 1);
                CHECK(record.variable[0].Size() == Format::MaxScopeNameBytes);
            }
        }

        CHECK(names == 1);
    }
}
