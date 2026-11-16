#include <Core/Memory/BinnedAllocator.h>
#include <Core/Memory/SystemAllocator.h>
#include <GcSoak/GcSoak.h>
#include <Object/ConstructionContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/HostFailure.h>

#include <stdio.h>

// ObjectRegistry GC 측정 도구다(Docs/Design/GarbageCollectionScale.ko.md GC-S0).
//
// 그래프: 256개 단위 Outer 묶음(머리 객체 아래 이름 있는 inner 255개)을 만들고, 노드마다 평균
// K개(0–2K 균등)의 무작위 간선을 ObjectRefArray에 넣는다. 루트는 묶음 머리 전부와 무작위 객체
// 1000개다. 첫 Collect가 도달하지 못한 객체(약 e^-K 비율)를 회수한 뒤부터 잰다.
//
// 측정 항목(시나리오마다):
// - 생성(묶음 생성·Commit)과 연결(AddReference) 시간
// - 정지형 Collect: Begin(루트 수집)·표식·sweep·회수·전체(반복 측정의 중앙값)
// - 증분 수집: 기본 GcBudget, 시간 한도만 둔 예산(2ms). Step 횟수와 Step 한 번의 시간 분포
// - scheduler 경로(worker N개, 예산 무제한)
// - churn: 살아 있는 객체의 10%만큼 쓰레기 묶음을 만든 뒤 Collect(회수 시간)
// - 메모리: 범주별(Slot chunk·객체·참조 배열·이름·기타) 바이트와 최고치
//
// 사용:
//   GcBench                          기본 표(100K·1M × 간선 4·8, 1M 잎 50%)
//   GcBench --objects 100000 --refs 4 [--leaf 50] [--reps 5] [--workers 8]
//   GcBench --allocators             할당자 비교(작은 그래프의 churn 회수, platform·System·Binned)
//   GcBench --soak 200000 [--seed 7] [--soak-objects 4000] [--soak-workers --workers 4]
//                  [--soak-time-budget]
//                                    장기 tick 시뮬레이터(GcSoak)를 돌리고 불변식 위반을 보고한다
namespace {

using namespace rocks;

constexpr uint32 GroupSize = 256;
constexpr uint32 RandomRoots = 1000;
constexpr TypeId NodeTypeId{0x4763424e4f444531ULL, 1};
constexpr TypeId LeafTypeId{0x4763424c45414631ULL, 1};

// 할당 범주다. ObjectRuntime의 할당 태그(ObjectRegistryCreate.cpp·ObjectRegistryArrayEdges.cpp)로
// 나눈다.
enum class Category : uint32 {
    SlotChunk,
    ObjectStorage,
    RefArray,
    Names,
    Other,
    Count
};

Category CategoryOf(uint32 tag) noexcept {
    switch (tag) {
    case 0x43484e4bu: // 'CHNK'
    case 0x43484e54u: // 'CHNT'
        return Category::SlotChunk;
    case 0x4f424a45u: // 'OBJE'
        return Category::ObjectStorage;
    case 0x4f524146u: // 'ORAF'
        return Category::RefArray;
    case 0x4e414d54u: // 'NAMT'
    case 0x4e414d52u: // 'NAMR'
        return Category::Names;
    default:
        return Category::Other;
    }
}

// 플랫폼 정렬 할당을 바로 쓰는 측정용 allocator다. 해제가 O(1)이라 할당자 비용을 GC 비용에서 떼어
// 낸다. 각 할당 앞 16 byte(또는 정렬 크기) 머리에 크기·범주를 두고 범주별 바이트와 전체 최고치를
// atomic으로 센다(worker의 capture 정리가 다른 스레드에서 반환한다).
class BenchAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (bytes == 0) {
            return Result<void*>::Success(nullptr);
        }

