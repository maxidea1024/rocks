#include "TraceCaptureTestSupport.h"

#include <TestSupport/Test.h>

using namespace rocks;
using namespace rocks::tracetest;

namespace {

TraceChannel FillChannel{"test.fill", "풀 고갈 시험"};
TraceChannel FillOtherChannel{"test.fill.other", "풀 고갈 시험(두 번째 채널)"};
TraceChannel NameChannel{"test.names", "정의 기록 시험"};

// 기록 하나가 2 + 64 = 66 byte인 형이다. 블록 하나에 PayloadBytes / 66개가 들어간다.
struct FillEvent {
    uint8 bytes[64];
};

constexpr TraceField FillFields[] = {{"head", TraceFieldType::UInt64, 0}};

TraceEventType FillType{"Test.Fill", FillChannel, sizeof(FillEvent), FillFields};
TraceEventType FillOtherType{"Test.FillOther", FillOtherChannel, sizeof(FillEvent), FillFields};

constexpr usize FillRecordBytes = sizeof(uint16) + sizeof(FillEvent);
constexpr usize FillRecordsPerBlock = TraceBlockLayout::PayloadBytes / FillRecordBytes;
constexpr usize FillRecordsPerPage = FillRecordsPerBlock * TraceBlockLayout::PageBlocks;

struct NameDefinition {
    uint32 id;
};

constexpr TraceField NameFields[] = {
    {"id", TraceFieldType::UInt32, 0},
    {"name", TraceFieldType::String, TraceField::Variable},
};

TraceEventType NameType{"Test.Name", NameChannel, sizeof(NameDefinition), NameFields,
                        TraceEventFlags::Definition};

FillEvent MakeFill(uint32 value) noexcept {
    FillEvent event{};
    for (usize index = 0; index < sizeof(event.bytes); ++index) {
        event.bytes[index] = static_cast<uint8>(value + index);
    }

    return event;
}

TraceSessionSettings ManualSettings(Allocator& allocator) noexcept {
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    return settings;
}

bool RegisterFill(TraceSession& session) noexcept {
    return session.RegisterChannel(FillChannel, true) &&
           session.RegisterChannel(FillOtherChannel, true) && session.RegisterEventType(FillType) &&
           session.RegisterEventType(FillOtherType);
}

// 기록 열에서 형 id가 typeId인 기록 수다.
usize CountType(const Array<DecodedRecord>& records, uint16 typeId) noexcept {
    usize count = 0;
    for (const DecodedRecord& record : records) {
        if (record.typeId == typeId) {
            ++count;
        }
    }

    return count;
}

// 손실 기록들의 채널별 수를 더한다.
uint64 SumLoss(const Array<DecodedRecord>& records) noexcept {
    uint64 total = 0;
    for (const DecodedRecord& record : records) {
        if (record.typeId == TraceFormat::LossTypeId) {
            for (usize index = 0; index < record.variable[1].Size() / sizeof(uint64); ++index) {
                total +=
                    LoadLittleEndian<uint64>(record.variable[1].Data() + index * sizeof(uint64));
            }
        }
    }

    return total;
}

} // namespace

