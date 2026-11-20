#include "PackageInternal.h"

#include <AssetRuntime/AssetLoader.h>
#include <AssetRuntime/Package.h>
#include <Core/Function.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringBuilder.h>
#include <Object/ConstructionContext.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <Task/WorkerScheduler.h>

// 쿠킹 패키지 로더다(Docs/Design/AssetSystem.ko.md 11.6절). 모든 상태 전이는 소유(Game) 스레드가
// 하고, IO·worker 스레드는 패키지 작업의 버퍼·로드 상태만 만진 뒤 완료 큐에 작업을 넣는다(할당
// 없음).
namespace rocks {

namespace {

constexpr uint32 LoaderNoIndex = NumericLimits<uint32>::Max();
// 패키지 경로 builder의 내장 용량이다("<루트>/Packages/xx/<36자>-<16자>.rpkg" — 루트가 짧으면 할당
// 없이 담긴다).
constexpr usize LoaderPathInlineBytes = 160;
// FindByPath 질의 키 builder의 내장 용량이다(경로 상한 256 byte 근처까지 할당 없이).
constexpr usize LoaderKeyInlineBytes = 272;

enum class LoaderCompletion : uint8 {
    None,
    Io,
    Worker
};

enum class LoaderRetire : uint8 {
    None,
    Fail,
    Abandon
};

struct LoaderCompletionQueue;

// 패키지 하나의 로드 작업이다. 주소가 고정이어야 하므로(IO·worker가 포인터를 쥔다) 하나씩 할당한다.
struct LoaderPackageJob {
    explicit LoaderPackageJob(Allocator& allocator) noexcept : file(allocator), path(allocator) {
    }

    LoaderCompletionQueue* queue = nullptr;
    const CookedManifest* manifest = nullptr;
    uint32 entry = 0;
    uint32 unit = 0;
    AssetLoaderPackagePhase phase = AssetLoaderPackagePhase::Queued;
    UniquePtr<PackageLoadState> state;
    Array<uint8> file;
    StringBuilder<LoaderPathInlineBytes> path;
    CookedPackageRead read;
    // io.Release가 남았는지다(Begin 성공 뒤 완료를 처리할 때까지).
    bool readBegun = false;
    // IO·worker에 넘긴 일이 있는지다(Game 스레드가 보는 값 — 완료를 꺼낼 때 지운다).
    bool asyncPending = false;
    // 버린 작업 표시다(worker가 원자 값으로 읽는다 — 1이면 Prepare를 하지 않는다).
    uint32 abandoned = 0;
    // 완료 결과(게시한 스레드가 쓰고, 큐 뮤텍스가 Game 스레드에 넘긴다).
    LoaderCompletion completion = LoaderCompletion::None;
    CookedPackageReadResult ioResult;
    ErrorCode workerError = ErrorCode::None;
    AssetDiagnostic workerDiagnostic;
    LoaderPackageJob* nextCompletion = nullptr;
    uint64 tempBytes = 0;
    uint64 phaseStart = 0;
    uint64 phaseNanoseconds[AssetLoaderPackagePhaseCount] = {};
};

// 완료 큐다(침입 연결 — 게시에 할당이 없다).
struct LoaderCompletionQueue {
    platform::Mutex mutex;
    platform::ConditionVariable changed;
    LoaderPackageJob* head = nullptr;
    LoaderPackageJob* tail = nullptr;
};

void PostLoaderCompletion(LoaderPackageJob& job) noexcept {
    LoaderCompletionQueue& queue = *job.queue;
    queue.mutex.Lock();
    job.nextCompletion = nullptr;
    if (queue.tail == nullptr) {
        queue.head = &job;
    } else {
        queue.tail->nextCompletion = &job;
    }

    queue.tail = &job;
    // 깨움도 뮤텍스 안에서 한다 — 놓은 뒤에는 job·queue를 만지지 않는다(Game이 바로 지울 수 있다).
    queue.changed.NotifyAll();
    queue.mutex.Unlock();
}

void OnLoaderReadComplete(CookedPackageRead& read, const CookedPackageReadResult& result) noexcept {
    auto& job = *static_cast<LoaderPackageJob*>(read.owner);
    job.ioResult = result;
    job.completion = LoaderCompletion::Io;
    PostLoaderCompletion(job);
}

// worker 단계다(11.6절 3): 크기·SHA-256 → Prepare(압축 해제·검증) → 내용 대조.
void RunLoaderPrepare(void* context) noexcept {
    auto& job = *static_cast<LoaderPackageJob*>(context);
    job.completion = LoaderCompletion::Worker;
    job.workerError = ErrorCode::None;
    if (platform::AtomicLoad<platform::MemoryOrder::Acquire>(&job.abandoned) != 0) {
        job.workerError = ErrorCode::Cancelled;
        PostLoaderCompletion(job);
        return;
    }

    const CookedManifestEntry& entry = job.manifest->entries[job.entry];
    AssetDiagnostic diagnostic;
    Result<void> step = CheckCookedPackageFile(entry, job.file.View(), &diagnostic);
    if (step) {
        step = job.state->Prepare(Move(job.file), &diagnostic);
    }

    if (step) {
        step = CheckCookedPackageContents(*job.manifest, entry, job.state->Contents(), &diagnostic);
    }

    if (!step) {
        job.workerError = step.Error();
        job.workerDiagnostic = diagnostic;
    }

    PostLoaderCompletion(job);
}

uint64 LoaderDefaultClock(void*) noexcept {
    return platform::MonotonicNanoseconds();
}

// 요청 기록이다(번호 재사용 — 세대로 옛 handle을 거른다).
struct LoaderRequest {
    uint32 generation = 1;
    bool used = false;
    uint32 nextFree = LoaderNoIndex;
    uint32 entry = 0;
    LoadPriority priority = LoadPriority::Normal;
    CancelToken cancel;
    AssetLoadState state = AssetLoadState::Pending;
    ErrorCode error = ErrorCode::None;
    AssetDiagnostic diagnostic;
    ObjectHandle package{};
    uint32 unit = LoaderNoIndex;
};

// 로드 단위(Hard SCC)다.
struct LoaderUnit {
    explicit LoaderUnit(Allocator& allocator) noexcept
        : members(allocator), jobs(allocator), dependencies(allocator), dependents(allocator),
          requests(allocator) {
    }

    bool used = false;
    uint32 nextFree = LoaderNoIndex;
    AssetLoaderUnitPhase phase = AssetLoaderUnitPhase::Queued;
    // 구성 패키지(매니페스트 항목 번호 — 오름차순)와 허가 뒤의 작업(같은 순서)이다.
    Array<uint32> members;
    Array<LoaderPackageJob*> jobs;
    // 아직 공개되지 않은 의존 단위와 그 단위에 기대는 단위다.
    Array<uint32> dependencies;
    Array<uint32> dependents;
    // 이 단위의 패키지를 기다리는 Pending 요청이다.
    Array<uint32> requests;
    uint32 unadmittedDependencies = 0;
    uint32 waitingDependencies = 0;
    uint32 interest = 0;
    uint32 preparedPackages = 0;
    uint32 aliveJobs = 0;
    LoadPriority priority = LoadPriority::Normal;
    uint64 sequence = 0;
    uint64 queuedAt = 0;
    uint64 tempBytes = 0;
    // 정리 작업 목록(침입 연결)이다.
    LoaderRetire retire = LoaderRetire::None;
    uint32 nextRetire = LoaderNoIndex;
    // 실패 원인(실패면): 처음 실패한 패키지 항목·오류·진단이다.
    uint32 failedEntry = LoaderNoIndex;
    ErrorCode failError = ErrorCode::None;
    AssetDiagnostic failDiagnostic;
};

// Tarjan 반복 구현의 틀 하나다.
struct LoaderTarjanFrame {
    uint32 entry = 0;
    uint32 position = 0;
};

// 여러 할당 단계의 처음 실패를 first에 남긴다(단계는 모두 실행한다 — 실패 뒤 단계는 버릴 상태에
// 용량만 더 잡는다).
void KeepFirst(ErrorCode& first, Result<void>&& result) noexcept {
    if (first == ErrorCode::None && !result) {
        first = result.Error();
    }
}

bool ValidPriority(LoadPriority priority) noexcept {
    return static_cast<uint32>(priority) < LoadPriorityCount;
}

} // namespace

struct AssetLoader::Impl {
    Impl(AssetLoader& owner, const AssetLoaderConfig& config) noexcept
        : owner(owner), config(config), allocator(*config.allocator),
          access(config.registry->OwnerAccess()), root(allocator), entryUnit(allocator),
          entryLoaded(allocator), keyBytes(allocator), keyOffsets(allocator), keyOrder(allocator),
          loadedRoots(allocator), loadedHandles(allocator), requests(allocator), units(allocator),
          admitCandidates(allocator), readyUnits(allocator), tarjanIndex(allocator),
          tarjanLow(allocator), tarjanOnStack(allocator), tarjanVisited(allocator),
          tarjanFrames(allocator), tarjanStack(allocator), sccMembers(allocator),
          sccStarts(allocator), newUnits(allocator), dependencyCounts(allocator),
          workStack(allocator) {
    }