        const usize prefix = alignment > sizeof(Header) ? alignment : sizeof(Header);
        void* base = platform::AlignedAllocate(bytes + prefix, prefix);
        if (base == nullptr) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        uint8* user = static_cast<uint8*>(base) + prefix;
        Header* header = reinterpret_cast<Header*>(user) - 1;
        header->bytes = bytes;
        header->prefix = static_cast<uint32>(prefix);
        header->category = static_cast<uint32>(CategoryOf(tag));
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&live_[header->category], bytes);
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations_, 1);
        const uint64 total =
            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&total_, bytes) + bytes;
        uint64 peak = platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&peak_);
        while (total > peak && !platform::AtomicCompareExchangeWeak<platform::MemoryOrder::Relaxed,
                                                                    platform::MemoryOrder::Relaxed>(
                                   &peak_, peak, total)) {
        }

        return Result<void*>::Success(user);
    }

    Result<void> Deallocate(void* address) noexcept override {
        if (address == nullptr) {
            return Result<void>::Success();
        }

        uint8* user = static_cast<uint8*>(address);
        Header* header = reinterpret_cast<Header*>(user) - 1;
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&live_[header->category],
                                                                 header->bytes);
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&total_, header->bytes);
        platform::AtomicFetchSub<platform::MemoryOrder::Relaxed>(&allocations_, 1);
        platform::AlignedFree(user - header->prefix);
        return Result<void>::Success();
    }

    uint64 Live(Category category) const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(
            &live_[static_cast<uint32>(category)]);
    }

    uint64 Total() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&total_);
    }

    uint64 Peak() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&peak_);
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations_);
    }

    void ResetPeak() noexcept {
        platform::AtomicStore<platform::MemoryOrder::Relaxed>(&peak_, Total());
    }

private:
    struct Header {
        uint64 bytes;
        uint32 prefix;
        uint32 category;
    };

    static_assert(sizeof(Header) == 16, "BenchAllocator header must stay 16 bytes");

    uint64 live_[static_cast<uint32>(Category::Count)] = {};
    uint64 total_ = 0;
    uint64 peak_ = 0;
    uint64 allocations_ = 0;
};

// 간선을 가진 노드다. Trace는 부수 효과가 없어 병렬 trace를 허용한다(parallelTraceSafe).
class BenchNode final : public Object {
public:
    explicit BenchNode(Allocator& allocator) noexcept : refs(allocator) {
    }

    void Trace(TraceVisitor& visitor) const noexcept {
        refs.Trace(visitor);
    }

    ObjectRefArray<Object> refs;
};

// 강한 참조가 없는 잎 객체다.
class BenchLeaf final : public Object {
public:
    BenchLeaf() noexcept = default;

    uint64 payload[2] = {};
};

struct Rng {
    uint64 state;

    uint64 Next() noexcept {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dULL;
    }

    uint32 Below(uint32 bound) noexcept {
        return static_cast<uint32>(Next() % bound);
    }
};

double Milliseconds(uint64 nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) * 1e-6;
}

double Microseconds(uint64 nanoseconds) noexcept {
    return static_cast<double>(nanoseconds) * 1e-3;
}

// 정렬된 표본의 백분위 값이다(최근접 순위).
uint64 Percentile(const Array<uint64>& sorted, uint32 percent) noexcept {
    if (sorted.IsEmpty()) {
        return 0;
    }

    const usize rank = (sorted.Size() * percent + 99) / 100;
    return sorted[rank == 0 ? 0 : rank - 1];
}

uint64 Median(Array<uint64>& values) noexcept {
    values.Sort();
    return values.IsEmpty() ? 0 : values[values.Size() / 2];
}

// 이름 "n<번호>"를 buffer에 쓰고 그 view를 돌려준다.
StringView NameOf(char (&buffer)[16], uint32 number) noexcept {
    char digits[12];
    uint32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + number % 10);
        number /= 10;
    } while (number != 0);

    buffer[0] = 'n';
    for (uint32 index = 0; index < count; ++index) {
        buffer[1 + index] = digits[count - 1 - index];
    }

    return StringView(buffer, count + 1);
}

struct Options {
    uint32 objects = 0;
    uint32 refs = 4;
    uint32 leafPercent = 0;
    uint32 reps = 0;
    uint32 workers = 8;
    bool allocators = false;
    uint64 soakTicks = 0;
    uint64 seed = 7;
    uint32 soakObjects = 4000;
    bool soakWorkers = false;
    bool soakTimeBudget = false;
};

struct TypeTokens {
    TypeRegistrationToken node;
    TypeRegistrationToken leaf;
};

bool RegisterTypes(TypeRegistry& types, TypeTokens& tokens) noexcept {
    RuntimeTypeOps node = MakeRuntimeTypeOps<BenchNode>();
    node.parallelTraceSafe = true;
    auto registeredNode = types.Register(TypeSchema{NodeTypeId, 1, {}, {}}, Move(node));
    RuntimeTypeOps leaf = MakeRuntimeTypeOps<BenchLeaf>();
    leaf.parallelTraceSafe = true;
    auto registeredLeaf = types.Register(TypeSchema{LeafTypeId, 1, {}, {}}, Move(leaf));
    if (!registeredNode || !registeredLeaf) {
        return false;
    }

    tokens.node = registeredNode.Value();
    tokens.leaf = registeredLeaf.Value();
    return true;
}

