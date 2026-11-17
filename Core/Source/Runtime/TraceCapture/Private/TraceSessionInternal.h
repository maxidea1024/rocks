#pragma once

#include <Base/Endian.h>
#include <Compression/Compression.h>
#include <Core/Build/BuildInfo.h>
#include <Core/Containers/Array.h>
#include <Platform/Atomics.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TraceCapture/TraceSession.h>
#include <TraceCapture/TraceWrite.h>

// TraceCapture 내부 구조다. 동시성 규약(누가 어떤 칸을 쓰고 어떤 순서로 공개하는지)을 칸 옆에
// 적는다.
namespace rocks::detail {

using platform::MemoryOrder;

struct TraceChannelAccess {
    static void SetId(TraceChannel& channel, uint16 id) noexcept {
        platform::AtomicStore<MemoryOrder::Relaxed>(&channel.id_, id);
    }

    static void SetEnabled(TraceChannel& channel, bool enabled) noexcept {
        platform::AtomicStore<MemoryOrder::Release>(&channel.enabled_, enabled ? 1u : 0u);
    }
};

struct TraceEventTypeAccess {
    static void SetId(TraceEventType& type, uint16 id) noexcept {
        platform::AtomicStore<MemoryOrder::Release>(&type.id_, id);
    }
};

// ---- 스레드 블록 ----

struct TraceBlock;

// 블록 끝의 머리 정보다(UE FWriteBuffer와 같이 끝에 둔다).
struct TraceBlockHeader {
    // 이 블록 첫 byte가 스레드 스트림에서 차지하는 위치(스레드가 공개한 byte 누계 기준)다. 블록을
    // 스레드에 연결하기 전에 생산자가 쓰고, 연결(next·firstBlock의 release 저장)로 공개한다.
    uint64 startTotal;
    // 같은 스레드의 다음 블록이다. 생산자가 release로 쓰고 writer가 acquire로 읽는다. 다음 블록이
    // 생기면 이 블록에는 더 쓰지 않는다(은퇴).
    TraceBlock* next;
    // 자유 목록에서 다음 블록의 색인 + 1(0은 끝)이다. 풀만 원자적으로 읽고 쓴다.
    uint32 freeNext;
    // 풀 안의 색인이다. 확보 뒤 바뀌지 않는다.
    uint32 index;
};

struct TraceBlock {
    uint8 payload[TraceBlockLayout::PayloadBytes];
    TraceBlockHeader header;
};

static_assert(sizeof(TraceBlockHeader) == TraceBlockLayout::HeaderBytes,
              "TraceBlockLayout::HeaderBytes must match the block header");
static_assert(sizeof(TraceBlock) == TraceBlockLayout::BlockBytes,
              "trace block must be exactly TraceBlockLayout::BlockBytes");

// 상한이 있는 블록 재사용 저장소다(4.1절). 자유 목록은 잠금 없는 stack이고(ABA는 상위 32비트 tag로
// 막는다), 새 페이지 확보만 growMutex_로 직렬화한다. 상한(maxPages_)에 닿으면 기다리지 않고 null을
// 돌려준다.
class TraceBlockPool {
public:
    Result<void> Initialize(Allocator& allocator, usize maxBytes) noexcept;

    // 모든 페이지를 해제한다. 어떤 스레드도 블록을 쓰지 않을 때만 부른다.
    void Release() noexcept;

    // 어느 스레드에서나 부른다. 실패(상한·할당 실패)는 null이다. 블록 내용은 정하지 않는다.
    TraceBlock* TryAcquire() noexcept;

    // 다 쓴 블록을 돌려준다. 어느 스레드에서나 부른다.
    void Free(TraceBlock* block) noexcept;

    uint64 BlocksInUse() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&inUse_);
    }

    uint64 PeakBlocksInUse() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&peak_);
    }

    uint64 ReservedPages() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&pageCount_);
    }

    uint32 MaxPages() const noexcept {
        return maxPages_;
    }

private:
    TraceBlock* BlockAt(uint32 index) const noexcept;
    TraceBlock* TryPop() noexcept;
    void Push(TraceBlock* block) noexcept;
    void CountAcquired() noexcept;

    Allocator* allocator_ = nullptr;
    // 페이지 시작 주소 표다(maxPages_칸). growMutex_ 아래에서 채우고, 블록을 자유 목록에 넣는
    // release CAS로 공개한다.
    TraceBlock** pages_ = nullptr;
    uint32 maxPages_ = 0;
    uint32 pageCount_ = 0;
    // 상위 32비트 tag | 하위 32비트 (색인 + 1). 0은 빈 목록이다.
    uint64 freeHead_ = 0;
    uint64 inUse_ = 0;
    uint64 peak_ = 0;
    platform::Mutex growMutex_;
};