    AssetLoader& owner;
    const AssetLoaderConfig& config;
    Allocator& allocator;
    ObjectAccessContext access;
    uint64 ownerThread = 0;
    StringBuilder<LoaderPathInlineBytes> root;
    bool shutDown = false;

    // 매니페스트 항목마다 진행 중 단위 번호와 공개 번호(없으면 LoaderNoIndex)다.
    Array<uint32> entryUnit;
    Array<uint32> entryLoaded;
    // FindByPath 색인: 항목마다 caseless 키를 이어 붙인 byte와 시작 위치, 키 순으로 정렬한 항목
    // 번호.
    Array<char> keyBytes;
    Array<uint64> keyOffsets;
    Array<uint32> keyOrder;
    // 공개한 패키지다.
    Array<RootRef<Object>> loadedRoots;
    Array<ObjectHandle> loadedHandles;

    Array<LoaderRequest> requests;
    uint32 freeRequest = LoaderNoIndex;
    Array<LoaderUnit> units;
    uint32 freeUnit = LoaderNoIndex;
    uint32 usedUnits = 0;
    // 허가 후보(의존 단위가 모두 허가됨)와 공개 준비 단위다. 용량은 단위 표 크기 이상으로 미리
    // 잡는다(추가에 할당이 없다).
    Array<uint32> admitCandidates;
    Array<uint32> readyUnits;
    uint32 retireHead = LoaderNoIndex;
    uint32 retireTail = LoaderNoIndex;

    // ResolveManifest 작업 공간이다(항목 표 크기 — 쓴 칸은 끝에 되돌린다).
    Array<uint32> tarjanIndex;
    Array<uint32> tarjanLow;
    Array<uint8> tarjanOnStack;
    Array<uint32> tarjanVisited;
    Array<LoaderTarjanFrame> tarjanFrames;
    Array<uint32> tarjanStack;
    Array<uint32> sccMembers;
    Array<uint32> sccStarts;
    Array<uint32> newUnits;
    Array<uint32> dependencyCounts;
    Array<uint32> workStack;

    LoaderCompletionQueue queue;
    // IO·worker에 넘긴 일 수다(Game 스레드 값).
    uint32 asyncInFlight = 0;
    uint32 packagesInFlight = 0;
    uint64 tempBytesInFlight = 0;
    uint64 admissions = 0;
    uint64 nextSequence = 0;
    uint64 syncWaits = 0;
    uint64 syncWaitNanoseconds = 0;

    uint64 Now() const noexcept {
        return config.clock != nullptr ? config.clock(config.clockContext)
                                       : LoaderDefaultClock(nullptr);
    }

    bool OnOwnerThread() const noexcept {
        return platform::CurrentThreadId() == ownerThread;
    }

    uint32 EntryIndex(const CookedManifestEntry* entry) const noexcept {
        return static_cast<uint32>(entry - config.manifest->entries.Data());
    }

    AssetId EntryAsset(uint32 entry) const noexcept {
        return config.manifest->entries[entry].asset;
    }

    LoadPriority Effective(const LoaderUnit& unit) const noexcept {
        const uint64 waited = admissions - unit.queuedAt;
        const uint64 bonus = waited / config.limits.agingAdmissions;
        const uint64 raised = static_cast<uint64>(unit.priority) + bonus;
        const uint64 top = static_cast<uint64>(LoadPriority::Critical);
        return static_cast<LoadPriority>(raised < top ? raised : top);
    }

    // a가 b보다 먼저인지다(실효 우선순위 → 생성 순서).
    bool Before(uint32 a, uint32 b) const noexcept {
        const auto pa = static_cast<uint32>(Effective(units[a]));
        const auto pb = static_cast<uint32>(Effective(units[b]));
        if (pa != pb) {
            return pa > pb;
        }

        return units[a].sequence < units[b].sequence;
    }

    // ---- 초기화 ----

    Result<void> BuildPathIndex() noexcept {
        const CookedManifest& manifest = *config.manifest;
        const usize count = manifest.entries.Size();
        if (auto reserved = keyOffsets.TryReserve(count + 1); !reserved) {
            return reserved;
        }

        if (auto reserved = keyOrder.TryReserve(count); !reserved) {
            return reserved;
        }

        (void)keyOffsets.TryPushBack(0);
        for (usize index = 0; index < count; ++index) {
            StringBuilder<LoaderKeyInlineBytes> key(allocator);
            if (auto made = package_detail::CheckCookedPathHint(
                    manifest.entries[index].pathHint.View(), allocator, key);
                !made) {
                // 매니페스트는 검증을 마쳤다 — 남는 실패는 할당뿐이다.
                return made;
            }

            const StringView view = key.View();
            if (auto appended = keyBytes.TryAppend(Span<const char>(view.Data(), view.ByteCount()));
                !appended) {
                return appended;
            }

            (void)keyOffsets.TryPushBack(static_cast<uint64>(keyBytes.Size()));
            (void)keyOrder.TryPushBack(static_cast<uint32>(index));
        }

        keyOrder.Sort([this](uint32 left, uint32 right) noexcept {
            return package_detail::Utf8ByteLess(KeyOf(left), KeyOf(right));
        });
        return Result<void>::Success();
    }

    StringView KeyOf(uint32 entry) const noexcept {
        const uint64 begin = keyOffsets[entry];
        const uint64 end = keyOffsets[entry + 1];
        return StringView(keyBytes.Data() + begin, static_cast<usize>(end - begin));
    }

    Result<void> Initialize(AssetDiagnostic* failure) noexcept {
        const usize count = config.manifest->entries.Size();
        if (auto checked =
                CheckCookedManifestProfile(*config.manifest, config.runtimeProfile, failure);
            !checked) {
            return checked;
        }

        auto resource = [failure](Result<void> result) noexcept {
            if (!result && failure != nullptr) {
                *failure = AssetDiagnostic{};
                failure->error = result.Error();
            }

            return result;
        };
        if (auto copied = root.TryAppend(config.cookedRoot); !copied) {
            return resource(Move(copied));
        }

        ErrorCode sized = ErrorCode::None;
        KeepFirst(sized, entryUnit.TryResize(count));
        KeepFirst(sized, entryLoaded.TryResize(count));
        KeepFirst(sized, tarjanIndex.TryResize(count));
        KeepFirst(sized, tarjanLow.TryResize(count));
        KeepFirst(sized, tarjanOnStack.TryResize(count));
        if (sized != ErrorCode::None) {
            return resource(Result<void>::Failure(sized));
        }

        for (usize index = 0; index < count; ++index) {
            entryUnit[index] = LoaderNoIndex;
            entryLoaded[index] = LoaderNoIndex;
            tarjanIndex[index] = LoaderNoIndex;
        }

        return resource(BuildPathIndex());
    }

    // ---- 단위 표 ----

    // 단위 칸 하나를 꺼낸다(용량은 호출자가 미리 잡았다 — 할당하지 않는다).
    uint32 TakeUnitSlot() noexcept {
        uint32 index = freeUnit;
        if (index != LoaderNoIndex) {
            freeUnit = units[index].nextFree;
        } else {
            index = static_cast<uint32>(units.Size());
            (void)units.TryEmplaceBack(allocator);
        }

        LoaderUnit& unit = units[index];
        unit.used = true;
        unit.nextFree = LoaderNoIndex;
        ++usedUnits;
        return index;
    }

