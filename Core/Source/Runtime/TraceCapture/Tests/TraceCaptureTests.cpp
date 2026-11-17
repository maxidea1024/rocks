#include "TraceCaptureTestSupport.h"

#include <TestSupport/Test.h>
#include <TraceCapture/TraceCpuScope.h>

#include <stdio.h>

using namespace rocks;
using namespace rocks::tracetest;

namespace {

TraceChannel RecordChannel{"test.record", "TR1 시험 기록"};
TraceChannel ReadOnlyChannel{"test.readonly", "읽기 전용 시험 채널", true};

struct SampleEvent {
    uint32 thread;
    uint32 sequence;
    uint64 value;
};

constexpr TraceField SampleFields[] = {
    {"thread", TraceFieldType::UInt32, ROCKS_OFFSET_OF(SampleEvent, thread)},
    {"sequence", TraceFieldType::UInt32, ROCKS_OFFSET_OF(SampleEvent, sequence)},
    {"value", TraceFieldType::UInt64, ROCKS_OFFSET_OF(SampleEvent, value)},
    {"label", TraceFieldType::String, TraceField::Variable},
    {"samples", TraceFieldType::UInt32Array, TraceField::Variable},
};

TraceEventType SampleType{"Test.Sample", RecordChannel, sizeof(SampleEvent), SampleFields};

struct OrderedEvent {
    uint32 thread;
    uint32 sequence;
};

constexpr TraceField OrderedFields[] = {
    {"thread", TraceFieldType::UInt32, ROCKS_OFFSET_OF(OrderedEvent, thread)},
    {"sequence", TraceFieldType::UInt32, ROCKS_OFFSET_OF(OrderedEvent, sequence)},
};

TraceEventType OrderedType{"Test.Ordered", RecordChannel, sizeof(OrderedEvent), OrderedFields,
                           TraceEventFlags::Ordered};

// ---- 결정적 기록 생성기 ----

constexpr uint32 WorkerCount = 4;
constexpr uint32 RecordsPerPhase = 200;
constexpr uint32 OrderedInterval = 10;
// 블록 하나(4 KiB)보다 큰 가변부다. 여러 블록에 나눠 쓰는 경로를 지난다.
constexpr usize LargeSampleCount = 3000;

usize SampleCount(uint32 sequence) noexcept {
    return sequence % 50 == 49 ? LargeSampleCount : (sequence * 7) % 50;
}

void FillSamples(uint32 thread, uint32 sequence, uint32* samples, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        // 작은 값의 반복이라 LZ4가 줄인다(압축 덩어리 경로).
        samples[index] = static_cast<uint32>(index % 17) + thread * 1000 + sequence % 3;
    }
}

usize FormatLabel(uint32 thread, uint32 sequence, char* label, usize capacity) noexcept {
    const int count = snprintf(label, capacity, "worker-%u-record-%u", thread, sequence);
    return count > 0 ? static_cast<usize>(count) : 0;
}

SampleEvent MakeSample(uint32 thread, uint32 sequence) noexcept {
    SampleEvent event{};
    event.thread = thread;
    event.sequence = sequence;
    event.value = (static_cast<uint64>(thread) << 32) ^ (sequence * 0x9E3779B97F4A7C15ull);
    return event;
}

struct Worker {
    uint32 index = 0;
    uint64 osThreadId = 0;
    platform::ManualResetEvent* start = nullptr;
    platform::ManualResetEvent* resume = nullptr;
    platform::ManualResetEvent phaseDone;
    uint32 lost = 0;
};