// ---- 정의 버퍼 ----

// 정의 기록을 여러 생산자가 bump로 쓰는 공유 버퍼다(4.3절). 기록마다 앞에 u32 머리(크기 | 준비
// 비트)를 두고, 생산자는 내용을 쓴 뒤 머리를 release로 쓴다. writer는 앞에서부터 준비된 기록만
// 읽고, 끝까지 비웠고 그 사이 새 예약이 없으면 버퍼를 처음으로 되돌린다.
class TraceDefinitionBuffer {
public:
    static constexpr uint32 ReadyBit = 0x80000000u;
    static constexpr usize RecordHeaderBytes = 4;
    static constexpr usize RecordAlignment = 4;

    Result<void> Initialize(Allocator& allocator, usize capacity) noexcept;
    void Release() noexcept;

    // 기록 하나를 쓸 자리를 예약한다. 실패(버퍼 가득 참·크기 상한 초과)는 null이다. 버린 기록으로
    // 셀지는 호출자가 정한다(NoteDropped). 성공하면 recordBytes byte를 쓴 뒤 Commit을 부른다.
    uint8* TryReserve(usize recordBytes) noexcept;
    static void Commit(uint8* record, usize recordBytes) noexcept;

    // 정의 기록 하나를 버렸다고 센다. writer가 DefinitionLoss 기록으로 스트림에 알린다.
    void NoteDropped() noexcept {
        platform::AtomicFetchAdd<MemoryOrder::Relaxed>(&dropped_, 1);
    }

    // writer 전용. 준비된 기록을 순서대로 visit(context, record)로 넘긴다. 준비되지 않은 기록(쓰는
    // 중)에서 멈추면 false, 예약된 기록을 모두 넘겼으면 true다.
    template <typename Visit>
    bool Drain(Visit&& visit) noexcept;

    uint64 Dropped() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&dropped_);
    }

private:
    Allocator* allocator_ = nullptr;
    uint8* data_ = nullptr;
    uint32 capacity_ = 0;
    // 생산자가 CAS로 늘리는 예약 끝이다. writer가 다 비운 뒤 0으로 되돌린다.
    uint32 reserved_ = 0;
    // writer 전용 읽기 위치다.
    uint32 readPosition_ = 0;
    uint64 dropped_ = 0;
};

template <typename Visit>
bool TraceDefinitionBuffer::Drain(Visit&& visit) noexcept {
    const uint32 reserved = platform::AtomicLoad<MemoryOrder::Acquire>(&reserved_);
    uint32 position = readPosition_;
    while (position < reserved) {
        const uint32 header =
            platform::AtomicLoad<MemoryOrder::Acquire>(reinterpret_cast<uint32*>(data_ + position));
        if ((header & ReadyBit) == 0) {
            readPosition_ = position;
            return false;
        }

        const uint32 bytes = header & ~ReadyBit;
        visit(Span<const uint8>(data_ + position + RecordHeaderBytes, bytes));
        position += static_cast<uint32>(RecordHeaderBytes +
                                        ((bytes + RecordAlignment - 1) & ~(RecordAlignment - 1)));
    }

    readPosition_ = position;
    // 비운 부분을 0으로 돌린 뒤(준비 비트가 남지 않게) 새 예약이 없을 때만 처음으로 되돌린다.
    // 생산자는 되돌린 뒤에야 [0, position)에 쓰므로 지우는 동안 겹치지 않는다.
    if (position != 0) {
        platform::MemoryZero(data_, position);
        uint32 expected = position;
        if (platform::AtomicCompareExchangeStrong<MemoryOrder::AcquireRelease,
                                                  MemoryOrder::Relaxed>(&reserved_, expected, 0u)) {
            readPosition_ = 0;
        }
    }

    return true;
}

// ---- 스레드 상태 ----

// 한 스레드가 한 세션에서 쓰는 상태다. 첫 기록 때 세션 할당기로 만들고 세션 Stop 때 해제한다.
struct TraceThreadState {
    // ---- 생산자(그 스레드) 전용 ----
    TraceBlock* block = nullptr;
    usize offset = 0;
    // 지금까지 공개한 byte 누계(published와 같은 값, 생산자가 읽기 쉽도록 따로 둔다).
    uint64 total = 0;
    bool writing = false;
    bool lossActive = false;
    uint64 lossStartTick = 0;
    uint64 lostCounts[TraceFormat::MaxChannels] = {};
    // 손실이 있는 채널 bit(MaxChannels bit)다.
    uint64 lostChannelMask[TraceFormat::MaxChannels / 64] = {};

