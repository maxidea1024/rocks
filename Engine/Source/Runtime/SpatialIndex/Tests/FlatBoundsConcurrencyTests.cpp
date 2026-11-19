#include "FlatBoundsTestSupport.h"

#include <Core/Memory/SystemAllocator.h>
#include <Platform/Atomics.h>
#include <Platform/Process.h>
#include <Task/ParallelFor.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/ProcessProbe.h>
#include <string.h>

using namespace rocks;
namespace flat_bounds_concurrency_tests_file {

using namespace rocks::spatial;
using namespace rocks::spatial::flattest;
} // namespace flat_bounds_concurrency_tests_file

// FB08: 읽기·변경 단계가 겹치면 platform::Fatal인지 자식 프로세스에서 확인하고(Docs/Design/
// FlatBounds.ko.md 8절), 여러 스레드의 동시 질의가 단독 질의와 같은지 본다. 자식은 이 실행 파일을
// "--process-probe flat-bounds <시나리오>"로 다시 띄운 probe이고 오류 대화상자를 띄우지 않는다.
// 순서는 수면이나 경쟁 운이 아니라 호출 구조로 만든다.
namespace rocks::test {
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace flat_bounds_concurrency_tests_file {

namespace {

FlatBoundsDesc UnitElement() noexcept {
    FlatBoundsDesc desc;
    desc.bounds = Box<double>(Vec3d(-1, -1, -1), Vec3d(1, 1, 1));
    desc.filterMask = 1;
    return desc;
}

// Allocate가 불리면(집합이 변경 함수 안에서 저장소를 늘릴 때) 같은 집합의 읽기 범위를 연다.
class ScopeOpeningAllocator final : public Allocator {
public:
    explicit ScopeOpeningAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    void Arm(const FlatBoundsSet& set) noexcept {
        set_ = &set;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (set_ != nullptr) {
            FlatBoundsReadScope read(*set_);
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
    const FlatBoundsSet* set_ = nullptr;
};

// 자식 쪽이다. Fatal이 나면 돌아오지 않는다. 돌아오면 검출이 빠졌다는 뜻이므로 0을 돌려 부모가
// 실패로 본다.
int FlatBoundsFatalProbe(int argc, char** argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    if (argc < 4) {
        return 20;
    }
    SystemAllocator system;
    ScopeOpeningAllocator allocator(system);
    auto created = FlatBoundsSet::TryCreate(FlatBoundsConfig{}, allocator);
    if (!created) {
        return 21;
    }
    FlatBoundsSet set = Move(created.Value());
    if (strcmp(argv[3], "mutation-in-scope") == 0) {
        FlatBoundsReadScope read(set);
        (void)set.TryAdd(UnitElement());
        return 0;
    }
    if (strcmp(argv[3], "visitor-mutation") == 0) {
        auto added = set.TryAdd(UnitElement());
        if (!added) {
            return 22;
        }
        FlatBoundsReadScope read(set);
        const FlatBoundsReader reader = read.Reader();
        uint64 words[4] = {};
        FlatVisibility out{Span<uint64>(words, 4), 0};
        FlatBoundsView view;
        const Plane<double> planes[] = {Plane<double>(Vec3d(1, 0, 0), -10.0)};
        view.volume = VolumeOf(planes);
        view.filterMask = 1;
        if (!reader.QueryViews(Span<const FlatBoundsView>(&view, 1), Span<FlatVisibility>(&out, 1),
                               nullptr, system)) {
            return 23;
        }
        (void)reader.ForEachVisible(out, [&](const FlatVisibleElement& element) noexcept {
            (void)set.TryRemove(element.handle);
            return QueryControl::Continue;
        });
        return 0;
    }
    if (strcmp(argv[3], "scope-during-mutation") == 0) {
        allocator.Arm(set);
        (void)set.TryAdd(UnitElement());
        return 0;
    }
    if (strcmp(argv[3], "move-with-scope") == 0) {
        FlatBoundsReadScope read(set);
        FlatBoundsSet moved(Move(set));
        return 0;
    }
    if (strcmp(argv[3], "destroy-with-scope") == 0) {
        alignas(FlatBoundsSet) unsigned char storage[sizeof(FlatBoundsSet)];
        FlatBoundsSet* owned = InitializeAt<FlatBoundsSet>(storage, Move(set));
        FlatBoundsReadScope read(*owned);
        DestroyAt(owned);
        return 0;
    }
    return 24;
}

const bool FlatBoundsProbeRegistered =
    rocks::test::RegisterProcessProbe("flat-bounds", &FlatBoundsFatalProbe);

struct ProbeRun {
    bool ran = false;
    bool abnormal = false;
    char error[4096] = {};
};

// 자식을 띄워 끝날 때까지 기다린다. 기다림에 시간 한도를 두지 않는다(기계 부하로 결과가 바뀌지
// 않게).
ProbeRun RunProbe(const char* scenario) noexcept {
    ProbeRun result;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return result;
    }
    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"flat-bounds", 11}, {scenario, strlen(scenario)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 3;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    platform::ProcessRunOptions options;
    options.error = result.error;
    options.errorCapacity = sizeof(result.error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    if (!run.status || run.timedOut) {
        return result;
    }
    result.ran = true;
    result
        .error[run.errorBytes < sizeof(result.error) ? run.errorBytes : sizeof(result.error) - 1] =
        '\0';
    result.abnormal = run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0;
    return result;
}

bool ProbeFails(const char* scenario, const char* message) noexcept {
    const ProbeRun run = RunProbe(scenario);
    return run.ran && run.abnormal && strstr(run.error, message) != nullptr;
}

} // namespace

TEST(FlatBounds_FB08_PhaseOverlapIsFatal) {
    CHECK(ProbeFails("mutation-in-scope", "FlatBoundsSet mutation while a read scope is open"));
    CHECK(ProbeFails("visitor-mutation", "FlatBoundsSet mutation while a read scope is open"));
    CHECK(ProbeFails("scope-during-mutation", "FlatBoundsSet read scope opened during a mutation"));
    CHECK(ProbeFails("move-with-scope", "FlatBoundsSet moved while a read scope is open"));
    CHECK(ProbeFails("destroy-with-scope", "FlatBoundsSet destroyed while a read scope is open"));
}

// 여러 worker가 같은 읽기 범위 안에서 서로 다른 뷰를 동시에 질의해도(각자 단독 실행) 호출 스레드
// 혼자의 결과와 비트 단위로 같다.
TEST(FlatBounds_FB08_ConcurrentQueriesMatchSequential) {
    SystemAllocator allocator;
    auto created = FlatBoundsSet::TryCreate(FlatBoundsConfig{}, allocator);
    REQUIRE(created);
    FlatBoundsSet set = Move(created.Value());
    RandomStream random(0xFB08u ^ test::CurrentSeed());
    for (uint32 index = 0; index < 20000; ++index) {
        FlatBoundsDesc desc;
        const Vec3d center(Uniform(random, -6.0e5, 6.0e5), Uniform(random, -6.0e5, 6.0e5),
                           Uniform(random, -2.0e4, 2.0e4));
        desc.bounds = Box<double>(center - Vec3d::Splat(50.0), center + Vec3d::Splat(50.0));
        desc.filterMask = 1;
        desc.maxDrawDistance = random.NextBool() ? 0.0f : 2.0e5f;
        REQUIRE(set.TryAdd(desc));
    }
    constexpr uint32 Views = 32;
    FlatBoundsView views[Views];
    for (FlatBoundsView& view : views) {
        const Vec3d apex(Uniform(random, -5.0e5, 5.0e5), Uniform(random, -5.0e5, 5.0e5), 0.0);
        view.volume = MakeFrustum(apex, RandomDirection(random), 10.0, 3.0e5);
        view.distanceOrigin = apex;
        view.filterMask = 1;
    }
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 4;
    REQUIRE(scheduler.Start(config));
    FlatBoundsReadScope read(set);
    const FlatBoundsReader reader = read.Reader();
    const uint32 wordCount = reader.WordCount();
    Array<uint64> sequential(allocator);
    Array<uint64> concurrent(allocator);
    REQUIRE(sequential.TryResize(static_cast<usize>(wordCount) * Views));
    REQUIRE(concurrent.TryResize(static_cast<usize>(wordCount) * Views));
    SystemAllocator scratch;
    for (uint32 index = 0; index < Views; ++index) {
        FlatVisibility out{Span<uint64>(sequential.Data() + index * wordCount, wordCount), 0};
        REQUIRE(reader.QueryViews(Span<const FlatBoundsView>(&views[index], 1),
                                  Span<FlatVisibility>(&out, 1), nullptr, scratch));
    }
    uint32 failures = 0;
    (void)ParallelFor(scheduler, scratch, Views, [&](usize begin, usize end) noexcept {
        for (usize index = begin; index < end; ++index) {
            FlatVisibility out{Span<uint64>(concurrent.Data() + index * wordCount, wordCount), 0};
            // worker 안에서 scheduler를 넘겨도 ParallelFor 계약대로 그 스레드가 혼자 처리한다.
            if (!reader.QueryViews(Span<const FlatBoundsView>(&views[index], 1),
                                   Span<FlatVisibility>(&out, 1), &scheduler, scratch)) {
                platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&failures, 1u);
            }
        }
    });
    CHECK(failures == 0);
    uint32 mismatches = 0;
    for (usize word = 0; word < sequential.Size(); ++word) {
        mismatches += sequential[word] != concurrent[word] ? 1u : 0u;
    }
    CHECK(mismatches == 0);
    REQUIRE(scheduler.Shutdown(WorkerShutdownMode::Drain));
}
} // namespace flat_bounds_concurrency_tests_file