// 그래프 생성기다. created에 만든 handle을, nodes에 그중 노드(간선 소유자)를 생성 순서로 붙이고,
// pins에 생성한 객체 전부의 RootRef를 붙인다(간선을 넣는 동안 살려 둔다).
struct GraphBuilder {
    ObjectRegistry& registry;
    Rng& rng;
    uint32 leafPercent = 0;

    bool CreateGroups(uint32 count, Array<ObjectHandle>& created, Array<ObjectHandle>& nodes,
                      Array<RootRef<Object>>& pins) noexcept {
        const auto access = registry.OwnerAccess();
        ObjectHandle group[GroupSize];
        RootRef<Object> outs[GroupSize];
        char name[16];
        for (uint32 done = 0; done < count;) {
            const uint32 size = count - done < GroupSize ? count - done : GroupSize;
            ConstructionContext construction(registry, access);
            auto head = construction.Create<BenchNode>(NodeTypeId);
            if (!head) {
                return false;
            }

            group[0] = head.Value();
            if (!nodes.TryPushBack(group[0])) {
                return false;
            }

            for (uint32 index = 1; index < size; ++index) {
                const bool leaf = leafPercent != 0 && rng.Below(100) < leafPercent;
                auto inner =
                    leaf
                        ? construction.Create<BenchLeaf>(LeafTypeId, group[0], NameOf(name, index))
                        : construction.Create<BenchNode>(NodeTypeId, group[0], NameOf(name, index));
                if (!inner) {
                    return false;
                }

                group[index] = inner.Value();
                if (!leaf && !nodes.TryPushBack(group[index])) {
                    return false;
                }
            }

            auto committed = construction.Commit(Span<const ObjectHandle>(group, size),
                                                 Span<RootRef<Object>>(outs, size));
            if (!committed) {
                return false;
            }

            for (uint32 index = 0; index < size; ++index) {
                if (!created.TryPushBack(group[index]) || !pins.TryPushBack(Move(outs[index]))) {
                    return false;
                }
            }

            done += size;
        }

        return true;
    }

    // owners의 노드마다 평균 refs개 간선을 targets에서 골라 넣는다.
    bool Link(const Array<ObjectHandle>& owners, const Array<ObjectHandle>& targets,
              uint32 refs) noexcept {
        const auto access = registry.OwnerAccess();
        const uint32 targetCount = static_cast<uint32>(targets.Size());
        for (usize index = 0; index < owners.Size(); ++index) {
            auto borrowed = registry.Resolve<BenchNode>(access, owners[index]);
            if (!borrowed) {
                return false;
            }

            auto pointer = borrowed.Value().Get();
            if (!pointer) {
                return false;
            }

            const uint32 degree = rng.Below(2 * refs + 1);
            for (uint32 edge = 0; edge < degree; ++edge) {
                const ObjectHandle target = targets[rng.Below(targetCount)];
                if (!registry.AddReference(access, owners[index], pointer.Value()->refs, target)) {
                    return false;
                }
            }
        }

        return true;
    }
};

// 한 시나리오의 GC 측정 결과다.
struct PhaseTimes {
    uint64 begin = 0;
    uint64 mark = 0;
    uint64 sweep = 0;
    uint64 reclaim = 0;
    uint64 total = 0;
};

PhaseTimes MedianOf(Array<GcReport>& reports, Allocator& allocator) noexcept {
    Array<uint64> values(allocator);
    PhaseTimes times;
    uint64 GcReport::* fields[] = {&GcReport::beginNanoseconds, &GcReport::markNanoseconds,
                                   &GcReport::sweepNanoseconds, &GcReport::reclaimNanoseconds,
                                   &GcReport::durationNanoseconds};
    uint64* outputs[] = {&times.begin, &times.mark, &times.sweep, &times.reclaim, &times.total};
    for (uint32 field = 0; field < 5; ++field) {
        values.Clear();
        for (usize index = 0; index < reports.Size(); ++index) {
            if (!values.TryPushBack(reports[index].*fields[field])) {
                return {};
            }
        }

        *outputs[field] = Median(values);
    }

    return times;
}

