#include "TraceCaptureTestSupport.h"

#include <TestSupport/Test.h>
#include <TraceAnalysis/TraceModel.h>
#include <TraceCapture/TraceCpuScope.h>

// TR5 시험이다(Docs/Design/TraceSystem.ko.md 8절): 최근 기록 고리의 넘침 범위, 스냅샷만으로 분석,
// writer 잠금 제한 시간 초과의 "불완전" 표시, 풀 사용률 경고, 수집 통계 기록. 모두 Manual writer와
// 명시 gate로 순서를 정한다.
using namespace rocks;
namespace trace_recent_tests_file {

using namespace rocks::tracetest;

namespace {

using Format = TraceStandardFormat;

TraceChannel RecentChannel{"test.recent", "Recent ring test records"};

// u32 순번 | 가변: 내용(UInt8Array)
constexpr TraceField RecentFields[] = {
    {"sequence", TraceFieldType::UInt32, 0},
    {"payload", TraceFieldType::UInt8Array, TraceField::Variable},
};

TraceEventType RecentType{"Test.Recent", RecentChannel, sizeof(uint32), RecentFields};

TraceScopeName SnapshotScope{"Test.SnapshotScope"};

struct RecentFixed {
    uint32 sequence = 0;
};

// 압축되지 않는 결정적 내용(xorshift)이다. 덩어리 크기를 정확히 셀 수 있게 한다.
void FillNoise(uint8* bytes, usize count, uint32 seed) noexcept {
    uint32 state = seed * 2654435761u + 1;
    for (usize index = 0; index < count; ++index) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[index] = static_cast<uint8>(state);
    }
}

constexpr usize PayloadBytes = 4000;
// 기록 하나: 형 id(2) + 고정부(4) + 길이(4) + 내용.
constexpr usize RecordBytes = sizeof(uint16) + sizeof(uint32) + sizeof(uint32) + PayloadBytes;
// 압축하지 않은 덩어리 하나(머리 포함)와 고리 칸(표시 1 byte 포함)이다.
constexpr usize ChunkBytes = TraceFormat::ChunkHeaderBytes + RecordBytes;
constexpr usize EntryBytes = 1 + ChunkBytes;

void WriteRecent(uint32 sequence) noexcept {
    uint8 payload[PayloadBytes];
    FillNoise(payload, sizeof(payload), sequence);
    const TraceVariablePart parts[] = {
        TraceVariablePart::FromSpan(Span<const uint8>(payload, sizeof(payload)))};
    RecentFixed fixed;
    fixed.sequence = sequence;
    CHECK(TraceWrite(RecentType, fixed, parts) == TraceWriteResult::Recorded);
}

TraceSessionSettings ManualSettings(Allocator& allocator) noexcept {
    TraceSessionSettings settings;
    settings.allocator = &allocator;
    settings.writerMode = TraceWriterMode::Manual;
    settings.recentBytes = TraceSessionSettings::MinRecentBytes;
    return settings;
}

uint16 TypeOf(const TraceStreamDecoder& decoder, const char* name) noexcept {
    const DecodedSchema* schema = decoder.FindSchema(StringView(name));
    return schema != nullptr ? schema->typeId : TraceFormat::InvalidTypeId;
}

// 디코드한 스냅샷의 모든 스레드 스트림에서 Test.Recent 순번을 차례로 모은다.
bool CollectSequences(const TraceStreamDecoder& decoder, Array<uint32>& out) noexcept {
    SystemAllocator scratch;
    const uint16 type = TypeOf(decoder, "Test.Recent");
    for (const DecodedStream& stream : decoder.Streams()) {
        if (stream.id < TraceFormat::FirstThreadStreamId) {
            continue;
        }

        Array<DecodedRecord> records(scratch);
        if (!decoder.DecodeRecords(stream.id, records)) {
            return false;
        }

        for (const DecodedRecord& record : records) {
            if (record.typeId == type && !out.TryPushBack(ReadFixed<uint32>(record, 0))) {
                return false;
            }
        }
    }

    return true;
}

// 정의 스트림의 동기 표지(Trace.SnapshotSync)를 찾는다.
bool FindSync(const TraceStreamDecoder& decoder, DecodedRecord& out) noexcept {
    SystemAllocator scratch;
    Array<DecodedRecord> records(scratch);
    if (!decoder.DecodeRecords(TraceFormat::DefinitionStreamId, records)) {
        return false;
    }

    const uint16 type = TypeOf(decoder, "Trace.SnapshotSync");
    bool found = false;
    for (const DecodedRecord& record : records) {
        if (record.typeId == type) {
            out = record;
            found = true;
        }
    }

    return found;
}

uint32 HeaderFlags(const Array<uint8>& bytes) noexcept {
    return LoadLittleEndian<uint32>(bytes.Data() + TraceFormat::HeaderFlagsOffset);
}

} // namespace