void WriteRange(Worker& worker, uint32 first, uint32 end) noexcept {
    uint32 samples[LargeSampleCount];
    char label[64];
    for (uint32 sequence = first; sequence < end; ++sequence) {
        const SampleEvent event = MakeSample(worker.index, sequence);
        const usize count = SampleCount(sequence);
        FillSamples(worker.index, sequence, samples, count);
        const usize labelBytes = FormatLabel(worker.index, sequence, label, sizeof(label));
        const TraceVariablePart parts[] = {
            TraceVariablePart::FromString(StringView(label, labelBytes)),
            TraceVariablePart::FromSpan(Span<const uint32>(samples, count))};
        if (TraceWrite(SampleType, event, parts) != TraceWriteResult::Recorded) {
            ++worker.lost;
        }

        if (sequence % OrderedInterval == 0) {
            const OrderedEvent ordered{worker.index, sequence};
            if (TraceWrite(OrderedType, ordered) != TraceWriteResult::Recorded) {
                ++worker.lost;
            }
        }
    }
}

void RunWorker(void* context) noexcept {
    auto& worker = *static_cast<Worker*>(context);
    worker.osThreadId = platform::CurrentThreadId();
    WaitGate(*worker.start);
    WriteRange(worker, 0, RecordsPerPhase);
    worker.phaseDone.Signal();
    WaitGate(*worker.resume);
    WriteRange(worker, RecordsPerPhase, 2 * RecordsPerPhase);
}

// 한 스레드 스트림의 시험 기록이 생성기와 byte 단위로 같은지 본다. 순서 번호를 orders에 모은다.
bool CheckWorkerStream(const TraceStreamDecoder& decoder, const Worker& worker, uint16 sampleId,
                       uint16 orderedId, Array<uint64>& orders) noexcept {
    SystemAllocator scratch;
    bool matched = true;
    {
        Array<DecodedRecord> records(scratch);
        const uint32 traceThread = FindTraceThread(decoder, worker.osThreadId);
        if (traceThread == 0 || !decoder.DecodeRecords(ThreadStream(traceThread), records)) {
            return false;
        }

        uint32 samples[LargeSampleCount];
        char label[64];
        usize cursor = 0;
        uint64 previousOrder = 0;
        bool firstOrder = true;
        for (uint32 sequence = 0; sequence < 2 * RecordsPerPhase && matched; ++sequence) {
            if (cursor == records.Size()) {
                return false;
            }

            const DecodedRecord& sample = records[cursor++];
            const SampleEvent expected = MakeSample(worker.index, sequence);
            const usize count = SampleCount(sequence);
            FillSamples(worker.index, sequence, samples, count);
            const usize labelBytes = FormatLabel(worker.index, sequence, label, sizeof(label));
            matched =
                sample.typeId == sampleId && sample.variableCount == 2 &&
                sample.fixed.Size() == sizeof(expected) &&
                platform::MemoryEquals(sample.fixed.Data(), &expected, sizeof(expected)) &&
                sample.variable[0].Size() == labelBytes &&
                platform::MemoryEquals(sample.variable[0].Data(), label, labelBytes) &&
                sample.variable[1].Size() == count * sizeof(uint32) &&
                platform::MemoryEquals(sample.variable[1].Data(), samples, count * sizeof(uint32));
            if (sequence % OrderedInterval == 0 && matched) {
                if (cursor == records.Size()) {
                    return false;
                }

                const DecodedRecord& ordered = records[cursor++];
                matched = ordered.typeId == orderedId && ordered.hasOrder &&
                          ReadFixed<uint32>(ordered, 4) == sequence &&
                          (firstOrder || ordered.order > previousOrder) &&
                          static_cast<bool>(orders.TryPushBack(ordered.order));
                previousOrder = ordered.order;
                firstOrder = false;
            }
        }

        matched = matched && cursor == records.Size();
    }

    return matched;
}

} // namespace

