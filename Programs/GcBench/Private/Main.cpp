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
// - 병렬 표식: worker 2·4·8·16개의 정지형 Collect, worker N개(--workers)의 증분 수집
// - churn: 살아 있는 객체의 10%만큼 쓰레기 묶음을 만든 뒤 Collect(회수 시간)
// - 메모리: 범주별(Slot chunk·객체·참조 배열·이름·GC 밀집 자료·기타) 바이트와 최고치
//
// 사용:
//   GcBench                          기본 표(100K·1M × 간선 4·8, 1M 잎 50%)
//   GcBench --objects 100000 --refs 4 [--leaf 50] [--reps 5] [--workers 8]
//   GcBench --allocators             할당자 비교(작은 그래프의 churn 회수, platform·System·Binned)
//   GcBench --soak 200000 [--seed 7] [--soak-objects 4000] [--soak-workers --workers 4]
//                  [--soak-time-budget]
//                                    장기 tick 시뮬레이터(GcSoak)를 돌리고 불변식 위반을 보고한다
//   GcBench --cluster-soak 2000 [--packages] [--seed 7] [--soak-objects 1500] [--soak-workers]
//                                    클러스터 장기 smoke(GcClusterSoak — Debug면 장부 검증 포함)
//   GcBench --clusters               클러스터·영구 영역 시나리오(GC-S7, 패키지·World·영구 영역,
//                                    간선 쓰기 비용 — 클러스터를 끈 경우와 번갈아 잰다)
namespace {

using namespace rocks;

constexpr uint32 GroupSize = 256;
constexpr uint32 RandomRoots = 1000;
constexpr TypeId NodeTypeId{0x4763424e4f444531ULL, 1};
constexpr TypeId LeafTypeId{0x4763424c45414631ULL, 1};
constexpr TypeId PackageTypeId{0x4763425041434b31ULL, 1};

// 할당 범주다. ObjectRuntime의 할당 태그(ObjectRegistryCreate.cpp·ObjectRegistryArrayEdges.cpp)로
// 나눈다.
enum class Category : uint32 {
    SlotChunk,
    ObjectStorage,
    RefArray,
    Names,
    GcDense,
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
    case 0x47435047u: // 'GCPG' GC 페이지(hot 레코드·비트 묶음)
    case 0x47435054u: // 'GCPT'
    case 0x47435459u: // 'GCTY'
    case 0x4743524cu: // 'GCRL' 루트 목록
        return Category::GcDense;
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

// 패키지 정책 Registry의 패키지 객체다(클러스터 시나리오).
class BenchPackage final : public Object {
public:
    BenchPackage() noexcept = default;
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

// 패키지형 그래프의 객체 수와 기본 패키지 크기다(--package-size로 바꾼다 — 64보다 작으면 chunk
// 하나에 클러스터가 셋 이상 섞인다).
constexpr uint32 PackageObjects = 1000000;
constexpr uint32 DefaultPackageSize = 100;
constexpr uint32 MinPackageSize = 2;

struct Options {
    uint32 objects = 0;
    uint32 refs = 4;
    uint32 leafPercent = 0;
    uint32 reps = 0;
    uint32 workers = 8;
    bool allocators = false;
    bool clusters = false;
    bool packages = false;
    // 참조 위치 표(M3)를 끈다(RegistryOptions::referenceTables — 비교 측정).
    bool noTables = false;
    // 패키지 시나리오 하나만 직렬 Collect를 반복해 작업 계수를 낸다(--package-profile CROSS —
    // 표본 프로파일러를 붙이는 동안 돈다). packageClustersOff면 클러스터를 끈다.
    uint32 packageProfile = 0;
    bool packageProfileSet = false;
    bool packageClustersOff = false;
    // 작업 계수를 센다(--count-work — 간선마다 읽기가 늘어 시간 비교에는 끈다).
    bool countWork = false;
    // 일반 그래프 시나리오에서 정지형 표식만 잰다(--mark-only — 직렬과 병렬 x8·x16 Collect를 reps번
    // 되풀이해 표식 시간의 최소·중앙·최대를 낸다. 표본 프로파일러를 붙일 때도 쓴다).
    bool markOnly = false;
    // 패키지 시나리오 하나에서 Collect 반복 전에 간선 쓰기를 이만큼 되풀이해 잰다(--write-rounds —
    // 부하가 있는 기계에서 최솟값을 보려고).
    uint32 writeRounds = 0;
    // --write-rounds의 쓰기마다 시간을 재 p50·p99·p99.9·max와 가장 느린 연산을
    // 낸다(--write-latency).
    bool writeLatency = false;
    // 패키지 시나리오 하나에서 Collect 반복 뒤 기본 예산 증분 cycle을 이만큼 돌려 가장 긴 slice를
    // 잰다(--incremental-reps — cutoff·정리 slice가 클러스터 수에 비례하는지 본다).
    uint32 incrementalReps = 0;
    // World형 그래프에서 Level 소속(Actor·Component)의 이 비율(%)이 다른 Level의 Actor를 하나 더
    // 가리킨다(--world-boundary P — 경계 소속 비율이 대략 P%가 된다. World 시나리오만 켬·끔으로
    // 돈다).
    uint32 worldBoundary = 0;
    bool worldBoundarySet = false;
    // 패키지형 그래프의 패키지 크기다(--package-size).
    uint32 packageSize = DefaultPackageSize;
    uint64 clusterSoakTicks = 0;
    uint64 soakTicks = 0;
    uint64 seed = 7;
    uint32 soakObjects = 4000;
    bool soakWorkers = false;
    bool soakTimeBudget = false;
};

struct TypeTokens {
    TypeRegistrationToken node;
    TypeRegistrationToken leaf;
    TypeRegistrationToken package;
};

bool RegisterTypes(TypeRegistry& types, TypeTokens& tokens) noexcept {
    RuntimeTypeOps node = MakeRuntimeTypeOps<BenchNode>();
    node.parallelTraceSafe = true;
    // Trace가 모든 강한 간선을 방문한다(클러스터 소속·영구 영역·패키지 정책의 조건).
    node.completeFieldLayout = true;
    // 참조 위치 표(M3): 리플렉션 클래스의 생성 함수와 같은 모양의 수동 함수다.
    node.referenceLayout =
        +[](const Object& object, detail::ReferenceLayoutBuilder& layout) noexcept {
            static_cast<const BenchNode&>(object).refs.ReportLayout(layout);
        };
    auto registeredNode = types.Register(TypeSchema{NodeTypeId, 1, {}, {}}, Move(node));
    RuntimeTypeOps package = MakeRuntimeTypeOps<BenchPackage>();
    package.parallelTraceSafe = true;
    package.completeFieldLayout = true;
    auto registeredPackage = types.Register(TypeSchema{PackageTypeId, 1, {}, {}}, Move(package));
    if (!registeredPackage) {
        return false;
    }

    tokens.package = registeredPackage.Value();
    RuntimeTypeOps leaf = MakeRuntimeTypeOps<BenchLeaf>();
    leaf.parallelTraceSafe = true;
    // 강한 참조가 없다(GC marker가 trace하지 않는다 — M2).
    leaf.hasStrongReferences = false;
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

// 표식 시간의 최소·중앙·최대다(--mark-only).
void PrintMarkSpread(const char* label, const Array<GcReport>& reports) noexcept {
    uint64 values[256];
    const usize count = reports.Size() < 256 ? reports.Size() : 256;
    for (usize index = 0; index < count; ++index) {
        values[index] = reports[index].markNanoseconds;
    }

    for (usize outer = 1; outer < count; ++outer) {
        for (usize inner = outer; inner > 0 && values[inner - 1] > values[inner]; --inner) {
            const uint64 swapped = values[inner];
            values[inner] = values[inner - 1];
            values[inner - 1] = swapped;
        }
    }

    if (count != 0) {
        printf("  %-28s mark min %8.3f  median %8.3f  max %8.3f ms (%llu reps)\n", label,
               Milliseconds(values[0]), Milliseconds(values[count / 2]),
               Milliseconds(values[count - 1]), static_cast<unsigned long long>(count));
    }
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
    // Step마다의 내역(GcSliceTimes): 준비·표식·worker 회수·회색 넘기기·Step 밖 비용(잠금·진입).
    Array<uint64> setups(allocator);
    Array<uint64> markings(allocator);
    Array<uint64> drains(allocator);
    Array<uint64> requeues(allocator);
    Array<uint64> outside(allocator);
    uint64 carried = 0;
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

        const GcSliceTimes& slice = progress.Value().slice;
        const uint64 inside =
            slice.setup + slice.marking + slice.drain + slice.requeue + slice.sweep + slice.reclaim;
        carried += slice.grayCarried;
        if (!steps.TryPushBack(after - before) || !setups.TryPushBack(slice.setup) ||
            !markings.TryPushBack(slice.marking) || !drains.TryPushBack(slice.drain) ||
            !requeues.TryPushBack(slice.requeue) ||
            !outside.TryPushBack(after - before > inside ? after - before - inside : 0)) {
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
    Array<uint64>* parts[] = {&setups, &markings, &drains, &requeues, &outside};
    for (Array<uint64>* part : parts) {
        part->Sort();
    }

    printf("  %-28s p50/p99 us: setup %.1f/%.1f  mark %.1f/%.1f  drain %.1f/%.1f  requeue "
           "%.1f/%.1f  other %.1f/%.1f | gray carried avg %.0f\n",
           "", Microseconds(Percentile(setups, 50)), Microseconds(Percentile(setups, 99)),
           Microseconds(Percentile(markings, 50)), Microseconds(Percentile(markings, 99)),
           Microseconds(Percentile(drains, 50)), Microseconds(Percentile(drains, 99)),
           Microseconds(Percentile(requeues, 50)), Microseconds(Percentile(requeues, 99)),
           Microseconds(Percentile(outside, 50)), Microseconds(Percentile(outside, 99)),
           steps.IsEmpty() ? 0.0
                           : static_cast<double>(carried) / static_cast<double>(steps.Size()));
    return true;
}

void PrintMemory(const BenchAllocator& memory, uint64 objects, const ObjectStats& stats) noexcept {
    const uint64 slots = uint64(stats.chunks) * 64;
    const double perObject = objects == 0 ? 0.0 : 1.0 / static_cast<double>(objects);
    printf(
        "  memory: slot chunks %.1f MiB (%.1f B/slot, %llu slots), objects %.1f MiB, ref "
        "arrays %.1f MiB, names %.1f MiB, GC dense %.1f MiB (%.1f B/slot), other %.1f MiB, total "
        "%.1f MiB (%.1f B/object), peak %.1f MiB\n",
        static_cast<double>(memory.Live(Category::SlotChunk)) / 1048576.0,
        slots == 0
            ? 0.0
            : static_cast<double>(memory.Live(Category::SlotChunk)) / static_cast<double>(slots),
        static_cast<unsigned long long>(slots),
        static_cast<double>(memory.Live(Category::ObjectStorage)) / 1048576.0,
        static_cast<double>(memory.Live(Category::RefArray)) / 1048576.0,
        static_cast<double>(memory.Live(Category::Names)) / 1048576.0,
        static_cast<double>(memory.Live(Category::GcDense)) / 1048576.0,
        slots == 0
            ? 0.0
            : static_cast<double>(memory.Live(Category::GcDense)) / static_cast<double>(slots),
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
        registryOptions.referenceTables = !options.noTables;
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
            if (options.markOnly) {
                PrintMarkSpread("serial Collect", reports);
                const uint32 markWorkers[] = {8, 16};
                for (uint32 workers : markWorkers) {
                    WorkerScheduler parallel(memory);
                    if (!parallel.Start({workers, 256, 16})) {
                        printf("scheduler start failed\n");
                        return 5;
                    }

                    reports.Clear();
                    for (uint32 rep = 0; rep < scenario.reps; ++rep) {
                        auto collected = registry.Collect(access, &parallel);
                        if (!collected || !reports.TryPushBack(collected.Value())) {
                            printf("parallel Collect failed\n");
                            return 5;
                        }
                    }

                    char label[64];
                    snprintf(label, sizeof(label), "parallel Collect x%u", workers);
                    PrintMarkSpread(label, reports);
                    if (!parallel.Shutdown()) {
                        return 5;
                    }
                }
            }

            if (!options.markOnly) {
                if (!RunIncremental(registry, "incremental default budget", GcBudget{}, nullptr,
                                    memory) ||
                    !RunIncremental(registry, "incremental time 2ms",
                                    GcBudget{NumericLimits<uint32>::Max(), 2000000}, nullptr,
                                    memory) ||
                    !RunIncremental(registry, "incremental time 0.5ms",
                                    GcBudget{NumericLimits<uint32>::Max(), 500000}, nullptr,
                                    memory)) {
                    return 5;
                }

                // 병렬 표식(GC-S4): worker 수마다 정지형 Collect(STW 병렬 전체 수집)를 잰다. 소유
                // 스레드도 참여하므로 참여자는 worker + 1이다.
                const uint32 workerCounts[] = {2, 4, 8, 16};
                for (uint32 workers : workerCounts) {
                    WorkerScheduler parallel(memory);
                    if (!parallel.Start({workers, 256, 16})) {
                        printf("scheduler start failed\n");
                        return 5;
                    }

                    reports.Clear();
                    for (uint32 rep = 0; rep < scenario.reps; ++rep) {
                        auto collected = registry.Collect(access, &parallel);
                        if (!collected || !reports.TryPushBack(collected.Value())) {
                            printf("parallel Collect failed\n");
                            return 5;
                        }
                    }

                    char label[64];
                    snprintf(label, sizeof(label), "parallel Collect x%u", workers);
                    PrintPhases(label, MedianOf(reports, memory));
                    if (!parallel.Shutdown()) {
                        return 5;
                    }
                }

                char label[64];
                snprintf(label, sizeof(label), "incremental default x%u", options.workers);
                if (!RunIncremental(registry, label, GcBudget{}, &scheduler, memory)) {
                    return 5;
                }

                snprintf(label, sizeof(label), "incremental time 2ms x%u", options.workers);
                if (!RunIncremental(registry, label,
                                    GcBudget{NumericLimits<uint32>::Max(), 2000000}, &scheduler,
                                    memory)) {
                    return 5;
                }

                // churn: 살아 있는 객체의 10%만큼 쓰레기 묶음을 만들고(간선은 산 그래프로)
                // 회수한다.
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
            }

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

        if (!types.Unregister(tokens.leaf) || !types.Unregister(tokens.node) ||
            !types.Unregister(tokens.package) || !types.Shutdown()) {
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

// ---- 클러스터·영구 영역 시나리오(GC-S7, Docs/Design/GarbageCollectionScale.ko.md 13절) ----

// 그래프 하나의 GC 측정이다: 정지형 직렬·병렬(worker N개) Collect 중앙값, 증분 기본(직렬)·기본과
// 2ms(병렬). 실패하면 거짓.
bool MeasureGraph(ObjectRegistry& registry, WorkerScheduler& scheduler, uint32 workers, uint32 reps,
                  Allocator& memory) noexcept {
    const auto access = registry.OwnerAccess();
    Array<GcReport> reports(memory);
    for (uint32 rep = 0; rep < reps; ++rep) {
        auto collected = registry.Collect(access);
        if (!collected || !reports.TryPushBack(collected.Value())) {
            printf("Collect failed\n");
            return false;
        }
    }

    PrintPhases("Collect (median)", MedianOf(reports, memory));
    printf("  %-28s marked %u, examined %u\n", "", reports[0].marked, reports[0].examined);
    reports.Clear();
    for (uint32 rep = 0; rep < reps; ++rep) {
        auto collected = registry.Collect(access, &scheduler);
        if (!collected || !reports.TryPushBack(collected.Value())) {
            printf("parallel Collect failed\n");
            return false;
        }
    }

    char label[64];
    snprintf(label, sizeof(label), "parallel Collect x%u", workers);
    PrintPhases(label, MedianOf(reports, memory));
    if (!RunIncremental(registry, "incremental default budget", GcBudget{}, nullptr, memory)) {
        return false;
    }

    snprintf(label, sizeof(label), "incremental default x%u", workers);
    if (!RunIncremental(registry, label, GcBudget{}, &scheduler, memory)) {
        return false;
    }

    snprintf(label, sizeof(label), "incremental time 2ms x%u", workers);
    return RunIncremental(registry, label, GcBudget{NumericLimits<uint32>::Max(), 2000000},
                          &scheduler, memory);
}

// owner의 refs에 target을 붙인다(Borrow를 쥐고 편집한다).
bool AddEdge(ObjectRegistry& registry, ObjectHandle owner, ObjectHandle target) noexcept {
    auto borrowed = registry.Resolve<BenchNode>(registry.OwnerAccess(), owner);
    if (!borrowed) {
        return false;
    }

    auto pointer = borrowed.Value().Get();
    return pointer &&
           registry.AddReference(registry.OwnerAccess(), owner, pointer.Value()->refs, target);
}

// 간선 쓰기 비용: 무작위 노드에 간선 count개를 더한 뒤(AddReference) 끝에서 지운다
// (RemoveReferenceAt). pick이 (소유자, 대상)을 고른다. 객체 Borrow 비용이 함께 들어간다.
template <class Pick>
bool MeasureEdgeWrites(ObjectRegistry& registry, uint32 count, Pick&& pick, Allocator& memory,
                       bool latency = false) noexcept {
    const auto access = registry.OwnerAccess();
    Array<ObjectHandle> owners(memory);
    if (!owners.TryReserve(count)) {
        return false;
    }

    // --write-latency: 연산마다 시간을 재 분포를 낸다(시계 읽기 비용이 연산마다 더해진다).
    Array<uint64> addTimes(memory);
    Array<uint64> removeTimes(memory);
    if (latency && (!addTimes.TryReserve(count) || !removeTimes.TryReserve(count))) {
        return false;
    }

    const uint64 started = platform::MonotonicNanoseconds();
    for (uint32 index = 0; index < count; ++index) {
        ObjectHandle owner;
        ObjectHandle target;
        pick(owner, target);
        const uint64 opStarted = latency ? platform::MonotonicNanoseconds() : 0;
        if (!AddEdge(registry, owner, target) || !owners.TryPushBack(owner)) {
            return false;
        }

        if (latency && !addTimes.TryPushBack(platform::MonotonicNanoseconds() - opStarted)) {
            return false;
        }
    }

    const uint64 added = platform::MonotonicNanoseconds();
    for (usize index = owners.Size(); index-- != 0;) {
        const uint64 opStarted = latency ? platform::MonotonicNanoseconds() : 0;
        auto borrowed = registry.Resolve<BenchNode>(access, owners[index]);
        if (!borrowed) {
            return false;
        }

        auto pointer = borrowed.Value().Get();
        if (!pointer || !registry.RemoveReferenceAt(access, owners[index], pointer.Value()->refs,
                                                    pointer.Value()->refs.Size() - 1)) {
            return false;
        }

        if (latency && !removeTimes.TryPushBack(platform::MonotonicNanoseconds() - opStarted)) {
            return false;
        }
    }

    const uint64 removed = platform::MonotonicNanoseconds();
    printf("  edge writes x%u: add %.1f ns/op, remove %.1f ns/op\n", count,
           static_cast<double>(added - started) / count,
           static_cast<double>(removed - added) / count);
    if (latency) {
        auto print = [&](const char* label, Array<uint64>& times) noexcept {
            times.Sort();
            const usize size = times.Size();
            auto at = [&](usize permille) noexcept {
                const usize index = size * permille / 1000;
                return static_cast<double>(times[index < size ? index : size - 1]);
            };
            // 가장 느린 연산 열두 개다(기록이 찰 때의 적용이 여기 든다).
            printf("    %s ns: p50 %.0f p99 %.0f p99.9 %.0f max %.0f | slowest", label, at(500),
                   at(990), at(999), static_cast<double>(times[size - 1]));
            constexpr usize Slowest = 12;
            for (usize index = size > Slowest ? size - Slowest : 0; index < size; ++index) {
                printf(" %llu", static_cast<unsigned long long>(times[index]));
            }

            printf("\n");
        };
        print("add", addTimes);
        print("remove", removeTimes);
    }

    return true;
}

// --write-rounds 한 번의 간선 쓰기 수다.
constexpr uint32 ProfileWrites = 50000;

// 패키지형: 10K 패키지 × 100 객체(1M), 노드마다 평균 4개 간선, 그중 cross%는 다른 패키지로 간다.
// clusters가 거짓이면 패키지 클러스터를 끈다(RegistryOptions::packageClusters).
int RunPackageScenario(uint32 crossPercent, bool clusters, const Options& options) noexcept {
    const uint32 PackageSize = options.packageSize;
    const uint32 Packages = PackageObjects / PackageSize;
    printf("\n== package graph: %u packages x %u, refs avg 4, cross %u%%, clusters %s ==\n",
           Packages, PackageSize, crossPercent, clusters ? "on" : "off");
    fflush(stdout);
    BenchAllocator memory;
    int status = 0;
    {
        TypeRegistry types(memory);
        TypeTokens tokens;
        if (!RegisterTypes(types, tokens)) {
            return 2;
        }

        WorkerScheduler scheduler(memory);
        if (!scheduler.Start({options.workers, 256, 16})) {
            return 2;
        }

        {
            RegistryOptions registryOptions;
            registryOptions.referenceTables = !options.noTables;
            registryOptions.policy.packageNativeType = NativeTypeKey<BenchPackage>();
            registryOptions.packageClusters = clusters;
            registryOptions.countGcWork = options.countWork;
            ObjectRegistry registry(memory, types, Guid{0x6764, crossPercent}, registryOptions);
            const auto access = registry.OwnerAccess();
            Rng rng{0x51ed270b9a3c4d21ULL ^ crossPercent};
            const uint32 Total = Packages * PackageSize;
            Array<ObjectHandle> all(memory);
            Array<RootRef<Object>> roots(memory);
            if (!all.TryReserve(Total) || !roots.TryReserve(Packages)) {
                return 3;
            }

            const uint64 started = platform::MonotonicNanoseconds();
            char name[16];
            for (uint32 package = 0; package < Packages; ++package) {
                ConstructionContext construction(registry, access);
                auto made = construction.Create<BenchPackage>(PackageTypeId);
                if (!made || !all.TryPushBack(made.Value())) {
                    return 3;
                }

                for (uint32 index = 1; index < PackageSize; ++index) {
                    auto node = construction.Create<BenchNode>(NodeTypeId, made.Value(),
                                                               NameOf(name, index));
                    if (!node || !all.TryPushBack(node.Value())) {
                        return 3;
                    }
                }

                const ObjectHandle held[] = {made.Value()};
                RootRef<Object> out[1];
                if (!construction.Commit(held, out) || !roots.TryPushBack(Move(out[0]))) {
                    return 3;
                }
            }

            const uint64 created = platform::MonotonicNanoseconds();
            auto pick = [&](ObjectHandle& owner, ObjectHandle& target) noexcept {
                const uint32 package = rng.Below(Packages);
                owner = all[package * PackageSize + 1 + rng.Below(PackageSize - 1)];
                const uint32 targetPackage =
                    rng.Below(100) < crossPercent ? rng.Below(Packages) : package;
                target = all[targetPackage * PackageSize + rng.Below(PackageSize)];
            };
            for (uint32 package = 0; package < Packages; ++package) {
                for (uint32 index = 1; index < PackageSize; ++index) {
                    const ObjectHandle owner = all[package * PackageSize + index];
                    const uint32 degree = rng.Below(9);
                    for (uint32 edge = 0; edge < degree; ++edge) {
                        const uint32 targetPackage =
                            rng.Below(100) < crossPercent ? rng.Below(Packages) : package;
                        if (!AddEdge(registry, owner,
                                     all[targetPackage * PackageSize + rng.Below(PackageSize)])) {
                            return 3;
                        }
                    }
                }
            }

            const uint64 linked = platform::MonotonicNanoseconds();
            const ObjectStats stats = registry.Stats();
            printf("  create %.1f ms, link %.1f ms, clusters %u, members %u\n",
                   Milliseconds(created - started), Milliseconds(linked - created), stats.clusters,
                   stats.clusterMembers);
            PrintMemory(memory, all.Size(), stats);
            if (options.packageProfileSet) {
                for (uint32 round = 0; round < options.writeRounds; ++round) {
                    if (!MeasureEdgeWrites(registry, ProfileWrites, pick, memory,
                                           options.writeLatency)) {
                        return 5;
                    }
                }

                // 작업 계수와 직렬 Collect 반복(표본 프로파일러용).
                const uint32 reps = options.reps != 0 ? options.reps : 5;
                Array<GcReport> reports(memory);
                for (uint32 rep = 0; rep < reps; ++rep) {
                    auto collected = registry.Collect(access);
                    if (!collected || !reports.TryPushBack(collected.Value())) {
                        return 4;
                    }
                }

                PrintPhases("Collect (median)", MedianOf(reports, memory));
                // 기본 예산 증분 cycle의 가장 긴 slice(벽시계)다. cycle마다의 최댓값 가운데
                // 최솟값을 쓴다(선점은 늘리기만 한다).
                uint64 slowestSlice = NumericLimits<uint64>::Max();
                uint32 slices = 0;
                for (uint32 cycle = 0; cycle < options.incrementalReps; ++cycle) {
                    if (!registry.BeginCollection(access)) {
                        return 4;
                    }

                    uint64 longest = 0;
                    slices = 0;
                    while (registry.CollectionPhase() != GcPhase::Idle) {
                        const uint64 stepStarted = platform::MonotonicNanoseconds();
                        if (!registry.StepCollection(access, GcBudget{})) {
                            return 4;
                        }

                        const uint64 took = platform::MonotonicNanoseconds() - stepStarted;
                        longest = took > longest ? took : longest;
                        ++slices;
                    }

                    slowestSlice = longest < slowestSlice ? longest : slowestSlice;
                }

                if (options.incrementalReps != 0) {
                    printf("  incremental default budget: longest slice %.1f us, slices %u\n",
                           static_cast<double>(slowestSlice) * 1e-3, slices);
                }

                // 부하가 있는 기계에서는 최솟값이 고유 비용에 더 가깝다(선점은 늘리기만 한다).
                uint64 fastest = NumericLimits<uint64>::Max();
                for (usize at = 0; at < reports.Size(); ++at) {
                    fastest = reports[at].markNanoseconds < fastest ? reports[at].markNanoseconds
                                                                    : fastest;
                }

                printf("  mark min %.3f ms\n", Milliseconds(fastest));
                // 경계 비율 안전장치가 첫 Collect 끝에 해체한 패키지 클러스터 수(이후 반복은 그
                // 상태다).
                printf("  flattened clusters %u (first Collect), clusters left %u\n",
                       reports[0].flattenedClusters, registry.Stats().clusters);
                const GcWorkCounters& work = reports[0].work;
                if (options.countWork) {
                    printf("  work: traced %llu, edges %llu, clustered edges %llu "
                           "(same cluster %llu), cluster items %llu, boundary pushes %llu, "
                           "detached marks %llu, cluster processing %.3f ms\n",
                           static_cast<unsigned long long>(work.tracedObjects),
                           static_cast<unsigned long long>(work.edges),
                           static_cast<unsigned long long>(work.clusteredEdges),
                           static_cast<unsigned long long>(work.sameClusterEdges),
                           static_cast<unsigned long long>(work.clusterItems),
                           static_cast<unsigned long long>(work.boundaryPushes),
                           static_cast<unsigned long long>(work.detachedMarks),
                           Milliseconds(work.clusterProcessNanoseconds));
                }

                fflush(stdout);
            }

            if (!options.packageProfileSet &&
                (!MeasureEdgeWrites(registry, 200000, pick, memory) ||
                 !MeasureGraph(registry, scheduler, options.workers, 3, memory))) {
                return 5;
            }

            fflush(stdout);
            for (uint32 package = 0; package < Packages; ++package) {
                if (!registry.Discard(access, all[package * PackageSize])) {
                    return 6;
                }
            }

            roots.Clear();
            if (!registry.Shutdown(access)) {
                status = 7;
            }
        }

        if (!scheduler.Shutdown() || !types.Unregister(tokens.leaf) ||
            !types.Unregister(tokens.node) || !types.Unregister(tokens.package) ||
            !types.Shutdown()) {
            status = 7;
        }
    }

    fflush(stdout);
    return memory.Allocations() == 0 ? status : 8;
}

constexpr uint32 Levels = 10;
constexpr uint32 ActorsPerLevel = 30000;
constexpr uint32 OutsideObjects = 200000;

// World형: Level 10개 × Actor 30K × (Actor + Component 2개), Actor마다 같은 Level Actor 참조 1개,
// Level 밖 일반 객체 200K(평균 간선 4, 그중 5%는 Level 소속을 하나 더 가리킨다), 일반 객체 churn.
// clusters가 참이면 Level마다 클러스터를 지정한다(World와 같은 방식).
int RunWorldScenario(bool clusters, const Options& options) noexcept {
    printf("\n== world graph: %u levels x %u actors x (actor + 2 components), outside %u, "
           "cross-level members %u%%, clusters %s ==\n",
           Levels, ActorsPerLevel, OutsideObjects, options.worldBoundary, clusters ? "on" : "off");
    fflush(stdout);
    BenchAllocator memory;
    int status = 0;
    {
        TypeRegistry types(memory);
        TypeTokens tokens;
        if (!RegisterTypes(types, tokens)) {
            return 2;
        }

        WorkerScheduler scheduler(memory);
        if (!scheduler.Start({options.workers, 256, 16})) {
            return 2;
        }

        {
            RegistryOptions registryOptions;
            registryOptions.referenceTables = !options.noTables;
            registryOptions.clusters = clusters;
            ObjectRegistry registry(memory, types, Guid{0x6765, clusters ? 1u : 0u},
                                    registryOptions);
            const auto access = registry.OwnerAccess();
            Rng rng{0x6a09e667f3bcc909ULL};
            Array<RootRef<Object>> levels(memory);
            Array<ObjectHandle> actors(memory);
            Array<ObjectHandle> members(memory);
            if (!levels.TryReserve(Levels) || !actors.TryReserve(Levels * ActorsPerLevel) ||
                !members.TryReserve(Levels * ActorsPerLevel * 3)) {
                return 3;
            }

            const uint64 started = platform::MonotonicNanoseconds();
            for (uint32 level = 0; level < Levels; ++level) {
                ConstructionContext construction(registry, access);
                auto made = construction.Create<BenchNode>(NodeTypeId);
                if (!made || (clusters && !construction.SetClusterRoot(made.Value()))) {
                    return 3;
                }

                const ObjectHandle held[] = {made.Value()};
                RootRef<Object> out[1];
                if (!construction.Commit(held, out) || !levels.TryPushBack(Move(out[0]))) {
                    return 3;
                }

                const ObjectHandle levelHandle = levels[level].Handle();
                for (uint32 actor = 0; actor < ActorsPerLevel; ++actor) {
                    ConstructionContext spawn(registry, access);
                    auto body = spawn.Create<BenchNode>(NodeTypeId);
                    auto first = spawn.Create<BenchNode>(NodeTypeId);
                    auto second = spawn.Create<BenchNode>(NodeTypeId);
                    if (!body || !first || !second) {
                        return 3;
                    }

                    if (clusters && (!spawn.JoinCluster(body.Value(), levelHandle) ||
                                     !spawn.JoinCluster(first.Value(), levelHandle) ||
                                     !spawn.JoinCluster(second.Value(), levelHandle))) {
                        return 3;
                    }

                    auto pointer = spawn.Get<BenchNode>(body.Value());
                    const ObjectHandle components[] = {first.Value(), second.Value()};
                    if (!pointer ||
                        !spawn.AssignReferences(body.Value(), pointer.Value()->refs,
                                                Span<const ObjectHandle>(components, 2)) ||
                        !spawn.Commit(Span<const ObjectHandle>{}, Span<RootRef<Object>>{})) {
                        return 3;
                    }

                    // Level이 Actor를 소유한다(World의 Level 배열과 같다).
                    if (!AddEdge(registry, levelHandle, body.Value()) ||
                        !actors.TryPushBack(body.Value()) || !members.TryPushBack(body.Value()) ||
                        !members.TryPushBack(first.Value()) ||
                        !members.TryPushBack(second.Value())) {
                        return 3;
                    }
                }
            }

            // 같은 Level 안 Actor 사이 참조.
            for (uint32 index = 0; index < actors.Size(); ++index) {
                const uint32 level = index / ActorsPerLevel;
                if (!AddEdge(registry, actors[index],
                             actors[level * ActorsPerLevel + rng.Below(ActorsPerLevel)])) {
                    return 3;
                }
            }

            // 경계 소속(--world-boundary): 소속의 worldBoundary%가 다른 Level의 Actor를 가리킨다.
            if (options.worldBoundary != 0 && Levels > 1) {
                for (usize index = 0; index < members.Size(); ++index) {
                    if (rng.Below(100) >= options.worldBoundary) {
                        continue;
                    }

                    const uint32 level = static_cast<uint32>(index / (ActorsPerLevel * 3));
                    const uint32 other = (level + 1 + rng.Below(Levels - 1)) % Levels;
                    if (!AddEdge(registry, members[index],
                                 actors[other * ActorsPerLevel + rng.Below(ActorsPerLevel)])) {
                        return 3;
                    }
                }
            }

            const uint64 built = platform::MonotonicNanoseconds();
            GraphBuilder builder{registry, rng, 0};
            Array<ObjectHandle> outside(memory);
            Array<ObjectHandle> outsideNodes(memory);
            Array<RootRef<Object>> pins(memory);
            if (!builder.CreateGroups(OutsideObjects, outside, outsideNodes, pins) ||
                !builder.Link(outsideNodes, outside, 4)) {
                return 3;
            }

            for (usize index = 0; index < outsideNodes.Size(); ++index) {
                if (rng.Below(100) < 5 &&
                    !AddEdge(registry, outsideNodes[index],
                             members[rng.Below(static_cast<uint32>(members.Size()))])) {
                    return 3;
                }
            }

            Array<RootRef<Object>> roots(memory);
            for (usize index = 0; index < pins.Size(); ++index) {
                if (index % GroupSize == 0) {
                    if (!roots.TryPushBack(Move(pins[index]))) {
                        return 3;
                    }
                } else {
                    pins[index].Reset();
                }
            }

            pins.Clear();
            const ObjectStats stats = registry.Stats();
            printf("  build levels %.1f ms, clusters %u, members %u\n",
                   Milliseconds(built - started), stats.clusters, stats.clusterMembers);
            if (!registry.Collect(access)) {
                return 4;
            }

            PrintMemory(memory, registry.Stats().liveObjects, registry.Stats());
            auto pick = [&](ObjectHandle& owner, ObjectHandle& target) noexcept {
                const uint32 index = rng.Below(static_cast<uint32>(actors.Size()));
                owner = actors[index];
                target =
                    actors[(index / ActorsPerLevel) * ActorsPerLevel + rng.Below(ActorsPerLevel)];
            };
            if (!MeasureEdgeWrites(registry, 200000, pick, memory) ||
                !MeasureGraph(registry, scheduler, options.workers, 3, memory)) {
                return 5;
            }

            // 일반 객체 churn: Level 밖 객체 10%만큼 쓰레기 묶음(Level 소속·일반 객체를 가리킨다).
            Array<GcReport> reports(memory);
            for (uint32 rep = 0; rep < 3; ++rep) {
                Array<ObjectHandle> garbage(memory);
                Array<ObjectHandle> garbageNodes(memory);
                Array<RootRef<Object>> garbagePins(memory);
                if (!builder.CreateGroups(OutsideObjects / 10, garbage, garbageNodes,
                                          garbagePins) ||
                    !builder.Link(garbageNodes, members, 4)) {
                    return 6;
                }

                garbagePins.Clear();
                auto collected = registry.Collect(access, &scheduler);
                if (!collected || !reports.TryPushBack(collected.Value())) {
                    return 6;
                }
            }

            char label[64];
            snprintf(label, sizeof(label), "churn %u parallel x%u", OutsideObjects / 10,
                     options.workers);
            PrintPhases(label, MedianOf(reports, memory));
            fflush(stdout);
            roots.Clear();
            levels.Clear();
            if (!registry.Shutdown(access)) {
                status = 7;
            }
        }

        if (!scheduler.Shutdown() || !types.Unregister(tokens.leaf) ||
            !types.Unregister(tokens.node) || !types.Unregister(tokens.package) ||
            !types.Shutdown()) {
            status = 7;
        }
    }

    fflush(stdout);
    return memory.Allocations() == 0 ? status : 8;
}

constexpr uint32 PermanentObjects = 100000;
constexpr uint32 NormalObjects = 1000000;

// 영구 영역: 영구 객체 100K(서로 평균 4개 간선) + 일반 객체 1M(평균 4개 간선, 그중 5%는 영구 객체를
// 하나 더 가리킨다). region이 거짓이면 같은 그래프를 일반 객체로 만든다(묶음 머리를 루트로 쥔다).
int RunPermanentScenario(bool region, const Options& options) noexcept {
    printf("\n== permanent region: permanent %u + normal %u, refs avg 4, region %s ==\n",
           PermanentObjects, NormalObjects, region ? "on" : "off");
    fflush(stdout);
    BenchAllocator memory;
    int status = 0;
    {
        TypeRegistry types(memory);
        TypeTokens tokens;
        if (!RegisterTypes(types, tokens)) {
            return 2;
        }

        WorkerScheduler scheduler(memory);
        if (!scheduler.Start({options.workers, 256, 16})) {
            return 2;
        }

        {
            RegistryOptions registryOptions;
            registryOptions.referenceTables = !options.noTables;
            registryOptions.permanentRegion = region;
            ObjectRegistry registry(memory, types, Guid{0x6766, region ? 1u : 0u}, registryOptions);
            const auto access = registry.OwnerAccess();
            Rng rng{0x2545f4914f6cdd1dULL};
            GraphBuilder builder{registry, rng, 0};
            Array<ObjectHandle> permanent(memory);
            Array<ObjectHandle> permanentNodes(memory);
            Array<RootRef<Object>> permanentPins(memory);
            if (!builder.CreateGroups(PermanentObjects, permanent, permanentNodes, permanentPins) ||
                !builder.Link(permanentNodes, permanent, 4) ||
                !registry.ClosePermanentRegion(access)) {
                return 3;
            }

            Array<RootRef<Object>> roots(memory);
            for (usize index = 0; index < permanentPins.Size(); ++index) {
                if (index % GroupSize == 0) {
                    if (!roots.TryPushBack(Move(permanentPins[index]))) {
                        return 3;
                    }
                } else {
                    permanentPins[index].Reset();
                }
            }

            permanentPins.Clear();
            Array<ObjectHandle> normal(memory);
            Array<ObjectHandle> normalNodes(memory);
            Array<RootRef<Object>> pins(memory);
            if (!builder.CreateGroups(NormalObjects, normal, normalNodes, pins) ||
                !builder.Link(normalNodes, normal, 4)) {
                return 3;
            }

            for (usize index = 0; index < normalNodes.Size(); ++index) {
                if (rng.Below(100) < 5 &&
                    !AddEdge(registry, normalNodes[index],
                             permanent[rng.Below(static_cast<uint32>(permanent.Size()))])) {
                    return 3;
                }
            }

            for (usize index = 0; index < pins.Size(); ++index) {
                if (index % GroupSize == 0) {
                    if (!roots.TryPushBack(Move(pins[index]))) {
                        return 3;
                    }
                } else {
                    pins[index].Reset();
                }
            }

            pins.Clear();
            if (!registry.Collect(access)) {
                return 4;
            }

            const ObjectStats stats = registry.Stats();
            printf("  live %u, clusters %u, permanent members %u\n", stats.liveObjects,
                   stats.clusters, stats.clusterMembers);
            if (!MeasureGraph(registry, scheduler, options.workers, 3, memory)) {
                return 5;
            }

            fflush(stdout);
            roots.Clear();
            if (!registry.Shutdown(access)) {
                status = 7;
            }
        }

        if (!scheduler.Shutdown() || !types.Unregister(tokens.leaf) ||
            !types.Unregister(tokens.node) || !types.Unregister(tokens.package) ||
            !types.Shutdown()) {
            status = 7;
        }
    }

    fflush(stdout);
    return memory.Allocations() == 0 ? status : 8;
}

// 클러스터 시나리오 전체다. 같은 실행 안에서 클러스터를 켠 경우와 끈 경우를 번갈아 잰다.
int RunClusterScenarios(const Options& options) noexcept {
    const uint32 crossPercents[] = {1, 10, 30};
    for (uint32 cross : crossPercents) {
        for (uint32 on = 0; on < 2; ++on) {
            if (const int status = RunPackageScenario(cross, on == 0, options); status != 0) {
                return status;
            }
        }
    }

    for (uint32 on = 0; on < 2; ++on) {
        if (const int status = RunWorldScenario(on == 0, options); status != 0) {
            return status;
        }
    }

    for (uint32 on = 0; on < 2; ++on) {
        if (const int status = RunPermanentScenario(on == 0, options); status != 0) {
            return status;
        }
    }

    return 0;
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

int RunClusterSoak(const Options& options) noexcept {
    SystemAllocator memory;
    GcClusterSoakReport report;
    {
        WorkerScheduler scheduler(memory);
        if (options.soakWorkers && !scheduler.Start({options.workers, 256, 16})) {
            return 2;
        }

        GcClusterSoakOptions soak;
        soak.seed = options.seed;
        soak.ticks = options.clusterSoakTicks;
        soak.maxObjects = options.soakObjects;
        soak.packages = options.packages;
        soak.scheduler = options.soakWorkers ? &scheduler : nullptr;
        // 진행 표시: 긴 실행이 멈췄는지 느린지 바깥에서 볼 수 있게 한다.
        soak.progressInterval = soak.ticks >= 10 ? soak.ticks / 10 : 1;
        soak.progress = +[](const GcClusterSoakReport& progress, void*) noexcept {
            printf("  progress tick %llu: created %llu, reclaimed %llu, cycles %llu\n",
                   static_cast<unsigned long long>(progress.ticks),
                   static_cast<unsigned long long>(progress.created),
                   static_cast<unsigned long long>(progress.reclaimed),
                   static_cast<unsigned long long>(progress.cyclesCompleted));
            fflush(stdout);
        };
        const uint64 started = platform::MonotonicNanoseconds();
        const bool passed = RunGcClusterSoak(memory, soak, report);
        printf("cluster soak (%s) seed %llu ticks %llu: %s (%.1f s)\n",
               options.packages ? "packages" : "world",
               static_cast<unsigned long long>(options.seed),
               static_cast<unsigned long long>(report.ticks), passed ? "PASS" : "FAIL",
               static_cast<double>(platform::MonotonicNanoseconds() - started) * 1e-9);
        printf("  created %llu, reclaimed %llu, cycles begun %llu / completed %llu, Collect %llu, "
               "steps %llu, verifications %llu, edge writes %llu (rejected %llu), destroys %llu, "
               "Discard %llu, flattened %llu\n",
               static_cast<unsigned long long>(report.created),
               static_cast<unsigned long long>(report.reclaimed),
               static_cast<unsigned long long>(report.cyclesBegun),
               static_cast<unsigned long long>(report.cyclesCompleted),
               static_cast<unsigned long long>(report.collects),
               static_cast<unsigned long long>(report.steps),
               static_cast<unsigned long long>(report.verifications),
               static_cast<unsigned long long>(report.edgeWrites),
               static_cast<unsigned long long>(report.rejectedWrites),
               static_cast<unsigned long long>(report.destroys),
               static_cast<unsigned long long>(report.discards),
               static_cast<unsigned long long>(report.flattened));
        if (report.failure != nullptr) {
            printf("  FAILURE at tick %llu (object %u, error %u): %s\n",
                   static_cast<unsigned long long>(report.failureTick), report.failureObject,
                   static_cast<unsigned>(report.failureError), report.failure);
        }

        if (options.soakWorkers && !scheduler.Shutdown()) {
            return 2;
        }

        if (!passed) {
            return 9;
        }
    }

    return memory.Stats().liveAllocations == 0 ? 0 : 8;
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
        } else if (platform::CStringEquals(argv[index], "--clusters")) {
            options.clusters = true;
        } else if (platform::CStringEquals(argv[index], "--packages")) {
            options.packages = true;
        } else if (platform::CStringEquals(argv[index], "--no-tables")) {
            options.noTables = true;
        } else if (platform::CStringEquals(argv[index], "--clusters-off")) {
            options.packageClustersOff = true;
        } else if (platform::CStringEquals(argv[index], "--count-work")) {
            options.countWork = true;
        } else if (platform::CStringEquals(argv[index], "--mark-only")) {
            options.markOnly = true;
        } else if (platform::CStringEquals(argv[index], "--write-latency")) {
            options.writeLatency = true;
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
            options.reps = static_cast<uint32>(value > 100000 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--workers")) {
            options.workers = static_cast<uint32>(value > 64 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--soak")) {
            options.soakTicks = value;
            ++index;
        } else if (platform::CStringEquals(argv[index], "--package-size")) {
            options.packageSize = static_cast<uint32>(
                value < MinPackageSize || value > PackageObjects ? DefaultPackageSize : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--incremental-reps")) {
            options.incrementalReps = static_cast<uint32>(value > 1000 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--write-rounds")) {
            options.writeRounds = static_cast<uint32>(value > 1000 ? 0 : value);
            ++index;
        } else if (platform::CStringEquals(argv[index], "--package-profile")) {
            options.packageProfile = static_cast<uint32>(value > 100 ? 100 : value);
            options.packageProfileSet = true;
            ++index;
        } else if (platform::CStringEquals(argv[index], "--world-boundary")) {
            options.worldBoundary = static_cast<uint32>(value > 100 ? 100 : value);
            options.worldBoundarySet = true;
            ++index;
        } else if (platform::CStringEquals(argv[index], "--cluster-soak")) {
            options.clusterSoakTicks = value;
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
    // --mark-only는 일반 그래프 시나리오(--objects)의 정지형 표식 시간만 잰다. 다른 시나리오에서는
    // 무시되고, 작업 계수(--count-work)는 간선마다 읽기를 더해 시간을 바꾸므로 함께 쓰지 않는다.
    if (options.markOnly && (options.objects == 0 || options.countWork || options.soakTicks != 0 ||
                             options.clusterSoakTicks != 0 || options.allocators ||
                             options.clusters || options.packageProfileSet)) {
        printf("--mark-only needs --objects N (general graph) and cannot be combined with "
               "--count-work or another scenario\n");
        return 1;
    }

    if (options.soakTicks != 0) {
        return RunSoak(options);
    }

    if (options.clusterSoakTicks != 0) {
        return RunClusterSoak(options);
    }

    if (options.allocators) {
        return RunAllocatorComparison(options);
    }

    if (options.clusters) {
        return RunClusterScenarios(options);
    }

    if (options.worldBoundarySet) {
        for (uint32 on = 0; on < 2; ++on) {
            if (const int status = RunWorldScenario(on == 0, options); status != 0) {
                return status;
            }
        }

        return 0;
    }

    if (options.packageProfileSet) {
        return RunPackageScenario(options.packageProfile, !options.packageClustersOff, options);
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
               "[--allocators] [--clusters] [--soak TICKS [--seed S] [--soak-objects N] "
               "[--soak-workers] [--soak-time-budget]] [--cluster-soak TICKS [--packages]] "
               "[--no-tables]\n");
        return 1;
    }

    printf("GcBench: workers %u, reference tables %s\n", options.workers,
           options.noTables ? "off" : "on");
    return Run(options);
}