// 고리가 넘치면 가장 오래된 덩어리부터 버리고 버린 덩어리·byte를 정확히 센다. 스냅샷은 남은
// 덩어리(가장 최근 기록)만 담고, 머리와 동기 표지에 넘침을 표시한다.
TEST(TraceRecent_OverflowDropsOldestChunksAndCounts) {
    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    REQUIRE(session.RegisterChannel(RecentChannel, true));
    REQUIRE(session.RegisterEventType(RecentType));

    // 펌프마다 기록 하나 = 덩어리 하나다. 고리에는 칸 Capacity / EntryBytes개가 들어간다.
    constexpr uint32 Writes = 12;
    constexpr uint64 Kept = TraceSessionSettings::MinRecentBytes / EntryBytes;
    for (uint32 sequence = 0; sequence < Writes; ++sequence) {
        WriteRecent(sequence);
        REQUIRE(session.Pump());
    }

    const TraceSessionStats stats = session.Stats();
    CHECK(stats.recentCapacity == TraceSessionSettings::MinRecentBytes);
    CHECK(stats.recentChunks == Kept);
    CHECK(stats.recentBytes == Kept * EntryBytes);
    CHECK(stats.recentDroppedChunks == Writes - Kept);
    CHECK(stats.recentDroppedBytes == (Writes - Kept) * ChunkBytes);

    MemoryOutput output(allocator);
    auto snapshot = session.WriteSnapshot(&MemoryOutput::Write, &output);
    REQUIRE(snapshot);
    CHECK(snapshot.Value().complete);
    CHECK(snapshot.Value().recentChunks == Kept);
    CHECK(snapshot.Value().skippedChunks == 0);
    CHECK(snapshot.Value().droppedChunks == Writes - Kept);
    CHECK(snapshot.Value().bytes == output.bytes.Size());
    session.Stop();

    const uint32 flags = HeaderFlags(output.bytes);
    CHECK((flags & TraceFormat::HeaderFlagSnapshot) != 0);
    CHECK((flags & TraceFormat::HeaderFlagRecentDropped) != 0);
    CHECK((flags & TraceFormat::HeaderFlagSnapshotIncomplete) == 0);
    TraceStreamDecoder decoder(allocator);
    REQUIRE(decoder.Decode(output.bytes.View()));
    Array<uint32> sequences(allocator);
    REQUIRE(CollectSequences(decoder, sequences));
    REQUIRE(sequences.Size() == Kept);
    for (usize index = 0; index < sequences.Size(); ++index) {
        CHECK(sequences[index] == Writes - Kept + index);
    }

    DecodedRecord sync;
    REQUIRE(FindSync(decoder, sync));
    CHECK(ReadFixed<uint64>(sync, TraceFormat::SnapshotSyncDroppedChunksOffset) == Writes - Kept);
    CHECK(ReadFixed<uint64>(sync, TraceFormat::SnapshotSyncDroppedBytesOffset) ==
          (Writes - Kept) * ChunkBytes);
    CHECK(ReadFixedByte(sync, TraceFormat::SnapshotSyncCompleteOffset) == 1);
}

// 한 펌프가 낸 덩어리 둘 가운데 앞 덩어리(기록 경계로 시작)를 잃으면, 뒷 덩어리는 기록 중간에서
// 시작하므로 스냅샷에서 뺀다. 그 뒤 펌프의 덩어리는 다시 기록 경계로 시작해 그대로 해석된다.
TEST(TraceRecent_SnapshotSkipsChunksThatLostTheirRecordStart) {
    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    REQUIRE(session.RegisterChannel(RecentChannel, true));
    REQUIRE(session.RegisterEventType(RecentType));

    // 기록 5개(20050 B)를 한 펌프로: 16 KiB 덩어리(경계) + 나머지 덩어리(기록 중간에서 시작).
    constexpr uint32 Batch = 5;
    for (uint32 sequence = 0; sequence < Batch; ++sequence) {
        WriteRecent(sequence);
    }

    REQUIRE(session.Pump());
    CHECK(session.Stats().recentChunks == 2);
    // 펌프마다 기록 하나를 더한다. 네 번째에서 첫 덩어리가 밀려난다.
    constexpr uint32 Singles = 4;
    for (uint32 index = 0; index < Singles; ++index) {
        WriteRecent(Batch + index);
        REQUIRE(session.Pump());
    }

    CHECK(session.Stats().recentDroppedChunks == 1);
    MemoryOutput output(allocator);
    auto snapshot = session.WriteSnapshot(&MemoryOutput::Write, &output);
    REQUIRE(snapshot);
    CHECK(snapshot.Value().skippedChunks == 1);
    CHECK(snapshot.Value().recentChunks == Singles);
    session.Stop();

    TraceStreamDecoder decoder(allocator);
    REQUIRE(decoder.Decode(output.bytes.View()));
    Array<uint32> sequences(allocator);
    REQUIRE(CollectSequences(decoder, sequences));
    REQUIRE(sequences.Size() == Singles);
    for (uint32 index = 0; index < Singles; ++index) {
        CHECK(sequences[index] == Batch + index);
    }

    DecodedRecord sync;
    REQUIRE(FindSync(decoder, sync));
    CHECK(ReadFixed<uint64>(sync, TraceFormat::SnapshotSyncSkippedChunksOffset) == 1);
}