// 여러 스레드가 동시에 기록한 byte가 파일에서 그대로 돌아온다(TR1 완료 기준 "여러 스레드 기록의
// 바이트 왕복"). 스레드 시작·중간·끝을 gate로 맞추고, 스레드가 살아 있는 중간에도 한 번 펌프해
// 은퇴하지 않은 블록의 부분 읽기와 이어 읽기를 지난다. 큰 가변부(블록 여러 개)와 압축 덩어리, 순서
// 번호도 확인한다.
TEST(TraceRoundTripsBytesAcrossThreads) {
    SystemAllocator allocator;
    TempTracePath path;
    {
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        REQUIRE(session.Start(settings));
        REQUIRE(session.RegisterChannel(RecordChannel, true));
        REQUIRE(session.RegisterEventType(SampleType));
        REQUIRE(session.RegisterEventType(OrderedType));
        REQUIRE(session.AttachFileOutput(path.path));
        const uint64 baseTick = session.BaseTick();
        const Guid guid = session.SessionGuid();

        platform::ManualResetEvent start;
        platform::ManualResetEvent resume;
        Worker workers[WorkerCount];
        platform::Thread threads[WorkerCount];
        for (uint32 index = 0; index < WorkerCount; ++index) {
            workers[index].index = index;
            workers[index].start = &start;
            workers[index].resume = &resume;
            REQUIRE(threads[index].Start(&RunWorker, &workers[index]) ==
                    platform::ThreadStatus::Success);
        }

        start.Signal();
        for (Worker& worker : workers) {
            WaitGate(worker.phaseDone);
        }

        // 모든 스레드가 살아 있고 멈춰 있는 동안 펌프한다.
        CHECK(session.Pump());
        resume.Signal();
        for (platform::Thread& thread : threads) {
            JoinThread(thread);
        }

        CHECK(session.Pump());
        const TraceSessionStats stats = session.Stats();
        CHECK(stats.lostRecords == 0);
        CHECK(stats.threadCount == WorkerCount);
        // 끝난 스레드의 블록은 모두 풀로 돌아왔다.
        CHECK(stats.poolBytesInUse == 0);
        CHECK(stats.poolBytesPeak > 0);
        for (const Worker& worker : workers) {
            CHECK(worker.lost == 0);
        }

        session.Stop();
        CHECK(!RecordChannel.IsEnabled());
        CHECK(RecordChannel.Id() == TraceChannel::NoId);
        CHECK(SampleType.Id() == TraceEventType::NoId);

        Array<uint8> file(allocator);
        REQUIRE(path.ReadAll(file));
        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(file.View()));
        CHECK(!decoder.Truncated());
        CHECK(decoder.CompressedChunkCount() > 0);
        CHECK(decoder.Header().baseTick == baseTick);
        CHECK(decoder.Header().tickFrequency == platform::MonotonicTickFrequency());
        CHECK(decoder.Header().sessionGuid == guid);
        CHECK(decoder.Header().flags == 0);
        CHECK(decoder.Header().attachOrderNumber == 0);
        REQUIRE(decoder.FindSchema(StringView("Test.Sample")) != nullptr);
        CHECK(decoder.FindSchema(StringView("Trace.Loss")) != nullptr);
        CHECK(decoder.FindSchema(StringView("Test.Ordered"))->IsOrdered());

        // 비정상 종료로 끝이 잘린 파일은 마지막 온전한 덩어리까지 읽고 잘림을 표시한다(OB03).
        TraceStreamDecoder truncated(allocator);
        CHECK(truncated.Decode(file.View().LeftChop(3)));
        CHECK(truncated.Truncated());
        CHECK(truncated.ChunkCount() + 1 == decoder.ChunkCount());

        const uint16 sampleId = decoder.FindSchema(StringView("Test.Sample"))->typeId;
        const uint16 orderedId = decoder.FindSchema(StringView("Test.Ordered"))->typeId;
        Array<uint64> orders(allocator);
        for (const Worker& worker : workers) {
            CHECK(CheckWorkerStream(decoder, worker, sampleId, orderedId, orders));
        }

        // 순서 번호는 모든 스레드에 걸쳐 0부터 빈틈없이 한 번씩 발급됐다.
        const usize orderedPerWorker = 2 * RecordsPerPhase / OrderedInterval;
        REQUIRE(orders.Size() == WorkerCount * orderedPerWorker);
        orders.Sort();
        for (usize index = 0; index < orders.Size(); ++index) {
            CHECK(orders[index] == index);
        }
    }
}

