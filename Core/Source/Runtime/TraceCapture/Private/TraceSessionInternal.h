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
#include <TraceCapture/TraceContext.h>
#include <TraceCapture/TraceCounter.h>
#include <TraceCapture/TraceCpuScope.h>
#include <TraceCapture/TraceMark.h>
#include <TraceCapture/TraceSession.h>
#include <TraceCapture/TraceTask.h>
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

    // id를 acquire로 읽는다. 등록 때 id보다 먼저 쓴 가변 필드 배치가 함께 보인다.
    static uint16 AcquireId(const TraceEventType& type) noexcept {
        return platform::AtomicLoad<MemoryOrder::Acquire>(&type.id_);
    }

    // 가변 필드 배치를 채운다. id를 공개하기 전(NoId인 동안)에만 부른다.
    static void SetLayout(TraceEventType& type) noexcept {
        uint8 count = 0;
        for (usize index = 0; index < type.FieldCount(); ++index) {
            const TraceField& field = type.Fields()[index];
            if (IsVariableTraceField(field.type)) {
                type.variableElementBytes_[count++] =
                    static_cast<uint8>(TraceFieldElementBytes(field.type));
            }
        }

        type.variableCount_ = count;
    }

    static uint8 VariableElementBytes(const TraceEventType& type, usize index) noexcept {
        return type.variableElementBytes_[index];
    }
};

// 정적 이름·카운터·표지 지점이 세션별 id를 캐시하는 칸의 형식이다: (serial 하위 32비트 << 32) | id.
constexpr uint64 PackSessionId(uint64 serial, uint32 id) noexcept {
    return (serial << 32) | id;
}

// cache가 serial 세션의 값이면 id를, 아니면 0을 돌려준다.
inline uint32 CachedSessionId(const uint64* cache, uint64 serial) noexcept {
    const uint64 value = platform::AtomicLoad<MemoryOrder::Relaxed>(cache);
    return static_cast<uint32>(value >> 32) == static_cast<uint32>(serial)
               ? static_cast<uint32>(value)
               : 0;
}

struct TraceScopeNameAccess {
    static uint64* Cache(TraceScopeName& name) noexcept {
        return &name.cache_;
    }
};

struct TraceCounterAccess {
    static uint64* Cache(TraceCounter& counter) noexcept {
        return &counter.cache_;
    }

    static uint64* Value(TraceCounter& counter) noexcept {
        return &counter.value_;
    }

    static uint64* LastBits(TraceCounter& counter) noexcept {
        return &counter.lastBits_;
    }

    static uint64* LastSerial(TraceCounter& counter) noexcept {
        return &counter.lastSerial_;
    }
};

