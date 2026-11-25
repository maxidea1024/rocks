#include <Base/PlacementNew.h>
#include <Platform/Atomics.h>
#include <Platform/Fatal.h>
#include <Platform/Sync/ParkingLot.h>
#include <Platform/Sync/SyncHooks.h>
#include <Platform/Sync/WaiterSignal.h>

#include "../VirtualMemoryBackend.h"
#include "QueueMutex.h"
#include "SyncSeams.h"

// ParkingLot 구현이다(Docs/Design/Synchronization 10.2절, UE ParkingLot.cpp 구조).
// - 스레드 레코드: 스레드마다 하나(WaiterSignal 하나). 처음 기다릴 때 thread_local 칸에 만든다.
// 깨우는
//   쪽이 쥔 동안 그 스레드가 끝나도 되도록 참조 계수로 수명을 잡고, 0이 되면 영구 풀의 free list로
//   돌아간다. 칸이 파괴된 뒤(스레드 종료 중 다른 thread_local 소멸자)의 대기는 대기마다 임시
//   레코드를 쓴다(D31).
// - 표: bucket 포인터 배열. 살아 있는 사용 스레드 수가 bucket 수를 넘으면 2배 이상의 2의
// 거듭제곱으로
//   키운다(모든 bucket을 색인 순서로 잠근 뒤 옮긴다). 옛 표는 해제하지 않는다.
// - 메모리: 가상 메모리 예약 하나에서 잘라 쓰는 영구 선형 할당(4 MiB + 64 KiB, D5).
// VirtualMemoryRegion
//   (멤버에 Mutex가 있다)을 거치지 않는다. 할당기 잠금도 QueueMutex라 재진입이 없다.
// - 깨울 스레드는 레코드의 next로 지역 목록에 엮는다(임시 배열·scratch 할당이 없다).
namespace rocks::platform::detail {

namespace {

constexpr usize KiB = 1024;
constexpr usize MiB = 1024 * KiB;
// 영구 할당 예약 크기다. UE와 같다: bucket·레코드 64 byte씩, 표 약 2N 포인터라 16,384 스레드에도
// 2.25 MiB이다(UE ParkingLot.cpp 59–61행). 넘침은 Fatal이다.
constexpr usize ArenaReserveBytes = 4 * MiB + 64 * KiB;
// 예약을 commit하는 단위다(페이지 크기의 배수).
constexpr usize ArenaCommitChunkBytes = 64 * KiB;
constexpr uint32 MinimumBucketCount = 32;
// 표를 키울 때 살아 있는 스레드 수에 곱하는 여유다(표 크기 ≥ 2 × 스레드 수).
constexpr uint32 BucketsPerThread = 2;
// 주소를 섞는 64-bit 곱셈 해시 상수다(황금비, Fibonacci hashing). 같은 cache line의 서로 다른
// 잠금이 다른 bucket으로 가게 상위 bit를 쓴다.
constexpr uint64 AddressHashMultiplier = 0x9E3779B97F4A7C15ull;
constexpr uint32 AddressBits = 64;

struct alignas(CacheLineSize) ThreadRecord {
    WaiterSignal signal;
    // 기다리는 주소다. bucket 잠금 아래에서만 읽고 쓴다(nullptr = 큐에 없음).
    const void* waitAddress = nullptr;
    // bucket 대기열 → 깨울 지역 목록 → free list로 쓰임이 바뀐다(한 번에 한 목록에만 있다).
    ThreadRecord* next = nullptr;
    // 깨운 쪽 WakeOne 콜백 값이다. bucket 잠금 아래에서 쓰고 Notify 뒤에 대기자가 읽는다.
    uint64 wakeToken = 0;
    // 소유 스레드(또는 임시 대기) 1 + 깨우는 중인 쪽 1.
    Atomic<uint32> references;
};

struct alignas(CacheLineSize) Bucket {
    QueueMutex mutex;
    ThreadRecord* head = nullptr;
    ThreadRecord* tail = nullptr;
};

struct Table {
    uint32 bucketCount = 0;
    uint32 shift = 0;
    Bucket** buckets = nullptr;
};

// 가상 메모리 예약 하나에서 잘라 쓰는 영구 선형 할당기다. 호출자가 mutex를 쥔다.
class PermanentArena {
public:
    QueueMutex& Mutex() noexcept {
        return mutex_;
    }