// 출력이 없을 때의 기록은 버려지고, 늦게 붙은 출력은 머리·스키마 전체·정의 저장소(채널·스레드
// 정보)를 먼저 받은 뒤 그 뒤 기록만 받는다. 스레드 스트림은 기록 경계에서 시작한다.
TEST(TraceLateOutputStartsWithDefinitionsAndRecordBoundary) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Manual;
        REQUIRE(session.Start(settings));
        REQUIRE(session.RegisterChannel(RecordChannel, true));
        REQUIRE(session.RegisterEventType(SampleType));
        REQUIRE(session.RegisterEventType(OrderedType));

        const uint32 before = 5;
        for (uint32 sequence = 0; sequence < before; ++sequence) {
            CHECK(TraceWrite(OrderedType, OrderedEvent{7, sequence}) == TraceWriteResult::Recorded);
        }

        CHECK(session.Pump());
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        const SampleEvent event = MakeSample(7, 100);
        const TraceVariablePart parts[] = {TraceVariablePart::FromString(StringView("late")),
                                           TraceVariablePart{}};
        CHECK(TraceWrite(SampleType, event, parts) == TraceWriteResult::Recorded);
        CHECK(TraceWrite(OrderedType, OrderedEvent{7, 101}) == TraceWriteResult::Recorded);
        const uint16 sampleId = SampleType.Id();
        const uint16 orderedId = OrderedType.Id();
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        CHECK(decoder.Header().attachOrderNumber == before);
        CHECK(decoder.Header().flags == 0);
        CHECK(decoder.FindSchema(sampleId) != nullptr);
        CHECK(decoder.FindSchema(TraceFormat::ChannelInfoTypeId) != nullptr);

        Array<DecodedRecord> definitions(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, definitions));
        bool sawChannel = false;
        for (const DecodedRecord& record : definitions) {
            if (record.typeId == TraceFormat::ChannelInfoTypeId &&
                StringView(reinterpret_cast<const char*>(record.variable[0].Data()),
                           record.variable[0].Size()) == StringView("test.record")) {
                sawChannel = true;
            }
        }

        CHECK(sawChannel);
        const uint32 traceThread = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(traceThread != 0);
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(traceThread), records));
        REQUIRE(records.Size() == 2);
        CHECK(records[0].typeId == sampleId);
        CHECK(ReadFixed<uint32>(records[0], 4) == 100);
        CHECK(records[1].typeId == orderedId);
        CHECK(records[1].order == before);
    }
}