// 풀 상한에서 기록은 기다리지 않고 버려지고, 손실 수(채널별)와 손실 구간이 정확히 남는다. 수동 펌프
// 모드에서는 펌프하지 않는 한 블록이 회수되지 않으므로(gate) 고갈 시점이 결정적이다.
TEST(TracePoolLimitLossIsExactAndNonBlocking) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings = ManualSettings(allocator);
        settings.poolMaxBytes = TraceBlockLayout::PageBytes;
        REQUIRE(session.Start(settings));
        REQUIRE(RegisterFill(session));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        const uint16 fillChannel = FillChannel.Id();
        const uint16 otherChannel = FillOtherChannel.Id();
        const uint16 fillId = FillType.Id();

        for (usize index = 0; index < FillRecordsPerPage; ++index) {
            CHECK(TraceWrite(FillType, MakeFill(static_cast<uint32>(index))) ==
                  TraceWriteResult::Recorded);
        }

        CHECK(session.Stats().poolBytesInUse == TraceBlockLayout::PageBytes);

        // 풀이 가득 찼다. 두 채널에 번갈아 기록하면 모두 즉시 버려진다.
        constexpr uint32 LostPerChannel = 25;
        const uint64 lossStartLow = platform::MonotonicTicks();
        CHECK(TraceWrite(FillType, MakeFill(0)) == TraceWriteResult::Lost);
        const uint64 lossStartHigh = platform::MonotonicTicks();
        CHECK(TraceWrite(FillOtherType, MakeFill(0)) == TraceWriteResult::Lost);
        for (uint32 index = 1; index < LostPerChannel; ++index) {
            CHECK(TraceWrite(FillType, MakeFill(index)) == TraceWriteResult::Lost);
            CHECK(TraceWrite(FillOtherType, MakeFill(index)) == TraceWriteResult::Lost);
        }

        CHECK(session.Stats().lostRecords == 2 * LostPerChannel);

        // 펌프가 은퇴 블록 15개를 돌려준다(스레드가 쓰는 중인 마지막 블록은 남는다).
        CHECK(session.Pump());
        CHECK(session.Stats().poolBytesInUse == TraceBlockLayout::BlockBytes);
        const uint64 lossEndLow = platform::MonotonicTicks();
        CHECK(TraceWrite(FillType, MakeFill(1000)) == TraceWriteResult::Recorded);
        const uint64 lossEndHigh = platform::MonotonicTicks();
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        const uint32 traceThread = FindTraceThread(decoder, platform::CurrentThreadId());
        REQUIRE(traceThread != 0);
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(traceThread), records));
        REQUIRE(records.Size() == FillRecordsPerPage + 2);
        CHECK(CountType(records, fillId) == FillRecordsPerPage + 1);

        // 손실 기록은 다시 블록을 받은 뒤의 첫 기록이다.
        const DecodedRecord& loss = records[FillRecordsPerPage];
        REQUIRE(loss.typeId == TraceFormat::LossTypeId);
        const uint64 lossStart = ReadFixed<uint64>(loss, TraceFormat::LossStartTickOffset);
        const uint64 lossEnd = ReadFixed<uint64>(loss, TraceFormat::LossEndTickOffset);
        CHECK(lossStart >= lossStartLow);
        CHECK(lossStart <= lossStartHigh);
        CHECK(lossEnd >= lossEndLow);
        CHECK(lossEnd <= lossEndHigh);
        REQUIRE(loss.variable[0].Size() == 2 * sizeof(uint16));
        REQUIRE(loss.variable[1].Size() == 2 * sizeof(uint64));
        CHECK(LoadLittleEndian<uint16>(loss.variable[0].Data()) == fillChannel);
        CHECK(LoadLittleEndian<uint16>(loss.variable[0].Data() + sizeof(uint16)) == otherChannel);
        CHECK(LoadLittleEndian<uint64>(loss.variable[1].Data()) == LostPerChannel);
        CHECK(LoadLittleEndian<uint64>(loss.variable[1].Data() + sizeof(uint64)) == LostPerChannel);
        CHECK(records[FillRecordsPerPage + 1].typeId == fillId);
        CHECK(ReadFixedByte(records[FillRecordsPerPage + 1], 0) == static_cast<uint8>(1000));
    }
}

namespace {

struct FillWorker {
    platform::ManualResetEvent* start = nullptr;
    uint64 osThreadId = 0;
    uint32 attempts = 0;
    uint32 recorded = 0;
    uint32 lost = 0;
};

void RunFillWorker(void* context) noexcept {
    auto& worker = *static_cast<FillWorker*>(context);
    worker.osThreadId = platform::CurrentThreadId();
    WaitGate(*worker.start);
    for (uint32 index = 0; index < worker.attempts; ++index) {
        if (TraceWrite(FillType, MakeFill(index)) == TraceWriteResult::Recorded) {
            ++worker.recorded;
        } else {
            ++worker.lost;
        }
    }
}

} // namespace