    void* AllocateLocked(usize bytes, usize alignment) noexcept {
        if (base_ == nullptr) {
            void* address = nullptr;
            if (!ReserveVirtualMemory(ArenaReserveBytes, VirtualMemoryAccess::Open, address)) {
                Fatal("ParkingLot could not reserve its permanent memory");
            }

            base_ = static_cast<unsigned char*>(address);
        }

        const usize start = (used_ + alignment - 1) & ~(alignment - 1);
        if (start > ArenaReserveBytes || bytes > ArenaReserveBytes - start) {
            Fatal("ParkingLot permanent memory is exhausted");
        }

        while (committed_ < start + bytes) {
            if (!CommitVirtualMemory(base_ + committed_, ArenaCommitChunkBytes,
                                     VirtualMemoryAccess::Open)) {
                Fatal("ParkingLot could not commit its permanent memory");
            }

            committed_ += ArenaCommitChunkBytes;
        }

        used_ = start + bytes;
        return base_ + start;
    }

private:
    QueueMutex mutex_;
    unsigned char* base_ = nullptr;
    usize used_ = 0;
    usize committed_ = 0;
};

static_assert(ArenaReserveBytes % ArenaCommitChunkBytes == 0);

constinit PermanentArena arena;
// free list 머리다(arena 잠금 아래).
constinit ThreadRecord* freeRecords = nullptr;
constinit Atomic<Table*> currentTable;
// 표 키우기를 한 번에 하나로 묶는다.
constinit QueueMutex growMutex;
constinit Atomic<uint32> liveThreads;
constinit Atomic<void (*)(const void*) noexcept> hookWaitBegin;
constinit Atomic<void (*)(const void*) noexcept> hookWaitEnd;

// 이 스레드의 레코드 칸이다. 칸 소유자의 소멸자가 레코드를 돌려주고 destroyed를 세운다.
thread_local ThreadRecord* threadRecord = nullptr;
thread_local bool threadRecordDestroyed = false;
// 콜백 안(canWait·onWake)이면 0보다 크다. beforeWait 안이면 inBeforeWait, 훅 안이면 inHook이다.
thread_local uint32 callbackDepth = 0;
thread_local bool inBeforeWait = false;
thread_local bool inHook = false;

ThreadRecord* AllocateRecord() noexcept {
    QueueMutexGuard guard(arena.Mutex());
    ThreadRecord* record = freeRecords;
    if (record != nullptr) {
        freeRecords = record->next;
        record->next = nullptr;
    } else {
        record =
            new (arena.AllocateLocked(sizeof(ThreadRecord), alignof(ThreadRecord))) ThreadRecord();
    }

    record->references.Store<MemoryOrder::Relaxed>(1);
    return record;
}

void ReleaseRecord(ThreadRecord* record) noexcept {
    if (record->references.FetchSub<MemoryOrder::AcquireRelease>(1) == 1) {
        QueueMutexGuard guard(arena.Mutex());
        record->next = freeRecords;
        freeRecords = record;
    }
}

uint32 Log2(uint32 powerOfTwo) noexcept {
    uint32 bits = 0;
    while ((1u << bits) < powerOfTwo) {
        ++bits;
    }

    return bits;
}

Table* CreateTable(uint32 bucketCount) noexcept {
    QueueMutexGuard guard(arena.Mutex());
    auto* table = new (arena.AllocateLocked(sizeof(Table), alignof(Table))) Table();
    table->bucketCount = bucketCount;
    table->shift = AddressBits - Log2(bucketCount);
    table->buckets = static_cast<Bucket**>(
        arena.AllocateLocked(sizeof(Bucket*) * bucketCount, alignof(Bucket*)));
    for (uint32 index = 0; index < bucketCount; ++index) {
        table->buckets[index] =
            new (arena.AllocateLocked(sizeof(Bucket), alignof(Bucket))) Bucket();
    }

    return table;
}

Table* EnsureTable() noexcept {
    Table* table = currentTable.Load<MemoryOrder::Acquire>();
    if (table != nullptr) {
        return table;
    }

    QueueMutexGuard guard(growMutex);
    table = currentTable.Load<MemoryOrder::Acquire>();
    if (table == nullptr) {
        table = CreateTable(MinimumBucketCount);
        currentTable.Store<MemoryOrder::Release>(table);
    }

    return table;
}

uint32 BucketIndex(const Table& table, const void* address) noexcept {
    return static_cast<uint32>((reinterpret_cast<uint64>(address) * AddressHashMultiplier) >>
                               table.shift);
}

// address의 bucket을 잠근다. 잠근 사이 표가 바뀌었으면(키우기) 풀고 새 표에서 다시 잡는다.
Bucket& LockBucket(const void* address) noexcept {
    while (true) {
        Table* table = EnsureTable();
        Bucket& bucket = *table->buckets[BucketIndex(*table, address)];
        bucket.mutex.Lock();
        if (currentTable.Load<MemoryOrder::Acquire>() == table) {
            return bucket;
        }

        bucket.mutex.Unlock();
    }
}

void Append(Bucket& bucket, ThreadRecord* record) noexcept {
    record->next = nullptr;
    if (bucket.tail != nullptr) {
        bucket.tail->next = record;
    } else {
        bucket.head = record;
    }

    bucket.tail = record;
}

// record를 bucket 대기열에서 뗀다(record가 그 bucket에 있어야 한다).
void Remove(Bucket& bucket, ThreadRecord* record) noexcept {
    ThreadRecord* previous = nullptr;
    for (ThreadRecord* current = bucket.head; current != nullptr; current = current->next) {
        if (current == record) {
            if (previous != nullptr) {
                previous->next = current->next;
            } else {
                bucket.head = current->next;
            }

            if (bucket.tail == current) {
                bucket.tail = previous;
            }

            current->next = nullptr;
            return;
        }

        previous = current;
    }

    Fatal("ParkingLot waiter is missing from its bucket");
}

// 살아 있는 스레드 수가 bucket 수를 넘으면 표를 키운다. 모든 bucket을 색인 순서로 잠그고 대기자를
// 새 표로 옮긴다. 같은 주소의 대기자는 같은 옛 bucket에 순서대로 있었으므로 순서가 보존된다.
void GrowIfNeeded(uint32 threads) noexcept {
    if (threads <= EnsureTable()->bucketCount) {
        return;
    }

    QueueMutexGuard guard(growMutex);
    Table* old = currentTable.Load<MemoryOrder::Acquire>();
    if (threads <= old->bucketCount) {
        return;
    }

    uint32 bucketCount = old->bucketCount;
    while (bucketCount < threads * BucketsPerThread) {
        bucketCount *= 2;
    }

    for (uint32 index = 0; index < old->bucketCount; ++index) {
        old->buckets[index]->mutex.Lock();
    }

    Table* fresh = CreateTable(bucketCount);
    for (uint32 index = 0; index < old->bucketCount; ++index) {
        Bucket& bucket = *old->buckets[index];
        ThreadRecord* record = bucket.head;
        while (record != nullptr) {
            ThreadRecord* next = record->next;
            Append(*fresh->buckets[BucketIndex(*fresh, record->waitAddress)], record);
            record = next;
        }

        bucket.head = nullptr;
        bucket.tail = nullptr;
    }

    currentTable.Store<MemoryOrder::Release>(fresh);
    for (uint32 index = 0; index < old->bucketCount; ++index) {
        old->buckets[index]->mutex.Unlock();
    }
}

// 스레드 종료 때 레코드 칸을 돌려준다. 그 뒤의 대기는 임시 레코드를 쓴다(D31).
struct ThreadRecordOwner {
    ThreadRecordOwner() noexcept = default;