TEST(TraceRegistrationRejectsInvalidDeclarations) {
    SystemAllocator allocator;
    TraceSession session;
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    CHECK(session.Pump().Error() == ErrorCode::WrongContext);
    CHECK(session.RegisterChannel(RecordChannel).Error() == ErrorCode::WrongContext);
    REQUIRE(session.Start(settings));
    CHECK(session.Start(settings).Error() == ErrorCode::AlreadyExists);
    {
        TraceSession other;
        CHECK(other.Start(settings).Error() == ErrorCode::Busy);
    }

    TraceChannel upper{"Bad", ""};
    TraceChannel emptySegment{"a..b", ""};
    TraceChannel leadingDot{".a", ""};
    TraceChannel trailingDot{"a.", ""};
    TraceChannel leadingDigit{"1a", ""};
    CHECK(session.RegisterChannel(upper).Error() == ErrorCode::InvalidArgument);
    CHECK(session.RegisterChannel(emptySegment).Error() == ErrorCode::InvalidArgument);
    CHECK(session.RegisterChannel(leadingDot).Error() == ErrorCode::InvalidArgument);
    CHECK(session.RegisterChannel(trailingDot).Error() == ErrorCode::InvalidArgument);
    CHECK(session.RegisterChannel(leadingDigit).Error() == ErrorCode::InvalidArgument);

    REQUIRE(session.RegisterChannel(RecordChannel, true));
    CHECK(session.RegisterChannel(RecordChannel).Error() == ErrorCode::AlreadyExists);
    TraceChannel sameName{"test.record", "같은 이름"};
    CHECK(session.RegisterChannel(sameName).Error() == ErrorCode::AlreadyExists);

    // 등록하지 않은 채널의 형은 등록할 수 없고, 등록 전에는 기록해도 아무것도 하지 않는다.
    TraceChannel unregistered{"test.unregistered", ""};
    TraceEventType orphan{"Test.Orphan", unregistered};
    CHECK(session.RegisterEventType(orphan).Error() == ErrorCode::NotFound);
    const TraceVariablePart empty[] = {TraceVariablePart{}, TraceVariablePart{}};
    CHECK(TraceWrite(SampleType, MakeSample(0, 0), empty) == TraceWriteResult::Disabled);

    constexpr TraceField OutOfRange[] = {{"value", TraceFieldType::UInt64, 4}};
    TraceEventType outOfRange{"Test.OutOfRange", RecordChannel, 8, OutOfRange};
    CHECK(session.RegisterEventType(outOfRange).Error() == ErrorCode::InvalidArgument);
    constexpr TraceField VariableWithOffset[] = {{"text", TraceFieldType::String, 0}};
    TraceEventType variableWithOffset{"Test.VariableOffset", RecordChannel, 8, VariableWithOffset};
    CHECK(session.RegisterEventType(variableWithOffset).Error() == ErrorCode::InvalidArgument);
    constexpr TraceField Duplicate[] = {{"value", TraceFieldType::UInt32, 0},
                                        {"value", TraceFieldType::UInt32, 4}};
    TraceEventType duplicate{"Test.Duplicate", RecordChannel, 8, Duplicate};
    CHECK(session.RegisterEventType(duplicate).Error() == ErrorCode::InvalidArgument);
    TraceEventType badName{"9Bad", RecordChannel};
    CHECK(session.RegisterEventType(badName).Error() == ErrorCode::InvalidArgument);

    // 고정부가 블록 하나에 들어가지 않는 형은 등록 때 오류다(기록 때 멈추지 않는다).
    constexpr TraceField Huge[] = {{"head", TraceFieldType::UInt8, 0}};
    TraceEventType huge{"Test.Huge", RecordChannel, TraceBlockLayout::PayloadBytes, Huge};
    CHECK(session.RegisterEventType(huge).Error() == ErrorCode::CapacityExceeded);

    REQUIRE(session.RegisterEventType(SampleType));
    CHECK(session.RegisterEventType(SampleType).Error() == ErrorCode::AlreadyExists);
    TraceEventType sameSchema{"Test.Sample", RecordChannel, sizeof(SampleEvent), SampleFields};
    CHECK(session.RegisterEventType(sameSchema).Error() == ErrorCode::AlreadyExists);
    TraceEventType otherSchema{"Test.Sample", RecordChannel, sizeof(OrderedEvent), OrderedFields};
    CHECK(session.RegisterEventType(otherSchema).Error() == ErrorCode::TypeMismatch);

    // 읽기 전용 채널은 등록 때 상태로 고정된다.
    REQUIRE(session.RegisterChannel(ReadOnlyChannel, true));
    CHECK(session.SetChannelEnabled(ReadOnlyChannel, false).Error() == ErrorCode::Unsupported);
    CHECK(ReadOnlyChannel.IsEnabled());

    CHECK(session.SetChannelEnabled(StringView("test.record"), false));
    CHECK(!RecordChannel.IsEnabled());
    CHECK(TraceWrite(SampleType, MakeSample(0, 0), empty) == TraceWriteResult::Disabled);
    CHECK(session.SetChannelEnabled(StringView("test.missing"), true).Error() ==
          ErrorCode::NotFound);
    CHECK(session.SetChannelEnabled(RecordChannel, true));
    CHECK(RecordChannel.IsEnabled());

    // 형이 남아 있는 채널은 풀 수 없다. 형을 먼저 풀면 채널도 풀린다.
    CHECK(session.UnregisterChannel(RecordChannel).Error() == ErrorCode::Busy);
    CHECK(session.UnregisterEventType(SampleType));
    CHECK(SampleType.Id() == TraceEventType::NoId);
    CHECK(session.UnregisterEventType(SampleType).Error() == ErrorCode::NotFound);
    CHECK(session.UnregisterChannel(RecordChannel));
    CHECK(RecordChannel.Id() == TraceChannel::NoId);
    CHECK(!RecordChannel.IsEnabled());
    CHECK(session.UnregisterChannel(RecordChannel).Error() == ErrorCode::NotFound);

    session.Stop();
    CHECK(!ReadOnlyChannel.IsEnabled());
    CHECK(ReadOnlyChannel.Id() == TraceChannel::NoId);
    CHECK(session.Pump().Error() == ErrorCode::WrongContext);
}