    // ---- 공유 ----
    // 공개한 byte 누계. 생산자가 기록을 다 쓴 뒤 release로 늘린다. writer는 acquire로 읽어 그
    // 앞까지만 읽는다(기록 경계에서만 늘어난다).
    uint64 published = 0;
    // 첫 블록. 생산자가 release로 한 번 쓴다.
    TraceBlock* firstBlock = nullptr;
    // 스레드가 끝났다(TLS 정리). 마지막 공개 뒤 release로 1을 쓴다.
    uint32 exited = 0;
    // 통계용 손실 누계(relaxed).
    uint64 lostTotal = 0;

    // ---- 불변(등록 때 정함) ----
    uint32 threadId = 0;
    uint64 osThreadId = 0;
    // 세션 스레드 목록의 다음 칸이다. 목록 머리에 넣기 전에 정하고 바꾸지 않는다.
    TraceThreadState* next = nullptr;

    // ---- writer 전용 ----
    TraceBlock* readBlock = nullptr;
    uint64 consumed = 0;
    uint64 target = 0;
    bool done = false;
};

// ---- 출력 ----

struct TraceOutput {
    uint32 id = 0;
    bool failed = false;
    // 파일 출력이면 세션 할당기로 만든 File이다(콜백 출력이면 null).
    platform::File* file = nullptr;
    TraceOutputWrite write = nullptr;
    void* context = nullptr;
};

// 레코드 하나의 모양이다. 쓰는 곳(스레드 블록·정의 버퍼·writer)이 같은 직렬화를 쓴다.
struct TraceRecordShape {
    uint16 typeId = 0;
    bool ordered = false;
    uint64 order = 0;
    const void* fixed = nullptr;
    usize fixedBytes = 0;
    Span<const TraceVariablePart> variable;
};

// 레코드 byte 수다. 크기 상한(TraceFormat::MaxRecordBytes)을 넘으면 MaxSize다.
usize TraceRecordBytes(const TraceRecordShape& shape) noexcept;

// 레코드를 sink.Write(source, bytes)로 순서대로 쓴다(TraceFormat.h의 기록 형식).
template <typename Sink>
void WriteTraceRecord(Sink& sink, const TraceRecordShape& shape) noexcept {
    uint8 head[sizeof(uint16) + sizeof(uint64)];
    StoreLittleEndian<uint16>(head, shape.typeId);
    usize headBytes = sizeof(uint16);
    if (shape.ordered) {
        StoreLittleEndian<uint64>(head + headBytes, shape.order);
        headBytes += sizeof(uint64);
    }

    sink.Write(head, headBytes);
    sink.Write(shape.fixed, shape.fixedBytes);
    for (const TraceVariablePart& part : shape.variable) {
        uint8 length[sizeof(uint32)];
        StoreLittleEndian<uint32>(length, static_cast<uint32>(part.bytes));
        sink.Write(length, sizeof(length));
    }

    for (const TraceVariablePart& part : shape.variable) {
        sink.Write(part.data, part.bytes);
    }
}

// 연속 메모리에 레코드를 쓴다. destination은 TraceRecordBytes 이상이어야 한다.
void EncodeTraceRecord(uint8* destination, const TraceRecordShape& shape) noexcept;

// ---- 세션 ----

struct TraceTypeEntry {
    // 등록한 객체다. 세션 내부 형과 등록을 푼 형은 null이다.
    TraceEventType* object = nullptr;
    // schemaBytes 안 스키마 기록의 위치다.
    usize schemaOffset = 0;
    usize schemaBytes = 0;
    bool present = false;
};

struct TraceSessionState {
    // 동시에 붙일 수 있는 출력 수다.
    static constexpr usize MaxOutputs = 4;
    // 출력 버퍼가 이 크기를 넘으면 펌프 도중에도 출력으로 쓴다.
    static constexpr usize OutputFlushBytes = 256 * 1024;

    explicit TraceSessionState(Allocator& allocator) noexcept;

    // ---- 불변(Start에서 정함) ----
    TraceSessionSettings settings;
    Allocator* allocator;
    uint64 serial = 0;
    uint64 baseTick = 0;
    uint64 tickFrequency = 0;
    int64 startUtcTicks = 0;
    Guid guid;
    Sha256Digest buildId;

    TraceBlockPool pool;
    TraceDefinitionBuffer definitions;
    // 순서 번호 발급기(relaxed fetch-add).
    uint64 nextOrder = 0;
    // 스레드 상태가 없어(등록 실패) 세지 못한 손실과 같은 스레드 재진입 손실이다.
    uint64 unattributedLost = 0;

    // ---- 등록 표(registryMutex) ----
    platform::Mutex registryMutex;
    TraceChannel* channels[TraceFormat::MaxChannels] = {};
    uint16 nextChannelId = 0;
    // 색인 = 형 id - 1.
    Array<TraceTypeEntry> types;
    Array<uint8> schemaBytes;
    uint32 nextThreadId = 1;
    // 스레드 목록 머리. registryMutex 아래에서 release로 바꾸고, writer·통계는 acquire로 읽는다.
    TraceThreadState* threads = nullptr;