// 출력을 붙이지 않은 세션의 스냅샷 하나만으로 분석 모델을 만든다: 스레드·이름 정의·CPU 구간이 모두
// 들어 있다.
TEST(TraceSnapshot_IsAnalyzableAlone) {
    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    REQUIRE(session.SetChannelEnabled(TraceCpuChannel, true));
    constexpr uint32 Scopes = 3;
    for (uint32 index = 0; index < Scopes; ++index) {
        CpuScope scope(SnapshotScope);
    }

    MemoryOutput output(allocator);
    auto snapshot = session.WriteSnapshot(&MemoryOutput::Write, &output);
    REQUIRE(snapshot);
    CHECK(snapshot.Value().complete);
    session.Stop();

    TraceModel model(allocator);
    REQUIRE(model.Build(output.bytes.View()));
    CHECK(model.Summary().snapshot);
    CHECK(!model.Summary().snapshotIncomplete);
    CHECK(!model.Summary().truncated);
    CHECK(model.Summary().missingScopeNames == 0);
    usize found = 0;
    for (const TraceModelThread& thread : model.Threads()) {
        for (const TraceModelScope& scope : thread.scopes) {
            if (model.ScopeName(scope.nameId) == StringView("Test.SnapshotScope") && scope.ended) {
                ++found;
            }
        }
    }

    CHECK(found == Scopes);
}

namespace {

// 펌프 안의 출력 쓰기에서 멈추는 콜백이다(writer 잠금을 쥔 채). armed 뒤 첫 쓰기에서 entered를
// 알리고 release를 기다린다.
struct BlockingOutput {
    static Result<void> Write(void* context, Span<const uint8> data) noexcept {
        auto& self = *static_cast<BlockingOutput*>(context);
        (void)data;
        if (platform::AtomicExchange(&self.armed, 0u) == 1u) {
            self.entered.Signal();
            WaitGate(self.release);
        }

        return Result<void>::Success();
    }

    uint32 armed = 0;
    platform::ManualResetEvent entered;
    platform::ManualResetEvent release;
};

struct PumpTask {
    TraceSession* session = nullptr;
    bool pumped = false;
};

void RunPump(void* context) noexcept {
    auto& task = *static_cast<PumpTask*>(context);
    task.pumped = task.session->Pump().IsOk();
}

} // namespace

// writer 잠금을 다른 스레드가 쥐고 있으면 스냅샷은 제한 시간 뒤 "불완전"으로 쓴다: 머리 플래그,
// 스키마, 동기 표지(완전 0)만 있고 고리 내용은 없다. 실패가 아니다.
TEST(TraceSnapshot_LockTimeoutMarksIncomplete) {
    SystemAllocator allocator;
    TraceSession session;
    REQUIRE(session.Start(ManualSettings(allocator)));
    REQUIRE(session.RegisterChannel(RecentChannel, true));
    REQUIRE(session.RegisterEventType(RecentType));
    BlockingOutput blocking;
    REQUIRE(session.AttachCallbackOutput(&BlockingOutput::Write, &blocking));
    WriteRecent(1);

    // 다른 스레드의 펌프가 출력 쓰기(writer 잠금 안)에서 멈춘다.
    platform::AtomicStore(&blocking.armed, 1u);
    PumpTask task{&session};
    platform::Thread pumper;
    REQUIRE(pumper.Start(&RunPump, &task) == platform::ThreadStatus::Success);
    WaitGate(blocking.entered);

    MemoryOutput output(allocator);
    auto snapshot = session.WriteSnapshot(&MemoryOutput::Write, &output, 0);
    blocking.release.Signal();
    JoinThread(pumper);
    CHECK(task.pumped);
    REQUIRE(snapshot);
    CHECK(!snapshot.Value().complete);
    CHECK(snapshot.Value().recentChunks == 0);

    // 잠금이 풀린 뒤의 스냅샷은 완전하다.
    MemoryOutput later(allocator);
    auto complete = session.WriteSnapshot(&MemoryOutput::Write, &later, 0);
    REQUIRE(complete);
    CHECK(complete.Value().complete);
    session.Stop();

    const uint32 flags = HeaderFlags(output.bytes);
    CHECK((flags & TraceFormat::HeaderFlagSnapshot) != 0);
    CHECK((flags & TraceFormat::HeaderFlagSnapshotIncomplete) != 0);
    TraceStreamDecoder decoder(allocator);
    REQUIRE(decoder.Decode(output.bytes.View()));
    DecodedRecord sync;
    REQUIRE(FindSync(decoder, sync));
    CHECK(ReadFixedByte(sync, TraceFormat::SnapshotSyncCompleteOffset) == 0);
    Array<uint32> sequences(allocator);
    REQUIRE(CollectSequences(decoder, sequences));
    CHECK(sequences.IsEmpty());

    TraceModel model(allocator);
    REQUIRE(model.Build(output.bytes.View()));
    CHECK(model.Summary().snapshot);
    CHECK(model.Summary().snapshotIncomplete);
}