void PrintPhases(const char* label, const PhaseTimes& times) noexcept {
    printf("  %-28s begin %8.3f  mark %9.3f  sweep %8.3f  reclaim %8.3f  total %9.3f ms\n", label,
           Milliseconds(times.begin), Milliseconds(times.mark), Milliseconds(times.sweep),
           Milliseconds(times.reclaim), Milliseconds(times.total));
}

// 증분 cycle 하나를 끝까지 돌리며 Step마다 걸린 시간을 잰다.
bool RunIncremental(ObjectRegistry& registry, const char* label, GcBudget budget,
                    WorkerScheduler* scheduler, Allocator& allocator) noexcept {
    const auto access = registry.OwnerAccess();
    Array<uint64> steps(allocator);
    const uint64 started = platform::MonotonicNanoseconds();
    if (!registry.BeginCollection(access)) {
        printf("  %s: BeginCollection failed\n", label);
        return false;
    }

    const uint64 begun = platform::MonotonicNanoseconds();
    for (;;) {
        const uint64 before = platform::MonotonicNanoseconds();
        auto progress = registry.StepCollection(access, budget, scheduler);
        const uint64 after = platform::MonotonicNanoseconds();
        if (!progress) {
            printf("  %s: StepCollection failed (%u)\n", label,
                   static_cast<unsigned>(progress.Error()));
            return false;
        }

        if (!steps.TryPushBack(after - before)) {
            return false;
        }

        if (progress.Value().IsComplete()) {
            break;
        }
    }

    const uint64 finished = platform::MonotonicNanoseconds();
    const GcReport report = registry.LastGcReport();
    steps.Sort();
    printf("  %-28s steps %7llu  p50 %8.1f  p95 %8.1f  p99 %8.1f  max %9.1f us | Begin %7.3f  "
           "mark %9.3f  sweep %8.3f  reclaim %7.3f  cycle %9.3f ms (wall %9.3f)\n",
           label, static_cast<unsigned long long>(steps.Size()),
           Microseconds(Percentile(steps, 50)), Microseconds(Percentile(steps, 95)),
           Microseconds(Percentile(steps, 99)),
           Microseconds(steps.IsEmpty() ? 0 : steps[steps.Size() - 1]),
           Milliseconds(begun - started), Milliseconds(report.markNanoseconds),
           Milliseconds(report.sweepNanoseconds), Milliseconds(report.reclaimNanoseconds),
           Milliseconds(report.durationNanoseconds), Milliseconds(finished - started));
    return true;
}

void PrintMemory(const BenchAllocator& memory, uint64 objects, const ObjectStats& stats) noexcept {
    const uint64 slots = uint64(stats.chunks) * 64;
    const double perObject = objects == 0 ? 0.0 : 1.0 / static_cast<double>(objects);
    printf("  memory: slot chunks %.1f MiB (%.1f B/slot, %llu slots), objects %.1f MiB, ref "
           "arrays %.1f MiB, names %.1f MiB, other %.1f MiB, total %.1f MiB (%.1f B/object), "
           "peak %.1f MiB\n",
           static_cast<double>(memory.Live(Category::SlotChunk)) / 1048576.0,
           slots == 0
               ? 0.0
               : static_cast<double>(memory.Live(Category::SlotChunk)) / static_cast<double>(slots),
           static_cast<unsigned long long>(slots),
           static_cast<double>(memory.Live(Category::ObjectStorage)) / 1048576.0,
           static_cast<double>(memory.Live(Category::RefArray)) / 1048576.0,
           static_cast<double>(memory.Live(Category::Names)) / 1048576.0,
           static_cast<double>(memory.Live(Category::Other)) / 1048576.0,
           static_cast<double>(memory.Total()) / 1048576.0,
           static_cast<double>(memory.Total()) * perObject,
           static_cast<double>(memory.Peak()) / 1048576.0);
}

struct Scenario {
    uint32 objects;
    uint32 refs;
    uint32 leafPercent;
    uint32 reps;
};