struct TraceMarkSiteAccess {
    static uint64* Cache(TraceMarkSite& site) noexcept {
        return &site.cache_;
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

// 스레드의 동적 이름 앞단 캐시 한 칸이다(결정 T4). text는 세션 이름 표의 저장소를 가리킨다.
struct TraceNameCacheEntry {
    uint64 hash = 0;
    const char* text = nullptr;
    uint32 length = 0;
    uint32 id = 0;
};

// 한 스레드가 한 세션에서 쓰는 상태다. 첫 기록 때 세션 할당기로 만들거나 끝난 스레드의 칸을 다시
// 쓰고, 세션 Stop 때 해제한다. 칸을 다시 쓸 때마다 generation이 늘고 trace 스레드 id는 새로 받는다.
struct TraceThreadState {
    static constexpr usize NameCacheEntries = 64;

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
    TraceNameCacheEntry nameCache[NameCacheEntries];

    // ---- 공유 ----
    // 공개한 byte 누계. 생산자가 기록을 다 쓴 뒤 release로 늘린다. writer는 acquire로 읽어 그
    // 앞까지만 읽는다(기록 경계에서만 늘어난다).
    uint64 published = 0;
    // 첫 블록. 생산자가 release로 한 번 쓴다.
    TraceBlock* firstBlock = nullptr;
    // 스레드가 끝났다(TLS 정리). 마지막 공개 뒤 release로 1을 쓴다.
    uint32 exited = 0;
    // 통계용 손실 누계(relaxed). 칸을 다시 써도 이어 센다.
    uint64 lostTotal = 0;
    // 칸의 세대다. 생산자가 칸을 다시 쓸 때 다른 칸을 모두 초기화한 뒤 release로 늘린다. writer는
    // 세대가 바뀌면 자기 칸을 처음으로 되돌린다.
    uint32 generation = 0;

    // ---- 세대마다 등록 때 정함 ----
    uint32 threadId = 0;
    uint64 osThreadId = 0;

    // ---- 목록(registryMutex) ----
    // 세션 스레드 목록의 다음 칸이다. 목록 머리에 넣기 전에 정하고 바꾸지 않는다(칸을 다시 써도
    // 목록 자리는 그대로다).
    TraceThreadState* next = nullptr;
    // 재사용 목록의 다음 칸이다.
    TraceThreadState* freeNext = nullptr;

    // ---- writer 전용 ----
    TraceBlock* readBlock = nullptr;
    uint64 consumed = 0;
    uint64 target = 0;
    // target을 읽을 때의 세대와 writer가 따르는 세대다.
    uint32 targetGeneration = 0;
    uint32 writerGeneration = 0;
    bool done = false;
};

// ---- 스레드 TLS 칸 ----

// 문맥 스택 한 칸이다. cache는 정의를 쓴 세션의 (serial, 문맥 id)이고, recorded는 쌓기를 기록한
// 세션 serial이다(0이면 기록하지 않았다).
struct TraceContextEntry {
    TraceContextValues values;
    uint64 cache = 0;
    uint64 recorded = 0;
};

// 스레드의 TLS 칸이다. 세션 메모리가 아니라 스레드 수명에 묶여 있어, Stop이 아직 세션에 등록하지
// 않은 스레드의 진행 중 기록도 기다릴 수 있다.
//
// 기록 호출은 busy를 올린 뒤(seq_cst) 현재 세션을 읽고, 끝나면 내린다. Stop은 현재 세션을 null로 쓴
// 뒤(seq_cst) 모든 칸의 busy가 0이 되기를 기다린다. 기록 호출이 세션을 읽었다면 그 전에 올린 busy를
// Stop이 반드시 보므로, Stop이 세션 메모리를 해제할 때 그 세션을 쓰는 기록 호출은 없다.
//
// CPU 구간 배치·스레드 정보·문맥 스택은 이 스레드만 쓰는 칸이라 세션 접근 없이 바꾼다.
struct TraceThreadSlot {
    // 진행 중인 기록 호출의 깊이다. 이 스레드만 쓰고 Stop이 읽는다.
    uint32 busy = 0;
    bool linked = false;
    // state가 속한 세션의 serial이다. 다르면 state는 이미 해제된 세션의 것이므로 보지 않는다.
    uint64 serial = 0;
    TraceThreadState* state = nullptr;
    TraceThreadSlot* previous = nullptr;
    TraceThreadSlot* next = nullptr;

    // ---- CPU 구간 배치 ----
    // 배치가 속한 세션 serial, 배치 안 마지막 tick, 열린 구간 깊이, 배치 byte 수다.
    uint64 scopeSerial = 0;
    uint64 scopeLastTick = 0;
    uint32 scopeDepth = 0;
    uint32 scopeBytes = 0;
    uint8 scopeBatch[TraceStandardFormat::ScopeBatchBytes] = {};

    // ---- 스레드 정보 ----
    char threadName[TraceStandardFormat::MaxThreadNameBytes] = {};
    char threadGroup[TraceStandardFormat::MaxThreadNameBytes] = {};
    uint8 threadNameBytes = 0;
    uint8 threadGroupBytes = 0;
    int32 threadSortOrder = 0;

    // ---- 문맥 스택 ----
    TraceContextEntry contexts[TraceStandardFormat::MaxContextDepth];
    uint32 contextDepth = 0;
    // 깊이 상한을 넘어 세기만 한 쌓기 수다.
    uint32 contextOverflow = 0;
    // 이 스레드에서 실행 중인 task의 trace id다(task 밖이면 0, TraceTaskScope가 바꾼다).
    uint64 currentTask = 0;

    constexpr TraceThreadSlot() noexcept = default;
    TraceThreadSlot(const TraceThreadSlot&) = delete;
    TraceThreadSlot& operator=(const TraceThreadSlot&) = delete;

    // 스레드 종료(TLS 정리) 때 남은 구간 배치를 내고 현재 세션의 스레드 상태를 은퇴로 표시한다.
    ~TraceThreadSlot() noexcept;

    uint32 Enter() noexcept {
        const uint32 depth = platform::AtomicLoad<MemoryOrder::Relaxed>(&busy);
        platform::AtomicStore<MemoryOrder::SequentiallyConsistent>(&busy, depth + 1);
        return depth;
    }

    void Leave(uint32 depth) noexcept {
        platform::AtomicStore<MemoryOrder::Release>(&busy, depth);
    }

    void Link() noexcept;
    void Unlink() noexcept;
};

// 이 스레드의 TLS 칸이다(처음 부를 때 Stop이 기다릴 수 있도록 전역 목록에 넣는다).
TraceThreadSlot& CurrentTraceThreadSlot() noexcept;

// 현재 세션을 진행 중 표시 아래에서 빌린다. 객체가 살아 있는 동안 Session()은 해제되지 않는다.
class TraceSessionGuard {
public:
    // 이 스레드의 TLS 칸으로 빌린다.
    TraceSessionGuard() noexcept;
    // 주어진 칸(이 스레드의 칸이어야 한다)으로 빌린다. TLS 칸의 소멸자가 자기 자신을 넘긴다.
    explicit TraceSessionGuard(TraceThreadSlot& slot) noexcept;
    ~TraceSessionGuard() noexcept;

    TraceSessionGuard(const TraceSessionGuard&) = delete;
    TraceSessionGuard& operator=(const TraceSessionGuard&) = delete;

    // 현재 세션이다. 없거나, serial이 주어졌는데 다르면 null이다.
    TraceSessionState* Session(uint64 serial = 0) const noexcept;

    TraceThreadSlot& Slot() const noexcept {
        return slot_;
    }

private:
    TraceThreadSlot& slot_;
    TraceSessionState* session_;
    uint32 depth_;
};

// ---- 이름 표 ----

// 세션 전역 이름 표다(결정 T4). 읽기는 잠금 없이 열린 주소법으로 찾고, 삽입만 잠금 하나로
// 직렬화한다. 칸은 해시를 마지막에 release로 써서 공개하고, 이름 byte는 세션 저장소에 복사해 세션
// 끝까지 옮기지 않는다. 칸의 3/4이 차면 더 넣지 않는다.
class TraceNameTable {
public:
    Result<void> Initialize(Allocator& allocator, usize capacity) noexcept;
    void Release() noexcept;

    // 이름의 id를 찾거나 새로 넣는다. 새로 넣으면 이름 정의(Trace.ScopeName)를 먼저 정의 버퍼에
    // 쓰고 칸을 공개한다. 표가 가득 차거나 정의를 쓰지 못하면 0이다. stored에는 표가 가진 이름
    // byte를 돌려준다(스레드 캐시가 비교에 쓴다). truncated는 name이 원래 이름의 앞부분이라는
    // 표시다(이름 정의에 남는다).
    uint32 FindOrInsert(TraceSessionState& session, StringView name, uint64 hash, bool truncated,
                        const char*& stored, uint32& storedLength) noexcept;

    usize Count() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&count_);
    }