    // ---- writer(writerMutex) ----
    platform::Mutex writerMutex;
    TraceOutput outputs[MaxOutputs];
    uint32 nextOutputId = 1;
    // 지금까지 출력으로 내보낸 스키마 수(types 색인 기준).
    usize schemasEmitted = 0;
    // 압축된 정의 덩어리 사본(정의 저장소)이다.
    Array<uint8> definitionStore;
    bool definitionStoreOverflowed = false;
    uint64 definitionRecordsNotStored = 0;
    uint64 definitionDropsReported = 0;
    // 덩어리 하나의 원본 내용을 모으는 버퍼, 출력 byte를 모으는 버퍼, 이번 펌프의 정의 덩어리,
    // 압축 결과 버퍼다.
    Array<uint8> staging;
    Array<uint8> outputBuffer;
    Array<uint8> definitionBatch;
    Array<uint8> compressed;
    Compressor compressor;
    uint64 outputBytes = 0;
    uint64 outputFailures = 0;
    // writer 버퍼를 늘리지 못해 버린 덩어리 수다.
    uint64 writerDroppedChunks = 0;

    // ---- writer 스레드 ----
    platform::Thread writerThread;
    platform::Mutex wakeMutex;
    platform::ConditionVariable wake;
    bool stopRequested = false;
    bool writerThreadStarted = false;

    // ---- 동작(TraceWriter.cpp, writerMutex를 잡은 채 부른다) ----
    Result<void> InitializeWriter() noexcept;
    void Pump() noexcept;
    // 마지막 비우기: 펌프 → 남은 손실 기록 → 펌프. 생산자가 모두 멈춘 뒤에만 부른다.
    void FinalFlush() noexcept;
    void CloseOutputs() noexcept;
    // output의 파일은 실패해도 이 함수가 닫고 해제한다.
    Result<TraceOutputId> AttachOutput(TraceOutput output) noexcept;
    Result<void> DetachOutput(TraceOutputId id) noexcept;
    bool HasActiveOutput() const noexcept;
    void EmitSchemas(bool hasOutputs) noexcept;
    bool DrainDefinitions(bool hasOutputs) noexcept;
    void DrainThread(TraceThreadState& thread, bool hasOutputs) noexcept;
    void AppendStream(Array<uint8>& out, uint16 stream, Span<const uint8> bytes) noexcept;
    void FinishStream(Array<uint8>& out, uint16 stream) noexcept;
    void AppendChunk(Array<uint8>& out, uint16 stream, Span<const uint8> payload) noexcept;
    void FlushOutputBuffer() noexcept;
    bool WriteOutput(TraceOutput& output, Span<const uint8> bytes) noexcept;
    void CloseOutput(TraceOutput& output, bool& closeFailed) noexcept;

    // ---- 동작(TraceThreadWrite.cpp) ----
    // 손실 상태로 남은 스레드의 손실 기록을 쓴다. 생산자가 모두 멈춘 뒤(Stop)에만 부른다.
    void FlushPendingLoss() noexcept;
    // 정의 기록 하나를 정의 버퍼에 쓴다. 버퍼가 차 있으면 false다.
    bool WriteDefinition(const TraceRecordShape& shape) noexcept;
};

// 현재 세션(TraceWrite가 쓰는 곳)과 세션 serial 발급기다. 상수 초기화되는 전역이다.
extern TraceSessionState* currentSession;
extern uint64 sessionSerialCounter;

// 현재 세션 공개를 거둔 뒤 부른다. 진행 중인 기록 호출(모든 스레드)이 끝날 때까지 기다린다.
void WaitForTraceProducers() noexcept;

// 손실 상태인 스레드의 손실 기록을 새 블록에 쓴다. 그 스레드가 쓰는 중이 아님이 보장될 때(스레드
// 종료 뒤·Stop 중)만 다른 스레드가 부른다. 블록을 받지 못하면 false이고 손실 상태가 남는다.
bool WriteThreadLoss(TraceBlockPool& pool, TraceThreadState& state) noexcept;

// writer 스레드 진입점이다(context = TraceSessionState). 주기마다 펌프하고, 정지 요청을 받으면
// 마지막 비우기와 출력 닫기를 한 뒤 끝난다.
void RunTraceWriter(void* context) noexcept;

// 이벤트 형 스키마 기록을 out 뒤에 붙인다. 이름·필드는 미리 검사한 값이다.
Result<void> AppendTraceSchema(Array<uint8>& out, uint16 typeId, const char* name, uint16 channelId,
                               uint8 flags, usize fixedBytes, const TraceField* fields,
                               usize fieldCount) noexcept;

} // namespace rocks::detail