    void FreeUnitSlot(uint32 index) noexcept {
        LoaderUnit& unit = units[index];
        unit.used = false;
        unit.members.Clear();
        unit.jobs.Clear();
        unit.dependencies.Clear();
        unit.dependents.Clear();
        unit.requests.Clear();
        unit.unadmittedDependencies = 0;
        unit.waitingDependencies = 0;
        unit.interest = 0;
        unit.preparedPackages = 0;
        unit.aliveJobs = 0;
        unit.tempBytes = 0;
        unit.retire = LoaderRetire::None;
        unit.nextRetire = LoaderNoIndex;
        unit.failedEntry = LoaderNoIndex;
        unit.failError = ErrorCode::None;
        unit.failDiagnostic = AssetDiagnostic{};
        unit.phase = AssetLoaderUnitPhase::Queued;
        unit.nextFree = freeUnit;
        freeUnit = index;
        --usedUnits;
    }

    static void RemoveValue(Array<uint32>& values, uint32 value) noexcept {
        const usize at = values.Find(value);
        if (at < values.Size()) {
            (void)values.TryRemoveAtSwap(at);
        }
    }

    // ---- ResolveManifest(11.6절 2) ----

    void ResetTarjan() noexcept {
        for (uint32 entry : tarjanVisited) {
            tarjanIndex[entry] = LoaderNoIndex;
            tarjanOnStack[entry] = 0;
        }

        tarjanVisited.Clear();
        tarjanFrames.Clear();
        tarjanStack.Clear();
    }

    // 항목 entry의 i번째 의존이 이번 해석에서 따라갈 Hard 대상이면 그 항목 번호, 아니면
    // LoaderNoIndex다.
    uint32 HardTarget(uint32 entry, uint32 position) const noexcept {
        const CookedManifestDependency& dependency =
            config.manifest->entries[entry].dependencies[position];
        if (dependency.kind != PackageDependencyKind::Hard) {
            return LoaderNoIndex;
        }

        // 매니페스트가 Hard 대상의 존재를 보장한다(AS 10.1절 6).
        return EntryIndex(config.manifest->Find(dependency.asset));
    }

    bool IsNew(uint32 entry) const noexcept {
        return entryLoaded[entry] == LoaderNoIndex && entryUnit[entry] == LoaderNoIndex;
    }

    Result<void> Visit(uint32 entry, uint32& counter) noexcept {
        if (auto pushed = tarjanVisited.TryPushBack(entry); !pushed) {
            return pushed;
        }

        if (auto pushed = tarjanFrames.TryPushBack(LoaderTarjanFrame{entry, 0}); !pushed) {
            (void)tarjanVisited.TryPopBack();
            return pushed;
        }

        if (auto pushed = tarjanStack.TryPushBack(entry); !pushed) {
            return pushed;
        }

        tarjanIndex[entry] = counter;
        tarjanLow[entry] = counter;
        tarjanOnStack[entry] = 1;
        ++counter;
        return Result<void>::Success();
    }

    // root에서 닿는 새 항목들의 SCC를 sccMembers·sccStarts에 의존 먼저 순서로 담는다(반복 Tarjan).
    Result<void> FindComponents(uint32 rootEntry) noexcept {
        sccMembers.Clear();
        sccStarts.Clear();
        uint32 counter = 0;
        if (auto visited = Visit(rootEntry, counter); !visited) {
            return visited;
        }

        while (!tarjanFrames.IsEmpty()) {
            LoaderTarjanFrame& frame = tarjanFrames[tarjanFrames.Size() - 1];
            const uint32 entry = frame.entry;
            const auto count =
                static_cast<uint32>(config.manifest->entries[entry].dependencies.Size());
            bool descended = false;
            while (frame.position < count) {
                const uint32 target = HardTarget(entry, frame.position);
                ++frame.position;
                if (target == LoaderNoIndex || !IsNew(target)) {
                    continue;
                }

                if (tarjanIndex[target] == LoaderNoIndex) {
                    if (auto visited = Visit(target, counter); !visited) {
                        return visited;
                    }

                    descended = true;
                    break;
                }

                if (tarjanOnStack[target] != 0 && tarjanIndex[target] < tarjanLow[entry]) {
                    tarjanLow[entry] = tarjanIndex[target];
                }
            }

            if (descended) {
                continue;
            }

            (void)tarjanFrames.TryPopBack();
            if (!tarjanFrames.IsEmpty()) {
                const uint32 parent = tarjanFrames[tarjanFrames.Size() - 1].entry;
                if (tarjanLow[entry] < tarjanLow[parent]) {
                    tarjanLow[parent] = tarjanLow[entry];
                }
            }

            if (tarjanLow[entry] != tarjanIndex[entry]) {
                continue;
            }

            if (auto pushed = sccStarts.TryPushBack(static_cast<uint32>(sccMembers.Size()));
                !pushed) {
                return pushed;
            }

            uint32 member = LoaderNoIndex;
            do {
                member = tarjanStack[tarjanStack.Size() - 1];
                (void)tarjanStack.TryPopBack();
                tarjanOnStack[member] = 0;
                if (auto pushed = sccMembers.TryPushBack(member); !pushed) {
                    return pushed;
                }
            } while (member != entry);
        }

        return Result<void>::Success();
    }

    // 새 단위들을 지운다(해석 실패 되돌림 — 아직 다른 단위·항목 표에 잇지 않았다).
    void DiscardNewUnits() noexcept {
        for (uint32 unit : newUnits) {
            FreeUnitSlot(unit);
        }

        newUnits.Clear();
    }

    // rootEntry에서 새 단위들을 만들어 잇는다. 성공하면 rootEntry가 속한 단위 번호를 준다(그 단위의
    // 요청 자리 하나와 Boost 작업 목록 용량도 잡는다). 실패(할당)면 로더 상태는 그대로다.
    Result<uint32> Resolve(uint32 rootEntry) noexcept {
        auto fail = [this](ErrorCode error) noexcept {
            ResetTarjan();
            DiscardNewUnits();
            return Result<uint32>::Failure(error);
        };
        newUnits.Clear();
        if (auto found = FindComponents(rootEntry); !found) {
            return fail(found.Error());
        }

        const auto components = static_cast<uint32>(sccStarts.Size());
        if (auto reserved = units.TryReserve(units.Size() + components); !reserved) {
            return fail(reserved.Error());
        }

        if (auto reserved = newUnits.TryReserve(components); !reserved) {
            return fail(reserved.Error());
        }

        for (uint32 component = 0; component < components; ++component) {
            const uint32 begin = sccStarts[component];
            const uint32 end = component + 1 < components ? sccStarts[component + 1]
                                                          : static_cast<uint32>(sccMembers.Size());
            const uint32 index = TakeUnitSlot();
            (void)newUnits.TryPushBack(index);
            LoaderUnit& unit = units[index];
            if (auto copied = unit.members.TryAppend(
                    Span<const uint32>(sccMembers.Data() + begin, end - begin));
                !copied) {
                return fail(copied.Error());
            }

            unit.members.Sort();
        }

        // 새 단위의 구성 항목에 임시로 단위 번호를 적어 간선을 찾는다(실패하면 되돌린다).
        for (uint32 unit : newUnits) {
            for (uint32 member : units[unit].members) {
                entryUnit[member] = unit;
            }
        }

        auto unmark = [this]() noexcept {
            for (uint32 unit : newUnits) {
                for (uint32 member : units[unit].members) {
                    entryUnit[member] = LoaderNoIndex;
                }
            }
        };
        for (uint32 unit : newUnits) {
            for (uint32 member : units[unit].members) {
                const auto count =
                    static_cast<uint32>(config.manifest->entries[member].dependencies.Size());
                for (uint32 position = 0; position < count; ++position) {
                    const uint32 target = HardTarget(member, position);
                    if (target == LoaderNoIndex || entryLoaded[target] != LoaderNoIndex) {
                        continue;
                    }

                    const uint32 other = entryUnit[target];
                    if (other == unit || units[unit].dependencies.Contains(other)) {
                        continue;
                    }

                    if (auto pushed = units[unit].dependencies.TryPushBack(other); !pushed) {
                        unmark();
                        return fail(pushed.Error());
                    }
                }
            }
        }

        // 대상 단위의 dependents·목록 용량을 미리 잡는다(아래 잇기는 할당하지 않는다).
        ErrorCode reserved = ErrorCode::None;
        for (uint32 unit : newUnits) {
            for (uint32 other : units[unit].dependencies) {
                LoaderUnit& target = units[other];
                KeepFirst(reserved,
                          target.dependents.TryReserve(target.dependents.Size() + newUnits.Size()));
            }
        }

        KeepFirst(reserved, admitCandidates.TryReserve(units.Size()));
        KeepFirst(reserved, readyUnits.TryReserve(units.Size()));
        KeepFirst(reserved, workStack.TryReserve(units.Size()));
        KeepFirst(reserved, units[entryUnit[rootEntry]].requests.TryReserve(1));
        if (reserved != ErrorCode::None) {
            unmark();
            return fail(reserved);
        }

        // 잇기(할당 없음): 생성 순서 = Tarjan 완료 순서(의존 먼저).
        for (uint32 unit : newUnits) {
            LoaderUnit& made = units[unit];
            made.phase = AssetLoaderUnitPhase::Queued;
            // 우선순위는 요청이 Boost로 올린다(새 단위는 모두 root에서 닿는다).
            made.priority = LoadPriority::Low;
            made.sequence = nextSequence++;
            made.queuedAt = admissions;
            for (uint32 other : made.dependencies) {
                LoaderUnit& target = units[other];
                (void)target.dependents.TryPushBack(unit);
                ++target.interest;
                ++made.waitingDependencies;
                if (target.phase == AssetLoaderUnitPhase::Queued) {
                    ++made.unadmittedDependencies;
                }
            }
        }

        for (uint32 unit : newUnits) {
            if (units[unit].unadmittedDependencies == 0) {
                (void)admitCandidates.TryPushBack(unit);
            }
        }

        const uint32 target = entryUnit[rootEntry];
        ResetTarjan();
        newUnits.Clear();
        return Result<uint32>::Success(target);
    }