// 여러 스레드가 같은 풀을 고갈시킨다. 누가 얼마나 기록할지는 경쟁에 따르지만, 스레드마다 "스트림의
// 기록 수 = 성공 수"와 "손실 기록의 합 = 실패 수"는 경쟁과 무관하게 성립해야 한다. 고갈된 채 끝난
// 스레드의 손실 기록은 writer가 대신 쓴다.
TEST(TracePoolLimitLossIsAccountedPerThread) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings = ManualSettings(allocator);
        settings.poolMaxBytes = TraceBlockLayout::PageBytes;
        REQUIRE(session.Start(settings));
        REQUIRE(RegisterFill(session));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        const uint16 fillId = FillType.Id();

        constexpr usize WorkerCount = 3;
        constexpr uint32 Attempts = 600;
        platform::ManualResetEvent start;
        FillWorker workers[WorkerCount];
        platform::Thread threads[WorkerCount];
        for (usize index = 0; index < WorkerCount; ++index) {
            workers[index].start = &start;
            workers[index].attempts = Attempts;
            REQUIRE(threads[index].Start(&RunFillWorker, &workers[index]) ==
                    platform::ThreadStatus::Success);
        }

        start.Signal();
        for (platform::Thread& thread : threads) {
            JoinThread(thread);
        }

        uint64 recorded = 0;
        uint64 lost = 0;
        for (const FillWorker& worker : workers) {
            recorded += worker.recorded;
            lost += worker.lost;
        }

        CHECK(recorded + lost == WorkerCount * Attempts);
        CHECK(recorded <= FillRecordsPerPage);
        CHECK(lost > 0);
        CHECK(session.Stats().lostRecords == lost);
        CHECK(session.Pump());
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        for (const FillWorker& worker : workers) {
            const uint32 traceThread = FindTraceThread(decoder, worker.osThreadId);
            REQUIRE(traceThread != 0);
            Array<DecodedRecord> records(allocator);
            REQUIRE(decoder.DecodeRecords(ThreadStream(traceThread), records));
            CHECK(CountType(records, fillId) == worker.recorded);
            CHECK(SumLoss(records) == worker.lost);
        }
    }
}

namespace {

struct BlockWorker {
    uint32 records = 0;
};

void RunBlockWorker(void* context) noexcept {
    auto& worker = *static_cast<BlockWorker*>(context);
    for (uint32 index = 0; index < worker.records; ++index) {
        if (TraceWrite(FillType, MakeFill(index)) != TraceWriteResult::Recorded) {
            platform::Fatal("Trace block worker lost a record");
        }
    }
}

} // namespace

// 끝난 스레드의 블록(마지막 블록 포함)은 writer가 내용을 보낸 뒤 풀로 돌아온다. Join이 TLS 정리까지
// 기다리므로 펌프 전 상태가 결정적이다.
TEST(TraceExitedThreadBlocksReturnToPool) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        REQUIRE(session.Start(ManualSettings(allocator)));
        REQUIRE(RegisterFill(session));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        const uint16 fillId = FillType.Id();

        constexpr usize Blocks = 3;
        BlockWorker worker;
        worker.records = static_cast<uint32>(FillRecordsPerBlock * Blocks);
        platform::Thread thread;
        REQUIRE(thread.Start(&RunBlockWorker, &worker) == platform::ThreadStatus::Success);
        JoinThread(thread);

        CHECK(session.Stats().poolBytesInUse == Blocks * TraceBlockLayout::BlockBytes);
        CHECK(session.Pump());
        CHECK(session.Stats().poolBytesInUse == 0);
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        bool found = false;
        for (const DecodedStream& stream : decoder.Streams()) {
            if (stream.id >= TraceFormat::FirstThreadStreamId) {
                Array<DecodedRecord> records(allocator);
                REQUIRE(decoder.DecodeRecords(stream.id, records));
                CHECK(CountType(records, fillId) == worker.records);
                found = true;
            }
        }

        CHECK(found);
    }
}