// 꺼진 채널의 기록 비용(relaxed 읽기와 분기 하나)을 재서 기록만 한다. 시간 문턱으로 판정하지
// 않는다.
TEST(TraceDisabledChannelCostIsMeasured) {
    SystemAllocator allocator;
    TraceSession session;
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    REQUIRE(session.Start(settings));
    REQUIRE(session.RegisterChannel(RecordChannel, false));
    REQUIRE(session.RegisterEventType(OrderedType));

    constexpr uint32 Iterations = 1000000;
    uint32 disabled = 0;
    const uint64 start = platform::MonotonicTicks();
    for (uint32 index = 0; index < Iterations; ++index) {
        if (TraceWrite(OrderedType, OrderedEvent{0, index}) == TraceWriteResult::Disabled) {
            ++disabled;
        }
    }

    const uint64 elapsed = platform::MonotonicTicks() - start;
    CHECK(disabled == Iterations);
    const double nanoseconds = static_cast<double>(elapsed) * 1e9 /
                               static_cast<double>(platform::MonotonicTickFrequency()) /
                               static_cast<double>(Iterations);
    printf("trace disabled channel: %.3f ns per TraceWrite (%u calls)\n", nanoseconds, Iterations);

    // 켜진 채널의 기록 1회(고정부 8 byte, 순서 번호)와 정적 이름 CPU 구간 1쌍(깊이 0이라 쌍마다
    // 배치 이벤트 하나)의 비용도 같은 방식으로 기록만 한다.
    constexpr uint32 EnabledIterations = 100000;
    REQUIRE(session.SetChannelEnabled(RecordChannel, true));
    uint32 recorded = 0;
    const uint64 writeStart = platform::MonotonicTicks();
    for (uint32 index = 0; index < EnabledIterations; ++index) {
        if (TraceWrite(OrderedType, OrderedEvent{0, index}) == TraceWriteResult::Recorded) {
            ++recorded;
        }
    }

    const uint64 writeElapsed = platform::MonotonicTicks() - writeStart;
    CHECK(recorded == EnabledIterations);
    REQUIRE(session.Pump());
    REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
    const uint64 scopeStart = platform::MonotonicTicks();
    for (uint32 index = 0; index < EnabledIterations; ++index) {
        ROCKS_TRACE_CPU_SCOPE("Test.Cost");
    }

    const uint64 scopeElapsed = platform::MonotonicTicks() - scopeStart;
    CHECK(session.Stats().lostRecords == 0);
    const double frequency = static_cast<double>(platform::MonotonicTickFrequency());
    printf("trace enabled: %.3f ns per TraceWrite, %.3f ns per CpuScope pair (%u each)\n",
           static_cast<double>(writeElapsed) * 1e9 / frequency / EnabledIterations,
           static_cast<double>(scopeElapsed) * 1e9 / frequency / EnabledIterations,
           EnabledIterations);
    session.Stop();
}