    // unit과 그 의존 단위의 우선순위를 priority로 올린다(UE UpdatePackagePriorityRecursive —
    // 올리기만 한다). workStack 용량은 단위 표 크기 이상이다.
    void Boost(uint32 start, LoadPriority priority) noexcept {
        if (static_cast<uint32>(units[start].priority) >= static_cast<uint32>(priority)) {
            return;
        }

        workStack.Clear();
        units[start].priority = priority;
        (void)workStack.TryPushBack(start);
        while (!workStack.IsEmpty()) {
            const uint32 unit = workStack[workStack.Size() - 1];
            (void)workStack.TryPopBack();
            for (uint32 other : units[unit].dependencies) {
                if (static_cast<uint32>(units[other].priority) < static_cast<uint32>(priority)) {
                    units[other].priority = priority;
                    (void)workStack.TryPushBack(other);
                }
            }
        }
    }

    // ---- 요청 ----

    bool ValidHandle(AssetLoadHandle handle) const noexcept {
        return handle.index < requests.Size() && requests[handle.index].used &&
               requests[handle.index].generation == handle.generation;
    }

    void FinishRequest(uint32 index, AssetLoadState state, ErrorCode error,
                       const AssetDiagnostic& diagnostic, ObjectHandle package) noexcept {
        LoaderRequest& request = requests[index];
        request.state = state;
        request.error = error;
        request.diagnostic = diagnostic;
        request.package = package;
        request.unit = LoaderNoIndex;
        request.cancel = CancelToken();
    }

    void FreeRequest(uint32 index) noexcept {
        LoaderRequest& request = requests[index];
        request.used = false;
        ++request.generation;
        request.cancel = CancelToken();
        request.nextFree = freeRequest;
        freeRequest = index;
    }

    // ---- 정리(실패·버림 — 침입 목록, 할당 없음) ----

    void QueueRetire(uint32 unit, LoaderRetire kind) noexcept {
        // 이미 정리 중인(Failed·Abandoned) 단위는 어느 목록에도 남지 않아 여기 오지 않는다.
        LoaderUnit& target = units[unit];
        if (target.retire != LoaderRetire::None) {
            return;
        }

        target.retire = kind;
        target.nextRetire = LoaderNoIndex;
        if (retireTail == LoaderNoIndex) {
            retireHead = unit;
        } else {
            units[retireTail].nextRetire = unit;
        }

        retireTail = unit;
    }

    void DropInterest(uint32 unit) noexcept {
        // 관심 수는 요청·기대는 단위마다 정확히 하나씩 걸고 놓는다.
        LoaderUnit& target = units[unit];
        --target.interest;
        if (target.interest == 0) {
            QueueRetire(unit, LoaderRetire::Abandon);
        }
    }

    // 실패한 단위 unit의 원인을 적고 정리 목록에 넣는다.
    void FailUnit(uint32 unit, uint32 failedEntry, ErrorCode error,
                  const AssetDiagnostic& diagnostic) noexcept {
        // 같은 정리 묶음에서 먼저 실패(또는 버림)로 들어간 단위는 그 원인을 지킨다.
        LoaderUnit& target = units[unit];
        if (target.retire != LoaderRetire::None) {
            return;
        }

        target.failedEntry = failedEntry;
        target.failError = error;
        target.failDiagnostic = diagnostic;
        QueueRetire(unit, LoaderRetire::Fail);
    }

    // job을 지운다. IO 요청은 완료를 처리할 때(HandleCompletion) 이미 돌려줬다 — 진행 중 일이 남은
    // 작업은 지우지 않는다.
    void DestroyJob(LoaderPackageJob* job) noexcept {
        --packagesInFlight;
        tempBytesInFlight -= job->tempBytes;
        Allocator& owner = allocator;
        job->~LoaderPackageJob();
        (void)owner.Deallocate(job);
    }

    void RemoveFromLists(uint32 unit) noexcept {
        RemoveValue(admitCandidates, unit);
        RemoveValue(readyUnits, unit);
    }

    void ProcessRetire() noexcept {
        while (retireHead != LoaderNoIndex) {
            const uint32 unit = retireHead;
            retireHead = units[unit].nextRetire;
            if (retireHead == LoaderNoIndex) {
                retireTail = LoaderNoIndex;
            }

            LoaderUnit& target = units[unit];
            const LoaderRetire kind = target.retire;
            target.retire = LoaderRetire::None;
            target.nextRetire = LoaderNoIndex;
            RemoveFromLists(unit);
            if (kind == LoaderRetire::Fail) {
                const AssetId failedAsset = EntryAsset(target.failedEntry);
                for (uint32 index : target.requests) {
                    LoaderRequest& request = requests[index];
                    if (request.entry == target.failedEntry) {
                        FinishRequest(index, AssetLoadState::Failed, target.failError,
                                      target.failDiagnostic, ObjectHandle{});
                        continue;
                    }

                    AssetDiagnostic dependency;
                    dependency.code = AssetDiagnosticCode::MissingDependency;
                    dependency.error = ErrorCode::DependencyFailed;
                    dependency.asset = EntryAsset(request.entry);
                    dependency.related = failedAsset;
                    FinishRequest(index, AssetLoadState::Failed, ErrorCode::DependencyFailed,
                                  dependency, ObjectHandle{});
                }

                // 기대는 단위는 같은 원인으로 실패한다(요청은 DependencyFailed).
                for (uint32 other : target.dependents) {
                    FailUnit(other, target.failedEntry, target.failError, target.failDiagnostic);
                }
            }

            target.requests.Clear();
            target.phase = kind == LoaderRetire::Fail ? AssetLoaderUnitPhase::Failed
                                                      : AssetLoaderUnitPhase::Abandoned;
            for (uint32 other : target.dependencies) {
                RemoveValue(units[other].dependents, unit);
                DropInterest(other);
            }

            for (uint32 other : target.dependents) {
                RemoveValue(units[other].dependencies, unit);
            }

            target.dependencies.Clear();
            target.dependents.Clear();
            for (uint32 member : target.members) {
                entryUnit[member] = LoaderNoIndex;
            }

            usize kept = 0;
            for (LoaderPackageJob* job : target.jobs) {
                if (job->asyncPending) {
                    platform::AtomicStore<platform::MemoryOrder::Release>(&job->abandoned, 1u);
                    target.jobs[kept++] = job;
                    continue;
                }

                DestroyJob(job);
            }

            (void)target.jobs.TryResize(kept);
            target.aliveJobs = static_cast<uint32>(kept);
            if (kept == 0) {
                FreeUnitSlot(unit);
            }
        }
    }