// 시나리오 하나를 BenchAllocator로 측정한다. 실패하면 0이 아닌 종료 코드다.
int RunScenario(const Scenario& scenario, const Options& options) noexcept {
    printf("\n== objects %u, refs avg %u, leaf %u%%, reps %u ==\n", scenario.objects, scenario.refs,
           scenario.leafPercent, scenario.reps);
    fflush(stdout);
    BenchAllocator memory;
    int status = 0;
    {
        TypeRegistry types(memory);
        TypeTokens tokens;
        if (!RegisterTypes(types, tokens)) {
            printf("type registration failed\n");
            return 2;
        }

        RegistryOptions registryOptions;
        const uint64 wanted = uint64(scenario.objects) * 2 + 65536;
        registryOptions.maxSlots = wanted > NumericLimits<uint32>::Max() / 2
                                       ? NumericLimits<uint32>::Max() / 2
                                       : static_cast<uint32>(wanted);
        WorkerScheduler scheduler(memory);
        if (!scheduler.Start({options.workers, 256, 16})) {
            printf("scheduler start failed\n");
            return 2;
        }

        {
            ObjectRegistry registry(memory, types, Guid{0x6763, 1}, registryOptions);
            const auto access = registry.OwnerAccess();
            Rng rng{0x9e3779b97f4a7c15ULL ^ scenario.objects ^ (uint64(scenario.refs) << 32)};
            GraphBuilder builder{registry, rng, scenario.leafPercent};
            Array<ObjectHandle> all(memory);
            Array<ObjectHandle> nodes(memory);
            Array<RootRef<Object>> pins(memory);
            if (!all.TryReserve(scenario.objects) || !pins.TryReserve(scenario.objects)) {
                return 3;
            }

            uint64 started = platform::MonotonicNanoseconds();
            if (!builder.CreateGroups(scenario.objects, all, nodes, pins)) {
                printf("graph creation failed\n");
                return 3;
            }

            const uint64 created = platform::MonotonicNanoseconds();
            if (!builder.Link(nodes, all, scenario.refs)) {
                printf("graph linking failed\n");
                return 3;
            }

            const uint64 linked = platform::MonotonicNanoseconds();
            // 묶음 머리와 무작위 RandomRoots개만 루트로 남긴다.
            Array<uint8> keep(memory);
            if (!keep.TryResize(all.Size())) {
                return 3;
            }

            for (usize index = 0; index < all.Size(); index += GroupSize) {
                keep[index] = 1;
            }

            for (uint32 index = 0; index < RandomRoots; ++index) {
                keep[rng.Below(static_cast<uint32>(all.Size()))] = 1;
            }

            Array<RootRef<Object>> roots(memory);
            for (usize index = 0; index < pins.Size(); ++index) {
                if (keep[index] != 0) {
                    if (!roots.TryPushBack(Move(pins[index]))) {
                        return 3;
                    }
                } else {
                    pins[index].Reset();
                }
            }

            pins.Clear();
            printf("  create %.1f ms, link %.1f ms, roots %llu\n", Milliseconds(created - started),
                   Milliseconds(linked - created), static_cast<unsigned long long>(roots.Size()));
            PrintMemory(memory, all.Size(), registry.Stats());

            // 첫 Collect: 도달하지 못한 객체를 회수한다(쓰레기가 있는 cycle의 기준).
            auto first = registry.Collect(access);
            if (!first) {
                printf("first Collect failed\n");
                return 4;
            }

            printf("  first Collect: marked %u, retired %u, reclaimed %u, total %.3f ms (begin "
                   "%.3f mark %.3f sweep %.3f reclaim %.3f)\n",
                   first.Value().marked, first.Value().retired, first.Value().reclaimed,
                   Milliseconds(first.Value().durationNanoseconds),
                   Milliseconds(first.Value().beginNanoseconds),
                   Milliseconds(first.Value().markNanoseconds),
                   Milliseconds(first.Value().sweepNanoseconds),
                   Milliseconds(first.Value().reclaimNanoseconds));
            fflush(stdout);

            // 살아남은 handle만 남긴다(churn 간선의 대상).
            Array<ObjectHandle> survivors(memory);
            for (usize index = 0; index < all.Size(); ++index) {
                auto lifecycle = registry.InspectLifecycle(access, all[index]);
                if (lifecycle && lifecycle.Value() == ObjectLifecycle::Alive &&
                    !survivors.TryPushBack(all[index])) {
                    return 3;
                }
            }

            Array<GcReport> reports(memory);
            for (uint32 rep = 0; rep < scenario.reps; ++rep) {
                auto collected = registry.Collect(access);
                if (!collected || !reports.TryPushBack(collected.Value())) {
                    printf("Collect failed\n");
                    return 4;
                }
            }

            PrintPhases("Collect (median)", MedianOf(reports, memory));
            if (!RunIncremental(registry, "incremental default budget", GcBudget{}, nullptr,
                                memory) ||
                !RunIncremental(registry, "incremental time 2ms",
                                GcBudget{NumericLimits<uint32>::Max(), 2000000}, nullptr, memory) ||
                !RunIncremental(registry, "incremental time 0.5ms",
                                GcBudget{NumericLimits<uint32>::Max(), 500000}, nullptr, memory)) {
                return 5;
            }

            char label[64];
            snprintf(label, sizeof(label), "scheduler x%u unlimited", options.workers);
            if (!RunIncremental(registry, label, GcBudget{NumericLimits<uint32>::Max(), 0},
                                &scheduler, memory)) {
                return 5;
            }

            // churn: 살아 있는 객체의 10%만큼 쓰레기 묶음을 만들고(간선은 산 그래프로) 회수한다.
            reports.Clear();
            Array<uint64> createTimes(memory);
            const uint32 churn = static_cast<uint32>(survivors.Size() / 10);
            for (uint32 rep = 0; rep < scenario.reps; ++rep) {
                Array<ObjectHandle> garbage(memory);
                Array<ObjectHandle> garbageNodes(memory);
                Array<RootRef<Object>> garbagePins(memory);
                const uint64 before = platform::MonotonicNanoseconds();
                if (!builder.CreateGroups(churn, garbage, garbageNodes, garbagePins) ||
                    !builder.Link(garbageNodes, survivors, scenario.refs)) {
                    printf("churn creation failed\n");
                    return 6;
                }

                garbagePins.Clear();
                const uint64 after = platform::MonotonicNanoseconds();
                auto collected = registry.Collect(access);
                if (!collected || collected.Value().reclaimed != churn ||
                    !reports.TryPushBack(collected.Value()) ||
                    !createTimes.TryPushBack(after - before)) {
                    printf("churn Collect failed\n");
                    return 6;
                }
            }

            char churnLabel[64];
            snprintf(churnLabel, sizeof(churnLabel), "churn %u Collect (median)", churn);
            PrintPhases(churnLabel, MedianOf(reports, memory));
            printf("  churn create+link median %.3f ms, reclaim %.1f ns/object\n",
                   Milliseconds(Median(createTimes)),
                   static_cast<double>(MedianOf(reports, memory).reclaim) /
                       static_cast<double>(churn == 0 ? 1 : churn));
            PrintMemory(memory, survivors.Size(), registry.Stats());
            roots.Clear();
            fflush(stdout);
            const uint64 teardown = platform::MonotonicNanoseconds();
            if (!registry.Shutdown(access)) {
                printf("registry shutdown failed\n");
                status = 7;
            }

            printf("  shutdown (reclaim all) %.1f ms\n",
                   Milliseconds(platform::MonotonicNanoseconds() - teardown));
        }

        if (!scheduler.Shutdown()) {
            status = 7;
        }

        if (!types.Unregister(tokens.leaf) || !types.Unregister(tokens.node) || !types.Shutdown()) {
            status = 7;
        }
    }

    if (memory.Allocations() != 0) {
        printf("  leaked allocations: %llu\n",
               static_cast<unsigned long long>(memory.Allocations()));
        status = 8;
    }

    fflush(stdout);
    return status;
}