    ~ThreadRecordOwner() noexcept {
        if (threadRecord != nullptr) {
            ReleaseRecord(threadRecord);
            threadRecord = nullptr;
            liveThreads.FetchSub<MemoryOrder::Relaxed>(1);
        }

        threadRecordDestroyed = true;
    }

    // thread_local 소유자를 만들어 소멸자를 등록한다.
    void Touch() noexcept {
    }
};

thread_local ThreadRecordOwner threadRecordOwner;

struct RecordUse {
    ThreadRecord* record;
    bool temporary;
};

RecordUse AcquireRecordForWait() noexcept {
    if (threadRecordDestroyed) {
        return {AllocateRecord(), true};
    }

    if (threadRecord == nullptr) {
        threadRecordOwner.Touch();
        threadRecord = AllocateRecord();
        GrowIfNeeded(liveThreads.FetchAdd<MemoryOrder::Relaxed>(1) + 1);
    }

    return {threadRecord, false};
}

void CheckWaitAllowed() noexcept {
#if !ROCKS_BUILD_RELEASE
    if (callbackDepth != 0 || inBeforeWait || inHook) {
        Fatal("ParkingLot wait called from a ParkingLot callback or hook");
    }
#endif
}

void CheckWakeAllowed() noexcept {
#if !ROCKS_BUILD_RELEASE
    if (callbackDepth != 0 || inHook) {
        Fatal("ParkingLot wake called from a ParkingLot callback or hook");
    }
#endif
}

void CallHook(const Atomic<void (*)(const void*) noexcept>& slot, const void* address) noexcept {
    void (*hook)(const void*) noexcept = slot.Load<MemoryOrder::Relaxed>();
    if (hook == nullptr || inHook) {
        return;
    }

    inHook = true;
    hook(address);
    inHook = false;
}

#if !ROCKS_BUILD_RELEASE
void CallTestHook(SyncParkFunction hook, const void* address) noexcept {
    if (hook == nullptr || inHook) {
        return;
    }

    inHook = true;
    hook(address);
    inHook = false;
}
#endif

// address의 대기자를 최대 count개 큐에서 꺼내 지역 목록으로 엮는다(bucket 잠금 아래). 같은 주소에
// 남은 대기자가 있으면 more가 true다.
ThreadRecord* Dequeue(Bucket& bucket, const void* address, uint32 count, uint32& woken,
                      bool& more) noexcept {
    ThreadRecord* listHead = nullptr;
    ThreadRecord* listTail = nullptr;
    ThreadRecord* previous = nullptr;
    ThreadRecord* record = bucket.head;
    woken = 0;
    more = false;
    while (record != nullptr) {
        ThreadRecord* next = record->next;
        if (record->waitAddress != address) {
            previous = record;
            record = next;
            continue;
        }

        if (woken == count) {
            more = true;
            break;
        }

        if (previous != nullptr) {
            previous->next = next;
        } else {
            bucket.head = next;
        }

        if (bucket.tail == record) {
            bucket.tail = previous;
        }

        // 깨우는 쪽 참조다. Notify를 마친 뒤 돌려준다(그 전에 레코드가 다른 스레드로 가지 않는다).
        record->references.FetchAdd<MemoryOrder::Relaxed>(1);
        record->waitAddress = nullptr;
        record->next = nullptr;
        if (listTail != nullptr) {
            listTail->next = record;
        } else {
            listHead = record;
        }

        listTail = record;
        ++woken;
        record = next;
    }

    return listHead;
}

// 잠금을 푼 뒤 깨운다. Notify 뒤로 레코드의 칸을 읽지 않는다(대기자가 돌아가 다시 기다릴 수 있다).
void NotifyAll(ThreadRecord* list) noexcept {
    while (list != nullptr) {
        ThreadRecord* next = list->next;
        ROCKS_SYNC_STEP("ParkingLot.Notify");
        list->signal.Notify();
        ReleaseRecord(list);
        list = next;
    }
}

} // namespace

ParkingLotWaitOutcome ParkingLotWait(const void* address, ParkingLotCanWait canWait,
                                     void* canWaitContext, ParkingLotBeforeWait beforeWait,
                                     void* beforeWaitContext, uint32 timeoutMs) noexcept {
    CheckWaitAllowed();
    // 대기 표시를 밖에서 세운 뒤 bucket에 들어가기 전 지점이다(10.3절 경쟁 시험이 여기서 멈춘다).
    ROCKS_SYNC_STEP("ParkingLot.WaitBegin");
    const RecordUse use = AcquireRecordForWait();
    ThreadRecord* record = use.record;
    CallHook(hookWaitBegin, address);
    {
        Bucket& bucket = LockBucket(address);
        ++callbackDepth;
        const bool wait = canWait(canWaitContext);
        --callbackDepth;
        if (!wait) {
            bucket.mutex.Unlock();
            CallHook(hookWaitEnd, address);
            if (use.temporary) {
                ReleaseRecord(record);
            }

            return {};
        }

        record->signal.Reset();
        record->waitAddress = address;
        record->wakeToken = 0;
        Append(bucket, record);
        bucket.mutex.Unlock();
    }

    ROCKS_SYNC_STEP("ParkingLot.Enqueued");
    inBeforeWait = true;
    beforeWait(beforeWaitContext);
    inBeforeWait = false;
#if !ROCKS_BUILD_RELEASE
    CallTestHook(SyncTestOnParked(), address);
#endif

    bool woken = true;
    if (timeoutMs == InfiniteTimeout) {
        record->signal.Wait();
    } else if (!record->signal.WaitFor(timeoutMs)) {
        // 시간 초과: 아직 큐에 있으면 나온다. 이미 깨우는 쪽이 꺼냈으면 그 Notify를 기다린다(그래야
        // 깨우는 쪽이 끝나기 전에 레코드를 다시 쓰지 않는다).
        Bucket& bucket = LockBucket(address);
        if (record->waitAddress != nullptr) {
            Remove(bucket, record);
            record->waitAddress = nullptr;
            bucket.mutex.Unlock();
            woken = false;
        } else {
            bucket.mutex.Unlock();
            record->signal.Wait();
        }
    }

#if !ROCKS_BUILD_RELEASE
    CallTestHook(SyncTestOnUnparked(), address);
#endif
    const uint64 token = woken ? record->wakeToken : 0;
    CallHook(hookWaitEnd, address);
    if (use.temporary) {
        ReleaseRecord(record);
    }

    return {true, woken, token};
}

void ParkingLotWakeOne(const void* address, ParkingLotWakeOneCallback onWake,
                       void* context) noexcept {
    CheckWakeAllowed();
    ROCKS_SYNC_STEP("ParkingLot.WakeOne");
    Bucket& bucket = LockBucket(address);
    uint32 woken = 0;
    bool more = false;
    ThreadRecord* list = Dequeue(bucket, address, 1, woken, more);
    ++callbackDepth;
    const uint64 token = onWake(context, {woken != 0, more});
    --callbackDepth;
    if (list != nullptr) {
        list->wakeToken = token;
    }

    bucket.mutex.Unlock();
    NotifyAll(list);
}

uint32 ParkingLotWakeMultiple(const void* address, uint32 count, ParkingLotWakeCallback onWake,
                              void* context) noexcept {
    CheckWakeAllowed();
    ROCKS_SYNC_STEP("ParkingLot.WakeMultiple");
    Bucket& bucket = LockBucket(address);
    uint32 woken = 0;
    bool more = false;
    ThreadRecord* list = Dequeue(bucket, address, count, woken, more);
    if (onWake != nullptr) {
        ++callbackDepth;
        onWake(context, {woken != 0, more});
        --callbackDepth;
    }

    bucket.mutex.Unlock();
    NotifyAll(list);
    return woken;
}

#if !ROCKS_BUILD_RELEASE
uint32 ParkingLotFreeRecordCount() noexcept {
    QueueMutexGuard guard(arena.Mutex());
    uint32 count = 0;
    for (const ThreadRecord* record = freeRecords; record != nullptr; record = record->next) {
        ++count;
    }

    return count;
}
#endif

} // namespace rocks::platform::detail

namespace rocks::platform {

void InstallSyncHooks(const SyncHooks& hooks) noexcept {
    detail::hookWaitBegin.Store<MemoryOrder::Release>(hooks.onWaitBegin);
    detail::hookWaitEnd.Store<MemoryOrder::Release>(hooks.onWaitEnd);
}

} // namespace rocks::platform