    // ---- 허가(11.6절 9) ----

    uint64 UnitBytes(const LoaderUnit& unit) const noexcept {
        uint64 bytes = 0;
        for (uint32 member : unit.members) {
            const CookedManifestEntry& entry = config.manifest->entries[member];
            bytes += entry.packageBytes + entry.bulkRawBytes;
        }

        return bytes;
    }

    void SetPhase(LoaderPackageJob& job, AssetLoaderPackagePhase phase) noexcept {
        const uint64 now = Now();
        job.phaseNanoseconds[static_cast<uint32>(job.phase)] += now - job.phaseStart;
        job.phase = phase;
        job.phaseStart = now;
    }

    Result<void> BeginPackage(uint32 unit, uint32 member, AssetDiagnostic& diagnostic) noexcept {
        LoaderUnit& target = units[unit];
        const CookedManifestEntry& entry = config.manifest->entries[member];
        diagnostic = AssetDiagnostic{};
        diagnostic.asset = entry.asset;
        auto memory = allocator.Allocate(sizeof(LoaderPackageJob), alignof(LoaderPackageJob));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        auto* job = InitializeAt<LoaderPackageJob>(memory.Value(), allocator);
        // jobs 용량은 허가 전에 구성 수만큼 잡았다.
        (void)target.jobs.TryPushBack(job);
        ++target.aliveJobs;
        ++packagesInFlight;
        job->tempBytes = entry.packageBytes + entry.bulkRawBytes;
        tempBytesInFlight += job->tempBytes;
        job->queue = &queue;
        job->manifest = config.manifest;
        job->entry = member;
        job->unit = unit;
        job->phaseStart = Now();
        PackageLoadOptions options = config.packageOptions;
        options.read.expectedKind = PackageKind::Cooked;
        options.maxBulkRawBytes = entry.bulkRawBytes;
        auto state = UniquePtr<PackageLoadState>::TryMake(allocator, allocator, *config.registry,
                                                          *config.types, access, options);
        if (!state) {
            return Result<void>::Failure(state.Error());
        }

        job->state = Move(state.Value());
        if (auto sized = job->file.TryResize(static_cast<usize>(entry.packageBytes)); !sized) {
            return sized;
        }

        ErrorCode built = ErrorCode::None;
        KeepFirst(built, job->path.TryAppend(root.View()));
        KeepFirst(built, job->path.TryAppendAscii('/'));
        KeepFirst(built, AppendCookedPackagePath(job->path, entry.asset, entry.packageSha256));
        if (built != ErrorCode::None) {
            return Result<void>::Failure(built);
        }

        CookedPackageRead& read = job->read;
        read.path = job->path.CString();
        read.pathBytes = job->path.View().ByteCount();
        read.expectedBytes = entry.packageBytes;
        read.destination = job->file.Data();
        read.complete = OnLoaderReadComplete;
        read.owner = job;
        read.backendState = nullptr;
        SetPhase(*job, AssetLoaderPackagePhase::Reading);
        // complete가 Begin 안에서 불릴 수 있으므로 진행 표시를 먼저 한다.
        job->asyncPending = true;
        job->readBegun = true;
        ++asyncInFlight;
        if (auto begun = config.io->Begin(read); !begun) {
            job->asyncPending = false;
            job->readBegun = false;
            --asyncInFlight;
            return begun;
        }

        return Result<void>::Success();
    }

    void AdmitUnit(uint32 unit) noexcept {
        LoaderUnit& target = units[unit];
        target.phase = AssetLoaderUnitPhase::Loading;
        RemoveValue(admitCandidates, unit);
        ++admissions;
        // 기대는 단위는 모두 이 단위가 Queued일 때 생겼다(허가 전 단위만 미허가 의존으로 센다) —
        // 그래서
        // 이 단위를 세고 있고, 의존보다 먼저 허가될 수 없어 아직 Queued다.
        for (uint32 other : target.dependents) {
            if (--units[other].unadmittedDependencies == 0) {
                (void)admitCandidates.TryPushBack(other);
            }
        }

        AssetDiagnostic diagnostic;
        if (auto reserved = target.jobs.TryReserve(target.members.Size()); !reserved) {
            diagnostic.asset = EntryAsset(target.members[0]);
            diagnostic.error = reserved.Error();
            FailUnit(unit, target.members[0], reserved.Error(), diagnostic);
            return;
        }

        for (uint32 member : target.members) {
            if (auto begun = BeginPackage(unit, member, diagnostic); !begun) {
                diagnostic.error = begun.Error();
                FailUnit(unit, member, begun.Error(), diagnostic);
                return;
            }
        }
    }

    uint32 Admit() noexcept {
        uint32 admitted = 0;
        while (!admitCandidates.IsEmpty()) {
            uint32 best = admitCandidates[0];
            for (uint32 candidate : admitCandidates) {
                if (Before(candidate, best)) {
                    best = candidate;
                }
            }

            const LoaderUnit& unit = units[best];
            const auto count = static_cast<uint32>(unit.members.Size());
            const uint64 bytes = UnitBytes(unit);
            const bool overCount = packagesInFlight + count > config.limits.maxPackagesInFlight;
            const bool overBytes = tempBytesInFlight + bytes > config.limits.maxTempBytes;
            if (packagesInFlight > 0 && (overCount || overBytes)) {
                break;
            }

            AdmitUnit(best);
            ProcessRetire();
            ++admitted;
        }

        return admitted;
    }

    // ---- 완료 처리 ----

    void MarkReadyIfDone(uint32 unit) noexcept {
        LoaderUnit& target = units[unit];
        if (target.phase == AssetLoaderUnitPhase::Loading && target.waitingDependencies == 0 &&
            target.preparedPackages == target.members.Size()) {
            target.phase = AssetLoaderUnitPhase::Ready;
            (void)readyUnits.TryPushBack(unit);
        }
    }

    // 작업 job을 단위 목록에서 빼고 지운다(버린 단위의 진행 중 작업이 끝났을 때).
    void DrainRetiredJob(LoaderPackageJob* job) noexcept {
        const uint32 unit = job->unit;
        LoaderUnit& target = units[unit];
        // 버린 단위의 jobs에는 진행 중이던 작업만 남아 있다.
        (void)target.jobs.TryRemoveAtSwap(target.jobs.Find(job));
        DestroyJob(job);
        if (--target.aliveJobs == 0) {
            FreeUnitSlot(unit);
        }
    }

    void HandleCompletion(LoaderPackageJob* job) noexcept {
        job->asyncPending = false;
        --asyncInFlight;
        const LoaderCompletion completion = job->completion;
        // IO 완료만 readBegun이 남아 있다(worker 완료 때는 이미 놓았다).
        if (job->readBegun) {
            config.io->Release(job->read);
            job->readBegun = false;
        }

        const uint32 unit = job->unit;
        const AssetLoaderUnitPhase phase = units[unit].phase;
        if (phase == AssetLoaderUnitPhase::Failed || phase == AssetLoaderUnitPhase::Abandoned) {
            DrainRetiredJob(job);
            return;
        }

        const CookedManifestEntry& entry = config.manifest->entries[job->entry];
        AssetDiagnostic diagnostic;
        diagnostic.asset = entry.asset;
        if (completion == LoaderCompletion::Io) {
            const CookedPackageReadResult result = job->ioResult;
            if (result.error != ErrorCode::None) {
                diagnostic.error = result.error;
                diagnostic.code = result.error == ErrorCode::NotFound
                                      ? AssetDiagnosticCode::MissingFile
                                      : AssetDiagnosticCode::None;
                FailUnit(unit, job->entry, result.error, diagnostic);
                return;
            }

            if (result.fileBytes != entry.packageBytes) {
                diagnostic.code = AssetDiagnosticCode::CorruptPackage;
                diagnostic.error = ErrorCode::InvalidArgument;
                diagnostic.section = static_cast<uint32>(CookedManifestField::PackageBytes);
                diagnostic.detail = CookedManifestNoEntry;
                FailUnit(unit, job->entry, ErrorCode::InvalidArgument, diagnostic);
                return;
            }

            SetPhase(*job, AssetLoaderPackagePhase::Preparing);
            job->asyncPending = true;
            ++asyncInFlight;
            if (auto run = config.worker->Run(RunLoaderPrepare, job); !run) {
                job->asyncPending = false;
                --asyncInFlight;
                diagnostic.error = run.Error();
                FailUnit(unit, job->entry, run.Error(), diagnostic);
            }

            return;
        }

        if (job->workerError != ErrorCode::None) {
            FailUnit(unit, job->entry, job->workerError, job->workerDiagnostic);
            return;
        }

        SetPhase(*job, AssetLoaderPackagePhase::Prepared);
        ++units[unit].preparedPackages;
        MarkReadyIfDone(unit);
    }