// 정의 저장소가 상한을 넘으면 넘친 정의는 이미 붙은 출력에만 가고, 그 수가 세어지며, 그 뒤 붙은
// 출력은 "정의 일부 없음" 표시를 받는다(4.3절, 결정 T2).
TEST(TraceDefinitionStoreOverflowIsMarked) {
    SystemAllocator allocator;
    {
        MemoryOutput early(allocator);
        MemoryOutput late(allocator);
        TraceSession session;
        TraceSessionSettings settings = ManualSettings(allocator);
        settings.definitionStoreMaxBytes = 1024;
        REQUIRE(session.Start(settings));
        REQUIRE(session.RegisterChannel(NameChannel, true));
        REQUIRE(session.RegisterEventType(NameType));
        CHECK(session.Pump());
        CHECK(session.Stats().definitionStoreBytes > 0);
        CHECK(session.Stats().definitionRecordsNotStored == 0);
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &early));

        constexpr uint32 Names = 64;
        char text[64];
        for (uint32 index = 0; index < Names; ++index) {
            // 압축으로 줄지 않도록 섞은 16진 이름을 쓴다(저장소 상한 비교는 압축된 크기 기준이다).
            const unsigned long long mixed = (index + 1) * 0x9E3779B97F4A7C15ull;
            const int length =
                snprintf(text, sizeof(text), "name-%016llx-%016llx", mixed, mixed ^ (mixed >> 29));
            const TraceVariablePart parts[] = {
                TraceVariablePart::FromString(StringView(text, static_cast<usize>(length)))};
            CHECK(TraceWrite(NameType, NameDefinition{index}, parts) == TraceWriteResult::Recorded);
        }

        CHECK(session.Pump());
        CHECK(session.Stats().definitionRecordsNotStored == Names);
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &late));
        const uint16 nameId = NameType.Id();
        session.Stop();

        TraceStreamDecoder earlyDecoder(allocator);
        TraceStreamDecoder lateDecoder(allocator);
        REQUIRE(earlyDecoder.Decode(early.bytes.View()));
        REQUIRE(lateDecoder.Decode(late.bytes.View()));
        CHECK(earlyDecoder.Header().flags == 0);
        CHECK(lateDecoder.Header().flags == TraceFormat::HeaderFlagDefinitionsIncomplete);

        Array<DecodedRecord> earlyRecords(allocator);
        Array<DecodedRecord> lateRecords(allocator);
        REQUIRE(earlyDecoder.DecodeRecords(TraceFormat::DefinitionStreamId, earlyRecords));
        REQUIRE(lateDecoder.DecodeRecords(TraceFormat::DefinitionStreamId, lateRecords));
        CHECK(CountType(earlyRecords, nameId) == Names);
        CHECK(CountType(lateRecords, nameId) == 0);
        // 저장된 채널 정보(표준 채널 포함)는 늦게 붙은 출력도 똑같이 받는다.
        const usize channelInfos = CountType(earlyRecords, TraceFormat::ChannelInfoTypeId);
        CHECK(channelInfos > 0);
        CHECK(CountType(lateRecords, TraceFormat::ChannelInfoTypeId) == channelInfos);
    }
}

// 정의 버퍼가 차면 정의 기록은 기다리지 않고 버려지고, writer가 DefinitionLoss 기록으로 누계를
// 알린다.
TEST(TraceDefinitionBufferFullIsCountedAndReported) {
    SystemAllocator allocator;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings = ManualSettings(allocator);
        settings.definitionBufferBytes = 4 * TraceFormat::MaxDefinitionRecordBytes;
        REQUIRE(session.Start(settings));
        REQUIRE(session.RegisterChannel(NameChannel, true));
        REQUIRE(session.RegisterEventType(NameType));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
        // 채널 정보를 비워 버퍼를 처음으로 되돌린다.
        CHECK(session.Pump());

        // 기록 하나 = 머리 4 + (형 id 2 + 고정부 4 + 길이 4 + 이름 8000) = 8014 → 4 byte 정렬 8016.
        constexpr usize NameBytes = 8000;
        constexpr usize SlotBytes = 4 + ((2 + 4 + 4 + NameBytes + 3) & ~usize{3});
        constexpr usize Fits = 4 * TraceFormat::MaxDefinitionRecordBytes / SlotBytes;
        constexpr usize Attempts = Fits + 2;
        static char name[NameBytes];
        for (char& c : name) {
            c = 'n';
        }

        const TraceVariablePart parts[] = {
            TraceVariablePart::FromString(StringView(name, NameBytes))};
        for (usize index = 0; index < Attempts; ++index) {
            const TraceWriteResult expected =
                index < Fits ? TraceWriteResult::Recorded : TraceWriteResult::Lost;
            CHECK(TraceWrite(NameType, NameDefinition{static_cast<uint32>(index)}, parts) ==
                  expected);
        }

        CHECK(session.Stats().definitionRecordsDropped == Attempts - Fits);
        CHECK(session.Pump());
        // 비운 뒤에는 다시 쓸 수 있다.
        CHECK(TraceWrite(NameType, NameDefinition{99}, parts) == TraceWriteResult::Recorded);
        const uint16 nameId = NameType.Id();
        session.Stop();

        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(output.bytes.View()));
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(TraceFormat::DefinitionStreamId, records));
        CHECK(CountType(records, nameId) == Fits + 1);
        usize reports = 0;
        for (const DecodedRecord& record : records) {
            if (record.typeId == TraceFormat::DefinitionLossTypeId) {
                ++reports;
                CHECK(ReadFixed<uint64>(record, TraceFormat::DefinitionLossCountOffset) ==
                      Attempts - Fits);
            }
        }

        CHECK(reports == 1);
    }
}