    uint64 Rejected() const noexcept {
        return platform::AtomicLoad<MemoryOrder::Relaxed>(&rejected_);
    }

    // 이름 byte의 해시다(0은 빈 칸 표지라 쓰지 않는다).
    static uint64 HashName(StringView name) noexcept;

private:
    struct Slot {
        uint64 hash;
        const char* text;
        uint32 length;
        uint32 id;
    };

    uint32 Find(StringView name, uint64 hash, const char*& stored,
                uint32& storedLength) const noexcept;
    char* CopyText(StringView name) noexcept;

    Allocator* allocator_ = nullptr;
    Slot* slots_ = nullptr;
    usize mask_ = 0;
    usize count_ = 0;
    uint64 rejected_ = 0;
    // 이름 byte 저장소(덩어리 목록, 덩어리 첫 8 byte가 다음 덩어리)다.
    uint8* chunk_ = nullptr;
    usize chunkUsed_ = 0;
    usize chunkBytes_ = 0;
    platform::Mutex insertMutex_;
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

// 고리에서 스냅샷으로 옮긴 결과다.
struct TraceRecentCopy {
    uint64 chunks = 0;
    uint64 bytes = 0;
    uint64 skippedChunks = 0;
};

// 최근 기록 고리다(TraceRecent.cpp, 8절). 스레드 스트림 덩어리를 고정 크기 byte 고리에 담고 넘치면
// 가장 오래된 덩어리부터 버린다. writer(writerMutex)만 쓴다.
class TraceRecentRing {
public:
    explicit TraceRecentRing(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    // capacity가 0이면 고리가 없다(Push는 무동작).
    Result<void> Initialize(Allocator& allocator, usize capacity) noexcept;
    void Release() noexcept;

    bool IsActive() const noexcept {
        return capacity_ != 0;
    }

    // 덩어리 하나(머리 포함)를 담는다. recordBoundary는 그 스레드 스트림이 이 덩어리에서 기록
    // 경계로 시작하는지다(펌프마다 스레드의 첫 덩어리).
    void Push(Span<const uint8> chunk, bool recordBoundary) noexcept;

    // 담긴 덩어리를 오래된 것부터 out 뒤에 붙인다. 스트림마다 기록 경계 덩어리 전의 덩어리는 뺀다.
    Result<TraceRecentCopy> AppendTo(Array<uint8>& out) const noexcept;

    usize Capacity() const noexcept {
        return capacity_;
    }

    usize UsedBytes() const noexcept {
        return used_;
    }

    uint64 Chunks() const noexcept {
        return chunks_;
    }

    uint64 DroppedChunks() const noexcept {
        return droppedChunks_;
    }

    uint64 DroppedBytes() const noexcept {
        return droppedBytes_;
    }

private:
    void CopyIn(usize position, const uint8* source, usize bytes) noexcept;
    void CopyOut(usize position, uint8* destination, usize bytes) const noexcept;
    // position 칸의 byte 수(표시 포함)와 표시·스트림 id다.
    usize EntryBytesAt(usize position, uint8& flags, uint16& stream) const noexcept;

    Allocator* allocator_;
    uint8* data_ = nullptr;
    usize capacity_ = 0;
    usize start_ = 0;
    usize used_ = 0;
    uint64 chunks_ = 0;
    uint64 droppedChunks_ = 0;
    uint64 droppedBytes_ = 0;
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
    TraceNameTable names;
    // 세션 안 카운터·표지 호출 지점 id(registryMutex)와 문맥·시간 띠 id(원자 fetch-add) 발급기다.
    uint32 nextCounterId = 0;
    uint32 nextMarkSiteId = 0;
    uint32 nextContextId = 0;
    uint64 nextSpanId = 0;
    uint64 nextTaskId = 0;
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
    // 끝나서 writer가 다 비운 칸의 재사용 목록과 지금까지 만든 칸 수다.
    TraceThreadState* freeThreads = nullptr;
    uint32 threadSlots = 0;

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
    platform::FileStatus lastOutputFileStatus;
    ErrorCode lastOutputCallbackError = ErrorCode::None;
    // writer 버퍼를 늘리지 못해 버린 덩어리 수다.
    uint64 writerDroppedChunks = 0;
    // 최근 기록 고리와, 지금 비우는 스레드 스트림의 다음 덩어리가 기록 경계로 시작하는지다.
    TraceRecentRing recent;
    bool recentBoundary = false;
    // 풀 사용률 경고 상태(경고한 뒤 아래로 내려가기 전)와 경고 수, 펌프 수, 낸 통계 기록 수다.
    bool poolWarned = false;
    uint64 poolWarnings = 0;
    uint64 pumpCount = 0;
    uint64 statsRecords = 0;

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
    void RecycleThread(TraceThreadState& thread) noexcept;
    void AppendStream(Array<uint8>& out, uint16 stream, Span<const uint8> bytes) noexcept;
    void FinishStream(Array<uint8>& out, uint16 stream) noexcept;
    // 덩어리 하나를 out에 붙인다(필요하면 압축). 출력 버퍼에 붙이는 스레드 스트림 덩어리는 최근
    // 기록 고리에도 담는다.
    void AppendChunk(Array<uint8>& out, uint16 stream, Span<const uint8> payload) noexcept;
    void AppendChunkBytes(Array<uint8>& out, uint16 stream, Span<const uint8> payload) noexcept;
    void FlushOutputBuffer() noexcept;
    bool WriteOutput(TraceOutput& output, Span<const uint8> bytes) noexcept;
    void CloseOutput(TraceOutput& output, bool& closeFailed) noexcept;
    // 파일 머리를 채운다(attachOrder는 머리의 순서 번호 칸, flags는 머리 플래그).
    void BuildFileHeader(uint8 (&header)[TraceFormat::FileHeaderBytes], uint64 attachOrder,
                         uint32 flags) const noexcept;
    // 펌프 시작 때 풀 사용률을 보고 경고한다(poolWarningPercent).
    void CheckPoolUsage() noexcept;
    // statsIntervalPumps마다(채널 "stats"가 켜져 있으면) 수집 통계 기록을 낸다.
    void WriteStatsRecord() noexcept;

    // ---- 동작(TraceThreadWrite.cpp) ----
    // 손실 상태로 남은 스레드의 손실 기록을 쓴다. 생산자가 모두 멈춘 뒤(Stop)에만 부른다.
    void FlushPendingLoss() noexcept;
    // 정의 기록 하나를 정의 버퍼에 쓴다. 버퍼가 차 있으면 false다.
    bool WriteDefinition(const TraceRecordShape& shape) noexcept;
};

// 현재 세션(TraceWrite가 쓰는 곳), 그 serial(없으면 0), 세션 serial 발급기다. 상수 초기화되는
// 전역이다. currentSessionSerial은 세션 메모리를 보지 않고 "어느 세션이 현재인가"만 알 때 쓴다(CPU
// 구간·정적 이름 캐시).
extern TraceSessionState* currentSession;
extern uint64 currentSessionSerial;
extern uint64 sessionSerialCounter;

// 이 스레드를 세션에 등록하거나 이미 등록한 상태를 돌려준다. 실패(스레드 id 상한·할당 실패)는 null.
// TraceSessionGuard 아래에서 부른다.
TraceThreadState* AcquireThreadState(TraceSessionState& session, TraceThreadSlot& slot) noexcept;

// 기록 하나를 세션에 쓴다(채널 검사 없음, 형은 등록돼 있어야 한다). TraceSessionGuard 아래에서
// 부른다.
TraceWriteResult WriteSessionRecord(TraceSessionState& session, TraceThreadSlot& slot,
                                    const TraceEventType& type, uint16 typeId, const void* fixed,
                                    usize fixedBytes,
                                    Span<const TraceVariablePart> variable) noexcept;

// 등록된 정의 형(Definition)의 기록을 채널 검사 없이 정의 버퍼에 쓴다. 버퍼가 차 있으면 버린 정의로
// 세고 false다.
bool WriteStandardDefinition(TraceSessionState& session, const TraceEventType& type,
                             const void* fixed, usize fixedBytes,
                             Span<const TraceVariablePart> variable) noexcept;

// 이 스레드의 스레드 정보(Trace.ThreadInfo)를 정의 기록으로 쓴다.
void WriteThreadInfo(TraceSessionState& session, const TraceThreadState& state,
                     const TraceThreadSlot& slot) noexcept;

// 이름(CPU 구간·시간 띠)의 세션 id를 찾거나 넣는다. MaxScopeNameBytes를 넘는 이름은 앞부분만 쓴다.
// useThreadCache면 이 스레드의 앞단 캐시를 먼저 본다(동적 이름). serial 세션이 현재가 아니거나 표가
// 가득 차면 0이다.
uint32 ResolveTraceName(uint64 serial, StringView name, bool useThreadCache) noexcept;

// 이 스레드의 CPU 구간 배치를 이벤트 하나로 낸다(배치 세션과 현재 세션이 같을 때만). 배치를 비운다.
void FlushCpuScopeBatch(TraceThreadSlot& slot) noexcept;

// 세션 Start가 등록하는 표준 채널·형이다(TraceStandardTypes.cpp).
extern TraceEventType TraceScopeNameType;
extern TraceEventType TraceCpuScopeBatchType;
extern TraceEventType TraceCounterDefinitionType;
extern TraceEventType TraceCounterIntegerType;
extern TraceEventType TraceCounterFloatType;
extern TraceEventType TraceMarkSiteType;
extern TraceEventType TraceMarkEventType;
extern TraceEventType TraceSpanBeginType;
extern TraceEventType TraceSpanEndType;
extern TraceEventType TraceFrameBeginType;
extern TraceEventType TraceFrameEndType;
extern TraceEventType TraceContextDefinitionType;
extern TraceEventType TraceContextPushType;
extern TraceEventType TraceContextPopType;
extern TraceEventType TraceTaskEventType;
extern TraceEventType TraceStatsType;

// 표준 채널(꺼진 상태로)과 표준 형을 등록한다.
Result<void> RegisterStandardTraceTypes(TraceSession& session) noexcept;

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