    uint32 DrainCompletions() noexcept {
        queue.mutex.Lock();
        LoaderPackageJob* head = queue.head;
        queue.head = nullptr;
        queue.tail = nullptr;
        queue.mutex.Unlock();
        uint32 count = 0;
        while (head != nullptr) {
            LoaderPackageJob* job = head;
            head = head->nextCompletion;
            HandleCompletion(job);
            ProcessRetire();
            ++count;
        }

        return count;
    }

    void CheckCancellations() noexcept {
        for (usize index = 0; index < requests.Size(); ++index) {
            LoaderRequest& request = requests[index];
            if (!request.used || request.state != AssetLoadState::Pending ||
                !request.cancel.IsCancellationRequested()) {
                continue;
            }

            CancelRequest(static_cast<uint32>(index), ErrorCode::Cancelled);
        }

        ProcessRetire();
    }

    // Pending 요청을 끝내고 관심을 놓는다(정리는 호출자가 ProcessRetire로).
    void CancelRequest(uint32 index, ErrorCode error) noexcept {
        const uint32 unit = requests[index].unit;
        AssetDiagnostic diagnostic;
        diagnostic.error = error;
        diagnostic.asset = EntryAsset(requests[index].entry);
        FinishRequest(index,
                      error == ErrorCode::Cancelled ? AssetLoadState::Cancelled
                                                    : AssetLoadState::Failed,
                      error, diagnostic, ObjectHandle{});
        // Pending 요청은 언제나 단위에 붙어 있다.
        RemoveValue(units[unit].requests, index);
        DropInterest(unit);
    }

    // ---- 공개(11.6절 10) ----

    uint32 PickReady() const noexcept {
        uint32 best = readyUnits[0];
        for (uint32 candidate : readyUnits) {
            if (Before(candidate, best)) {
                best = candidate;
            }
        }

        return best;
    }

    // 실패 진단의 asset이 구성 패키지면 그 항목, 아니면 첫 구성 항목이다.
    uint32 FailedMember(const LoaderUnit& unit, AssetId asset) const noexcept {
        uint32 failed = unit.members[0];
        for (uint32 member : unit.members) {
            failed = EntryAsset(member) == asset ? member : failed;
        }

        return failed;
    }

    void InstantiateUnit(uint32 unit) noexcept {
        RemoveValue(readyUnits, unit);
        const usize count = units[unit].members.Size();
        Array<PackageLoadState*> states(allocator);
        Array<ObjectHandle> handles(allocator);
        Array<RootRef<Object>> roots(allocator);
        AssetDiagnostic diagnostic;
        diagnostic.asset = EntryAsset(units[unit].members[0]);
        ErrorCode step = ErrorCode::None;
        KeepFirst(step, states.TryReserve(count));
        KeepFirst(step, handles.TryResize(count));
        KeepFirst(step, roots.TryResize(count));
        KeepFirst(step, loadedRoots.TryReserve(loadedRoots.Size() + count));
        KeepFirst(step, loadedHandles.TryReserve(loadedHandles.Size() + count));
        if (step != ErrorCode::None) {
            diagnostic.error = step;
            FailUnit(unit, units[unit].members[0], step, diagnostic);
            return;
        }

        for (LoaderPackageJob* job : units[unit].jobs) {
            (void)states.TryPushBack(job->state.Get());
        }

        {
            ConstructionContext transaction(*config.registry, access);
            AssetDiagnostic made;
            if (auto instantiated = InstantiatePackages(
                    transaction, Span<PackageLoadState* const>(states.Data(), states.Size()),
                    &owner, handles.View(), &made);
                !instantiated) {
                FailUnit(unit, FailedMember(units[unit], made.asset), instantiated.Error(), made);
                return;
            }

            if (auto committed = transaction.Commit(
                    Span<const ObjectHandle>(handles.Data(), handles.Size()), roots.View());
                !committed) {
                diagnostic.error = committed.Error();
                FailUnit(unit, units[unit].members[0], committed.Error(), diagnostic);
                return;
            }
        }

        LoaderUnit& target = units[unit];
        // jobs와 members는 같은 순서다(허가 때 members 순서로 만들었다).
        for (usize index = 0; index < count; ++index) {
            const uint32 member = target.jobs[index]->entry;
            entryLoaded[member] = static_cast<uint32>(loadedRoots.Size());
            entryUnit[member] = LoaderNoIndex;
            (void)loadedRoots.TryPushBack(Move(roots[index]));
            (void)loadedHandles.TryPushBack(handles[index]);
        }

        for (uint32 index : target.requests) {
            FinishRequest(index, AssetLoadState::Succeeded, ErrorCode::None, AssetDiagnostic{},
                          loadedHandles[entryLoaded[requests[index].entry]]);
        }

        target.requests.Clear();
        for (uint32 other : target.dependents) {
            LoaderUnit& dependent = units[other];
            RemoveValue(dependent.dependencies, unit);
            --dependent.waitingDependencies;
            MarkReadyIfDone(other);
        }

        for (LoaderPackageJob* job : target.jobs) {
            DestroyJob(job);
        }

        target.jobs.Clear();
        FreeUnitSlot(unit);
    }

    // ---- Pump ----

    AssetLoaderPumpResult PumpOnce(const AssetLoaderPumpBudget& budget) noexcept {
        AssetLoaderPumpResult result;
        result.completions = DrainCompletions();
        CheckCancellations();
        result.admittedUnits = Admit();
        // 시계는 공개 구간에서만 읽는다(단위 사이 검사 — 11.6절 10).
        if (readyUnits.IsEmpty()) {
            return result;
        }

        const uint64 start = Now();
        for (;;) {
            InstantiateUnit(PickReady());
            ProcessRetire();
            ++result.instantiatedUnits;
            if (readyUnits.IsEmpty()) {
                break;
            }

            if (result.instantiatedUnits >= budget.maxUnits ||
                Now() - start >= budget.maxNanoseconds) {
                result.budgetExhausted = true;
                break;
            }
        }

        return result;
    }

    // ---- 종료 ----

    void ShutdownAll() noexcept {
        for (usize index = 0; index < requests.Size(); ++index) {
            if (requests[index].used && requests[index].state == AssetLoadState::Pending) {
                CancelRequest(static_cast<uint32>(index), ErrorCode::ShuttingDown);
            }
        }

        for (usize index = 0; index < units.Size(); ++index) {
            if (units[index].used && units[index].phase != AssetLoaderUnitPhase::Failed &&
                units[index].phase != AssetLoaderUnitPhase::Abandoned) {
                QueueRetire(static_cast<uint32>(index), LoaderRetire::Abandon);
            }
        }

        ProcessRetire();
        while (asyncInFlight > 0) {
            WaitCompletions();
            (void)DrainCompletions();
        }

        loadedRoots.Clear();
        loadedHandles.Clear();
        for (usize index = 0; index < entryLoaded.Size(); ++index) {
            entryLoaded[index] = LoaderNoIndex;
        }
    }