// 풀 사용률이 경고 수준을 넘으면 펌프가 경고를 한 번 남기고, 아래로 내려간 뒤 다시 넘으면 또
// 남긴다.
TEST(TracePool_WarningIsLoggedOncePerCrossing) {
    SystemAllocator allocator;
    TraceSession session;
    TraceSessionSettings settings = ManualSettings(allocator);
    settings.poolMaxBytes = 4 * TraceBlockLayout::PageBytes;
    settings.poolWarningPercent = 50;
    REQUIRE(session.Start(settings));
    REQUIRE(session.RegisterChannel(RecentChannel, true));
    REQUIRE(session.RegisterEventType(RecentType));
    const uint64 limit = session.Stats().poolBytesLimit;
    auto fillAboveHalf = [&]() noexcept {
        uint32 sequence = 0;
        while (session.Stats().poolBytesInUse * 2 < limit) {
            WriteRecent(sequence++);
        }
    };

    fillAboveHalf();
    REQUIRE(session.Pump());
    CHECK(session.Stats().poolWarnings == 1);
    // 펌프가 블록을 돌려줘 아래로 내려갔다. 다음 펌프가 상태를 되돌린다(경고 없음).
    CHECK(session.Stats().poolBytesInUse * 2 < limit);
    REQUIRE(session.Pump());
    CHECK(session.Stats().poolWarnings == 1);
    fillAboveHalf();
    REQUIRE(session.Pump());
    CHECK(session.Stats().poolWarnings == 2);
    session.Stop();
}

// 채널 "stats"가 켜져 있으면 statsIntervalPumps 펌프마다 수집 통계 기록이 나간다.
TEST(TraceStats_RecordsAreWrittenEveryInterval) {
    SystemAllocator allocator;
    TraceSession session;
    TraceSessionSettings settings = ManualSettings(allocator);
    settings.statsIntervalPumps = 2;
    REQUIRE(session.Start(settings));
    REQUIRE(session.SetChannelEnabled(TraceStatsChannel, true));
    MemoryOutput output(allocator);
    REQUIRE(session.AttachCallbackOutput(&MemoryOutput::Write, &output));
    constexpr uint32 Pumps = 4;
    for (uint32 index = 0; index < Pumps; ++index) {
        REQUIRE(session.Pump());
    }

    const TraceSessionStats stats = session.Stats();
    CHECK(stats.statsRecords == Pumps / 2);
    session.Stop();

    TraceStreamDecoder decoder(allocator);
    REQUIRE(decoder.Decode(output.bytes.View()));
    const uint16 type = TypeOf(decoder, Format::StatsType);
    REQUIRE(type != TraceFormat::InvalidTypeId);
    usize records = 0;
    for (const DecodedStream& stream : decoder.Streams()) {
        if (stream.id < TraceFormat::FirstThreadStreamId) {
            continue;
        }

        Array<DecodedRecord> decoded(allocator);
        REQUIRE(decoder.DecodeRecords(stream.id, decoded));
        for (const DecodedRecord& record : decoded) {
            if (record.typeId == type) {
                ++records;
                CHECK(ReadFixed<uint64>(record, Format::StatsPoolLimitOffset) ==
                      stats.poolBytesLimit);
                CHECK(ReadFixed<uint64>(record, Format::StatsTickOffset) != 0);
            }
        }
    }

    CHECK(records == Pumps / 2);
}
} // namespace trace_recent_tests_file