// 할당자별 churn 회수 비교다. SystemAllocator·BinnedAllocator는 해제 때 원장을 선형 검색하므로 큰
// 그래프에서는 끝나지 않는다(Docs/Design/GarbageCollectionScale.ko.md 3.2절 8번). 작은 그래프에서
// 잰다.
int RunAllocatorComparison(const Options& options) noexcept {
    constexpr uint32 LiveObjects = 20000;
    constexpr uint32 Rounds = 5;
    printf("\n== allocator comparison: live %u, refs avg %u, churn 10%% x %u ==\n", LiveObjects,
           options.refs, Rounds);
    for (uint32 kind = 0; kind < 3; ++kind) {
        BenchAllocator platformMemory;
        SystemAllocator system;
        BinnedAllocator binned(system);
        Allocator* memory = kind == 0   ? static_cast<Allocator*>(&platformMemory)
                            : kind == 1 ? static_cast<Allocator*>(&system)
                                        : static_cast<Allocator*>(&binned);
        const char* name = kind == 0 ? "platform" : kind == 1 ? "SystemAllocator" : "Binned";
        uint64 createTotal = 0;
        uint64 reclaimTotal = 0;
        uint64 teardownTime = 0;
        {
            TypeRegistry types(*memory);
            TypeTokens tokens;
            if (!RegisterTypes(types, tokens)) {
                return 2;
            }

            {
                ObjectRegistry registry(*memory, types, Guid{0x6764, kind});
                const auto access = registry.OwnerAccess();
                Rng rng{0x51afd7ed558ccd1ULL};
                GraphBuilder builder{registry, rng, 0};
                Array<ObjectHandle> all(*memory);
                Array<ObjectHandle> nodes(*memory);
                Array<RootRef<Object>> roots(*memory);
                if (!builder.CreateGroups(LiveObjects, all, nodes, roots) ||
                    !builder.Link(nodes, all, options.refs)) {
                    return 3;
                }

                for (uint32 round = 0; round < Rounds; ++round) {
                    Array<ObjectHandle> garbage(*memory);
                    Array<ObjectHandle> garbageNodes(*memory);
                    Array<RootRef<Object>> garbagePins(*memory);
                    const uint64 before = platform::MonotonicNanoseconds();
                    if (!builder.CreateGroups(LiveObjects / 10, garbage, garbageNodes,
                                              garbagePins) ||
                        !builder.Link(garbageNodes, all, options.refs)) {
                        return 3;
                    }

                    garbagePins.Clear();
                    createTotal += platform::MonotonicNanoseconds() - before;
                    auto collected = registry.Collect(access);
                    if (!collected) {
                        return 4;
                    }

                    reclaimTotal += collected.Value().reclaimNanoseconds;
                }

                roots.Clear();
                const uint64 teardown = platform::MonotonicNanoseconds();
                if (!registry.Shutdown(access)) {
                    return 7;
                }

                teardownTime = platform::MonotonicNanoseconds() - teardown;
            }

            if (!types.Unregister(tokens.leaf) || !types.Unregister(tokens.node) ||
                !types.Shutdown()) {
                return 7;
            }
        }

        if (!binned.Shutdown()) {
            return 7;
        }

        const double churned = static_cast<double>(LiveObjects / 10) * Rounds;
        printf("  %-16s create+link %8.1f ns/object, reclaim %8.1f ns/object, teardown %8.1f ms\n",
               name, static_cast<double>(createTotal) / churned,
               static_cast<double>(reclaimTotal) / churned, Milliseconds(teardownTime));
        fflush(stdout);
    }

    return 0;
}