    void WaitCompletions() noexcept {
        queue.mutex.Lock();
        while (queue.head == nullptr && asyncInFlight > 0) {
            (void)queue.changed.Wait(queue.mutex);
        }

        queue.mutex.Unlock();
    }
};

Result<void> WorkerSchedulerLoaderWorker::Run(Job job, void* context) noexcept {
    auto body = Function<void(WorkerTaskContext&)>::TryCreate(
        allocator_, [job, context](WorkerTaskContext&) noexcept { job(context); });
    if (!body) {
        return Result<void>::Failure(body.Error());
    }

    auto submitted = scheduler_.Submit(Move(body.Value()));
    if (!submitted) {
        return Result<void>::Failure(submitted.Error());
    }

    return Result<void>::Success();
}

const char* LoadPriorityName(LoadPriority priority) noexcept {
    switch (priority) {
    case LoadPriority::Low:
        return "Low";
    case LoadPriority::Normal:
        return "Normal";
    case LoadPriority::High:
        return "High";
    case LoadPriority::Critical:
        return "Critical";
    }

    return "Unknown";
}

const char* AssetLoadStateName(AssetLoadState state) noexcept {
    switch (state) {
    case AssetLoadState::Pending:
        return "Pending";
    case AssetLoadState::Succeeded:
        return "Succeeded";
    case AssetLoadState::Failed:
        return "Failed";
    case AssetLoadState::Cancelled:
        return "Cancelled";
    }

    return "Unknown";
}

const char* AssetLoaderUnitPhaseName(AssetLoaderUnitPhase phase) noexcept {
    switch (phase) {
    case AssetLoaderUnitPhase::Queued:
        return "Queued";
    case AssetLoaderUnitPhase::Loading:
        return "Loading";
    case AssetLoaderUnitPhase::Ready:
        return "Ready";
    case AssetLoaderUnitPhase::Abandoned:
        return "Abandoned";
    case AssetLoaderUnitPhase::Failed:
        return "Failed";
    }

    return "Unknown";
}

const char* AssetLoaderPackagePhaseName(AssetLoaderPackagePhase phase) noexcept {
    switch (phase) {
    case AssetLoaderPackagePhase::Queued:
        return "Queued";
    case AssetLoaderPackagePhase::Reading:
        return "Reading";
    case AssetLoaderPackagePhase::Preparing:
        return "Preparing";
    case AssetLoaderPackagePhase::Prepared:
        return "Prepared";
    case AssetLoaderPackagePhase::Count:
        break;
    }

    return "Unknown";
}

const char* AssetLoaderWaitName(AssetLoaderWait wait) noexcept {
    switch (wait) {
    case AssetLoaderWait::None:
        return "None";
    case AssetLoaderWait::Admission:
        return "Admission";
    case AssetLoaderWait::DependencyAdmission:
        return "DependencyAdmission";
    case AssetLoaderWait::Dependency:
        return "Dependency";
    case AssetLoaderWait::Io:
        return "Io";
    case AssetLoaderWait::Worker:
        return "Worker";
    case AssetLoaderWait::Pump:
        return "Pump";
    }

    return "Unknown";
}

AssetLoader::AssetLoader(const AssetLoaderConfig& config) noexcept : config_(config) {
}

AssetLoader::~AssetLoader() {
    if (impl_ == nullptr) {
        return;
    }

    (void)Shutdown();
    Allocator& allocator = *config_.allocator;
    impl_->~Impl();
    (void)allocator.Deallocate(impl_);
    impl_ = nullptr;
}

Result<void> AssetLoader::Initialize(AssetDiagnostic* failure) noexcept {
    const AssetLoaderConfig& config = config_;
    auto invalid = [failure]() noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = ErrorCode::InvalidArgument;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    };
    if (impl_ != nullptr || config.allocator == nullptr || config.registry == nullptr ||
        config.types == nullptr || config.manifest == nullptr || config.io == nullptr ||
        config.worker == nullptr || config.limits.maxPackagesInFlight == 0 ||
        config.limits.maxTempBytes == 0 || config.limits.agingAdmissions == 0 ||
        !config.registry->UsesTypeRegistry(*config.types)) {
        return invalid();
    }

    auto memory = config.allocator->Allocate(sizeof(Impl), alignof(Impl));
    if (!memory) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = memory.Error();
        }

        return Result<void>::Failure(memory.Error());
    }

    impl_ = InitializeAt<Impl>(memory.Value(), *this, config_);
    impl_->ownerThread = platform::CurrentThreadId();
    if (auto initialized = impl_->Initialize(failure); !initialized) {
        Allocator& allocator = *config_.allocator;
        impl_->~Impl();
        (void)allocator.Deallocate(impl_);
        impl_ = nullptr;
        return initialized;
    }

    return Result<void>::Success();
}

Result<AssetLoadHandle> AssetLoader::Request(const AssetLoadRequest& request,
                                             AssetDiagnostic* failure) noexcept {
    using Out = AssetLoadHandle;
    auto fail = [failure](ErrorCode error, AssetDiagnosticCode code, AssetId asset) noexcept {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = code;
            failure->error = error;
            failure->asset = asset;
        }

        return Result<Out>::Failure(error);
    };
    if (impl_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, AssetDiagnosticCode::None, request.id);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return fail(ErrorCode::WrongThread, AssetDiagnosticCode::None, request.id);
    }

    if (impl.shutDown) {
        return fail(ErrorCode::ShuttingDown, AssetDiagnosticCode::None, request.id);
    }

    if (!ValidPriority(request.priority)) {
        return fail(ErrorCode::InvalidArgument, AssetDiagnosticCode::None, request.id);
    }

    const CookedManifestEntry* found = config_.manifest->Find(request.id);
    if (found == nullptr) {
        return fail(ErrorCode::NotFound, AssetDiagnosticCode::MissingDependency, request.id);
    }

    const uint32 entry = impl.EntryIndex(found);
    // 요청 칸을 먼저 잡는다(되돌림이 쉽다).
    uint32 index = impl.freeRequest;
    if (index == LoaderNoIndex) {
        if (auto added = impl.requests.TryEmplaceBack(); !added) {
            return fail(added.Error(), AssetDiagnosticCode::None, request.id);
        }

        index = static_cast<uint32>(impl.requests.Size() - 1);
    } else {
        impl.freeRequest = impl.requests[index].nextFree;
    }

    auto release = [&impl, index]() noexcept {
        impl.requests[index].nextFree = impl.freeRequest;
        impl.freeRequest = index;
    };
    LoaderRequest& slot = impl.requests[index];
    slot.used = true;
    slot.entry = entry;
    slot.priority = request.priority;
    slot.state = AssetLoadState::Pending;
    slot.error = ErrorCode::None;
    slot.diagnostic = AssetDiagnostic{};
    slot.package = ObjectHandle{};
    slot.unit = LoaderNoIndex;
    const AssetLoadHandle handle{index, slot.generation};
    if (impl.entryLoaded[entry] != LoaderNoIndex) {
        slot.state = AssetLoadState::Succeeded;
        slot.package = impl.loadedHandles[impl.entryLoaded[entry]];
        return Result<Out>::Success(handle);
    }

    uint32 unit = impl.entryUnit[entry];
    if (unit == LoaderNoIndex) {
        auto resolved = impl.Resolve(entry);
        if (!resolved) {
            impl.requests[index].used = false;
            release();
            return fail(resolved.Error(), AssetDiagnosticCode::None, request.id);
        }

        unit = resolved.Value();
    } else {
        ErrorCode reserved = ErrorCode::None;
        KeepFirst(reserved,
                  impl.units[unit].requests.TryReserve(impl.units[unit].requests.Size() + 1));
        KeepFirst(reserved, impl.workStack.TryReserve(impl.units.Size()));
        if (reserved != ErrorCode::None) {
            impl.requests[index].used = false;
            release();
            return fail(reserved, AssetDiagnosticCode::None, request.id);
        }
    }

    LoaderRequest& pending = impl.requests[index];
    pending.unit = unit;
    pending.cancel = request.cancel;
    (void)impl.units[unit].requests.TryPushBack(index);
    ++impl.units[unit].interest;
    impl.Boost(unit, request.priority);
    return Result<Out>::Success(handle);
}

Result<AssetId> AssetLoader::FindByPath(StringView path) const noexcept {
    if (impl_ == nullptr) {
        return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetId>::Failure(ErrorCode::WrongThread);
    }

    StringBuilder<LoaderKeyInlineBytes> key(impl.allocator);
    if (auto made = package_detail::CheckCookedPathHint(path, impl.allocator, key); !made) {
        return Result<AssetId>::Failure(made.Error());
    }

    const StringView wanted = key.View();
    usize low = 0;
    usize high = impl.keyOrder.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (package_detail::Utf8ByteLess(impl.KeyOf(impl.keyOrder[middle]), wanted)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low == impl.keyOrder.Size() || !(impl.KeyOf(impl.keyOrder[low]) == wanted)) {
        return Result<AssetId>::Failure(ErrorCode::NotFound);
    }

    return Result<AssetId>::Success(impl.EntryAsset(impl.keyOrder[low]));
}