namespace {

struct StopContext {
    TraceSession* session = nullptr;
};

void RunStop(void* context) noexcept {
    static_cast<StopContext*>(context)->session->Stop();
}

struct GatedWriter {
    TraceWriteResult result = TraceWriteResult::Disabled;
    uint64 osThreadId = 0;
    platform::ManualResetEvent ready;
    platform::ManualResetEvent go;
};

void RunGatedWriter(void* context) noexcept {
    auto& writer = *static_cast<GatedWriter*>(context);
    writer.osThreadId = platform::CurrentThreadId();
    writer.ready.Signal();
    WaitGate(writer.go);
    writer.result = TraceWrite(FillType, MakeFill(42));
}

} // namespace

// 종료 순서(계획 29.4절): 1) 입장 차단 2) 진행 중 기록 완료 대기 3) 마지막 비우기 4) 출력 닫기
// 5) writer join 6) 해제. 기록 한가운데(블록 풀의 새 페이지 확보)에서 멈춘 스레드가 있는 채 Stop을
// 시작하고, Stop이 채널을 끈 것(1단계)을 본 뒤에 그 기록을 풀어 준다. Stop은 그 기록이 끝나기를
// 기다렸다가 비우므로, 그 기록은 닫힌 파일과 콜백 출력에 모두 있어야 한다. writer 스레드 모드다.
TEST(TraceStopWaitsForInFlightWriteThenFlushesAndCloses) {
    GatedAllocator allocator;
    TempTracePath path;
    {
        MemoryOutput output(allocator);
        TraceSession session;
        TraceSessionSettings settings;
        settings.allocator = &allocator;
        settings.writerMode = TraceWriterMode::Thread;
        REQUIRE(session.Start(settings));
        REQUIRE(RegisterFill(session));
        REQUIRE(session.AttachFileOutput(path.path));
        REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));

        GatedWriter writer;
        platform::Thread writerThread;
        REQUIRE(writerThread.Start(&RunGatedWriter, &writer) == platform::ThreadStatus::Success);
        WaitGate(writer.ready);
        allocator.Arm(writer.osThreadId, TraceBlockLayout::PageBytes);
        writer.go.Signal();
        WaitGate(allocator.entered);

        StopContext stop{&session};
        platform::Thread stopThread;
        REQUIRE(stopThread.Start(&RunStop, &stop) == platform::ThreadStatus::Success);
        // Stop 1단계(채널 끄기)를 본다. 시간이 아니라 상태를 기다린다.
        while (FillChannel.IsEnabled()) {
            platform::YieldThread();
        }

        allocator.release.Signal();
        JoinThread(writerThread);
        JoinThread(stopThread);
        CHECK(writer.result == TraceWriteResult::Recorded);
        CHECK(!session.IsRunning());
        CHECK(FillType.Id() == TraceEventType::NoId);
        CHECK(TraceWrite(FillType, MakeFill(0)) == TraceWriteResult::Disabled);

        Array<uint8> file(allocator);
        REQUIRE(path.ReadAll(file));
        // 파일과 콜백은 같은 byte 열을 받는다.
        CHECK(file.Size() == output.bytes.Size() &&
              platform::MemoryEquals(file.Data(), output.bytes.Data(), file.Size()));
        TraceStreamDecoder decoder(allocator);
        REQUIRE(decoder.Decode(file.View()));
        CHECK(!decoder.Truncated());
        const uint16 fillId = decoder.FindSchema(StringView("Test.Fill"))->typeId;
        const uint32 traceThread = FindTraceThread(decoder, writer.osThreadId);
        REQUIRE(traceThread != 0);
        Array<DecodedRecord> records(allocator);
        REQUIRE(decoder.DecodeRecords(ThreadStream(traceThread), records));
        REQUIRE(records.Size() == 1);
        CHECK(records[0].typeId == fillId);
        CHECK(ReadFixedByte(records[0], 0) == 42);
    }
}