void PrintSoakReport(const GcSoakReport& report) noexcept {
    printf("  created %llu, reclaimed %llu, peak tracked %u, cycles begun %llu / completed %llu, "
           "Collect %llu, cancels %llu, drains %llu, steps %llu, verifications %llu\n",
           static_cast<unsigned long long>(report.created),
           static_cast<unsigned long long>(report.reclaimed), report.peakObjects,
           static_cast<unsigned long long>(report.cyclesBegun),
           static_cast<unsigned long long>(report.cyclesCompleted),
           static_cast<unsigned long long>(report.collects),
           static_cast<unsigned long long>(report.cancels),
           static_cast<unsigned long long>(report.drains),
           static_cast<unsigned long long>(report.steps),
           static_cast<unsigned long long>(report.verifications));
    printf(
        "  edge writes %llu (rejected as expected %llu), root changes %llu, RequestDestroy %llu, "
        "Discard %llu, holds %llu, cross-thread releases in slice %llu / outside %llu, Busy "
        "checks %llu\n",
        static_cast<unsigned long long>(report.edgeWrites),
        static_cast<unsigned long long>(report.rejectedWrites),
        static_cast<unsigned long long>(report.rootChanges),
        static_cast<unsigned long long>(report.destroys),
        static_cast<unsigned long long>(report.discards),
        static_cast<unsigned long long>(report.holds),
        static_cast<unsigned long long>(report.crossThreadInSlice),
        static_cast<unsigned long long>(report.crossThreadOutside),
        static_cast<unsigned long long>(report.busyChecks));
    if (report.failure != nullptr) {
        printf("  FAILURE at tick %llu (object %u, error %u): %s\n",
               static_cast<unsigned long long>(report.failureTick), report.failureObject,
               static_cast<unsigned>(report.failureError), report.failure);
    }
}