Result<AssetLoadStatus> AssetLoader::Status(AssetLoadHandle handle) const noexcept {
    if (impl_ == nullptr) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::WrongThread);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<AssetLoadStatus>::Failure(ErrorCode::StaleHandle);
    }

    const LoaderRequest& request = impl.requests[handle.index];
    AssetLoadStatus status;
    status.state = request.state;
    status.error = request.error;
    status.diagnostic = request.diagnostic;
    status.package = request.package;
    return Result<AssetLoadStatus>::Success(status);
}

Result<void> AssetLoader::SetPriority(AssetLoadHandle handle, LoadPriority priority) noexcept {
    if (impl_ == nullptr || !ValidPriority(priority)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    LoaderRequest& request = impl.requests[handle.index];
    if (request.state != AssetLoadState::Pending) {
        return Result<void>::Success();
    }

    request.priority = priority;
    // Boost의 작업 목록 용량은 요청·해석 때 단위 표 크기 이상으로 잡았다.
    impl.Boost(request.unit, priority);
    return Result<void>::Success();
}

Result<void> AssetLoader::Release(AssetLoadHandle handle) noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<void>::Failure(ErrorCode::StaleHandle);
    }

    if (impl.requests[handle.index].state == AssetLoadState::Pending) {
        impl.CancelRequest(handle.index, ErrorCode::Cancelled);
        impl.ProcessRetire();
    }

    impl.FreeRequest(handle.index);
    return Result<void>::Success();
}

Result<AssetLoaderPumpResult> AssetLoader::Pump(const AssetLoaderPumpBudget& budget) noexcept {
    using Out = AssetLoaderPumpResult;
    if (impl_ == nullptr) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<Out>::Failure(ErrorCode::WrongThread);
    }

    if (impl.shutDown) {
        return Result<Out>::Failure(ErrorCode::ShuttingDown);
    }

    return Result<Out>::Success(impl.PumpOnce(budget));
}

Result<void> AssetLoader::WaitForCompletions() noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!impl_->OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    impl_->WaitCompletions();
    return Result<void>::Success();
}

Result<AssetLoadStatus> AssetLoader::WaitFor(AssetLoadHandle handle, const char* site) noexcept {
    using Out = AssetLoadStatus;
    if (impl_ == nullptr) {
        return Result<Out>::Failure(ErrorCode::InvalidArgument);
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<Out>::Failure(ErrorCode::WrongThread);
    }

    if (config_.inFrame != nullptr && config_.inFrame(config_.inFrameContext)) {
        return Result<Out>::Failure(ErrorCode::WrongContext);
    }

    if (!impl.ValidHandle(handle)) {
        return Result<Out>::Failure(ErrorCode::StaleHandle);
    }

    if (impl.shutDown) {
        return Result<Out>::Failure(ErrorCode::ShuttingDown);
    }

    const uint64 start = impl.Now();
    while (impl.requests[handle.index].state == AssetLoadState::Pending) {
        (void)impl.PumpOnce(AssetLoaderPumpBudget{});
        if (impl.requests[handle.index].state != AssetLoadState::Pending) {
            break;
        }

        impl.WaitCompletions();
    }

    const uint64 elapsed = impl.Now() - start;
    ++impl.syncWaits;
    impl.syncWaitNanoseconds += elapsed;
    if (config_.observer != nullptr) {
        AssetLoaderSyncWait wait;
        wait.site = site;
        wait.asset = impl.EntryAsset(impl.requests[handle.index].entry);
        wait.nanoseconds = elapsed;
        config_.observer->OnSyncWait(wait);
    }

    return Status(handle);
}

Result<void> AssetLoader::Describe(AssetLoaderSnapshot& out) const noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    out.requests.Clear();
    out.units.Clear();
    out.packages.Clear();
    const uint64 now = impl.Now();
    for (usize index = 0; index < impl.requests.Size(); ++index) {
        const LoaderRequest& request = impl.requests[index];
        if (!request.used) {
            continue;
        }

        AssetLoaderRequestInfo info;
        info.handle = AssetLoadHandle{static_cast<uint32>(index), request.generation};
        info.asset = impl.EntryAsset(request.entry);
        info.priority = request.priority;
        info.state = request.state;
        info.error = request.error;
        info.diagnostic = request.diagnostic;
        info.unit = request.unit;
        if (auto pushed = out.requests.TryPushBack(info); !pushed) {
            return pushed;
        }
    }

    for (usize index = 0; index < impl.units.Size(); ++index) {
        const LoaderUnit& unit = impl.units[index];
        if (!unit.used) {
            continue;
        }

        AssetLoaderUnitInfo info;
        info.unit = static_cast<uint32>(index);
        info.asset = impl.EntryAsset(unit.members[0]);
        info.phase = unit.phase;
        info.priority = unit.priority;
        info.effectivePriority = impl.Effective(unit);
        info.packages = static_cast<uint32>(unit.members.Size());
        info.preparedPackages = unit.preparedPackages;
        info.interest = unit.interest;
        for (const LoaderPackageJob* job : unit.jobs) {
            info.tempBytes += job->tempBytes;
        }

        if (unit.phase == AssetLoaderUnitPhase::Queued) {
            info.waiting = AssetLoaderWait::Admission;
            for (uint32 other : unit.dependencies) {
                if (impl.units[other].phase == AssetLoaderUnitPhase::Queued) {
                    info.waiting = AssetLoaderWait::DependencyAdmission;
                    info.waitingOn = impl.EntryAsset(impl.units[other].members[0]);
                    break;
                }
            }
        } else if (unit.phase == AssetLoaderUnitPhase::Ready) {
            info.waiting = AssetLoaderWait::Pump;
        } else if (unit.phase == AssetLoaderUnitPhase::Loading) {
            for (const LoaderPackageJob* job : unit.jobs) {
                if (job->phase == AssetLoaderPackagePhase::Reading) {
                    info.waiting = AssetLoaderWait::Io;
                    break;
                }

                if (job->phase == AssetLoaderPackagePhase::Preparing) {
                    info.waiting = AssetLoaderWait::Worker;
                }
            }

            // IO·worker에 없는 Loading 단위는 모든 패키지가 Prepared이고 공개 안 된 의존이
            // 있다(아니면
            // Ready다).
            if (info.waiting == AssetLoaderWait::None) {
                info.waiting = AssetLoaderWait::Dependency;
                info.waitingOn = impl.EntryAsset(impl.units[unit.dependencies[0]].members[0]);
            }
        }

        if (auto pushed = out.units.TryPushBack(info); !pushed) {
            return pushed;
        }

        for (const LoaderPackageJob* job : unit.jobs) {
            AssetLoaderPackageInfo package;
            package.asset = impl.EntryAsset(job->entry);
            package.unit = static_cast<uint32>(index);
            package.phase = job->phase;
            package.tempBytes = job->tempBytes;
            for (uint32 phase = 0; phase < AssetLoaderPackagePhaseCount; ++phase) {
                package.phaseNanoseconds[phase] = job->phaseNanoseconds[phase];
            }

            package.phaseNanoseconds[static_cast<uint32>(job->phase)] += now - job->phaseStart;
            if (auto pushed = out.packages.TryPushBack(package); !pushed) {
                return pushed;
            }
        }
    }

    out.loadedPackages = static_cast<uint32>(impl.loadedRoots.Size());
    out.packagesInFlight = impl.packagesInFlight;
    out.tempBytesInFlight = impl.tempBytesInFlight;
    out.syncWaits = impl.syncWaits;
    out.syncWaitNanoseconds = impl.syncWaitNanoseconds;
    return Result<void>::Success();
}

Result<ObjectHandle> AssetLoader::FindPackage(AssetId asset) noexcept {
    if (impl_ == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    const CookedManifestEntry* found = config_.manifest->Find(asset);
    if (found == nullptr) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    const uint32 loaded = impl_->entryLoaded[impl_->EntryIndex(found)];
    if (loaded == LoaderNoIndex) {
        return Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    }

    return Result<ObjectHandle>::Success(impl_->loadedHandles[loaded]);
}

Result<void> AssetLoader::Shutdown() noexcept {
    if (impl_ == nullptr) {
        return Result<void>::Success();
    }

    Impl& impl = *impl_;
    if (!impl.OnOwnerThread()) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    if (impl.shutDown) {
        return Result<void>::Success();
    }

    impl.shutDown = true;
    impl.ShutdownAll();
    return Result<void>::Success();
}

} // namespace rocks