int RunSoak(const Options& options) noexcept {
    SystemAllocator memory;
    GcSoakReport report;
    {
        WorkerScheduler scheduler(memory);
        if (options.soakWorkers && !scheduler.Start({options.workers, 256, 16})) {
            return 2;
        }

        GcSoakOptions soak;
        soak.seed = options.seed;
        soak.ticks = options.soakTicks;
        soak.maxObjects = options.soakObjects;
        soak.timeBudget = options.soakTimeBudget;
        soak.scheduler = options.soakWorkers ? &scheduler : nullptr;
        const uint64 started = platform::MonotonicNanoseconds();
        const bool passed = RunGcSoak(memory, soak, report);
        printf("soak seed %llu ticks %llu: %s (%.1f s)\n",
               static_cast<unsigned long long>(options.seed),
               static_cast<unsigned long long>(report.ticks), passed ? "PASS" : "FAIL",
               static_cast<double>(platform::MonotonicNanoseconds() - started) * 1e-9);
        PrintSoakReport(report);
        if (options.soakWorkers && !scheduler.Shutdown()) {
            return 2;
        }

        if (!passed) {
            return 9;
        }
    }

    return memory.Stats().liveAllocations == 0 ? 0 : 8;
}

bool ParseNumber(const char* text, uint64 maximum, uint64& value) noexcept {
    value = 0;
    if (*text == '\0') {
        return false;
    }

    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9') {
            return false;
        }

        value = value * 10 + static_cast<uint64>(*digit - '0');
        if (value > maximum) {
            return false;
        }
    }

    return true;
}

bool Parse(int argc, char** argv, Options& options) noexcept {
    for (int index = 1; index < argc; ++index) {
        const bool hasValue = index + 1 < argc;
        uint64 value = 0;
        if (platform::CStringEquals(argv[index], "--allocators")) {
            options.allocators = true;
        } else if (platform::CStringEquals(argv[index], "--soak-workers")) {
            options.soakWorkers = true;
        } else if (platform::CStringEquals(argv[index], "--soak-time-budget")) {
            options.soakTimeBudget = true;
        } else if (!hasValue || !ParseNumber(argv[index + 1], 1ULL << 40, value)) {
            return false;
        } else if (platform::CStringEquals(argv[index], "--objects")) {
            options.objects = static_cast<uint32>(value > 8000000 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--refs")) {
            options.refs = static_cast<uint32>(value > 64 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--leaf")) {
            options.leafPercent = static_cast<uint32>(value > 100 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--reps")) {
            options.reps = static_cast<uint32>(value > 100 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--workers")) {
            options.workers = static_cast<uint32>(value > 64 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--soak")) {
            options.soakTicks = value;
            ++index;
        } else if (platform::CStringEquals(argv[index], "--seed")) {
            options.seed = value;
            ++index;
        } else if (platform::CStringEquals(argv[index], "--soak-objects")) {
            options.soakObjects = static_cast<uint32>(value > 1000000 ? 0 : value);
            ++index;
        } else {
            return false;
        }
    }

    return options.workers != 0 && options.soakObjects != 0;
}

int Run(const Options& options) noexcept {
    if (options.soakTicks != 0) {
        return RunSoak(options);
    }

    if (options.allocators) {
        return RunAllocatorComparison(options);
    }

    if (options.objects != 0) {
        const uint32 reps = options.reps != 0 ? options.reps : 5;
        return RunScenario({options.objects, options.refs, options.leafPercent, reps}, options);
    }

    const Scenario scenarios[] = {{100000, 4, 0, 5},
                                  {100000, 8, 0, 5},
                                  {1000000, 4, 0, 3},
                                  {1000000, 8, 0, 3},
                                  {1000000, 4, 50, 3}};
    for (const Scenario& scenario : scenarios) {
        Scenario chosen = scenario;
        if (options.reps != 0) {
            chosen.reps = options.reps;
        }

        if (const int status = RunScenario(chosen, options); status != 0) {
            return status;
        }
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    rocks::test::PrepareAutomatedTestProcess();
    Options options;
    if (!Parse(argc, argv, options)) {
        printf("usage: GcBench [--objects N] [--refs K] [--leaf P] [--reps R] [--workers W] "
               "[--allocators] [--soak TICKS [--seed S] [--soak-objects N] [--soak-workers] "
               "[--soak-time-budget]]\n");
        return 1;
    }

    printf("GcBench: workers %u\n", options.workers);
    return Run(options);
}
