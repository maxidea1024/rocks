#include <Core/Memory/SystemAllocator.h>
#include <Platform/Process.h>
#include <SpatialIndex/LooseOctree.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <string.h>

using namespace rocks;
using namespace rocks::spatial;

// LO11(일부): 읽기·변경 단계가 겹치면 platform::Fatal인지 자식 프로세스에서 확인한다(Docs/Design/
// SpatialIndex.ko.md 6.9절). 자식은 이 실행 파일을 "--process-probe spatial-index <시나리오>"로
// 다시 띄운 probe이고 오류 대화상자를 띄우지 않는다(PrepareFatalTestProcess). 순서는 수면이나 경쟁
// 운이 아니라 호출 구조로 만든다: visitor 안의 변경, 변경 중 할당 콜백 안의 읽기 범위 생성.
namespace rocks::test {
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace {

SpatialProxyDesc UnitProxy() noexcept;

// Allocate가 불리면(옥트리가 변경 함수 안에서 저장소를 늘릴 때) 같은 트리의 읽기 범위를 열거나
// (ArmScope) 같은 트리를 다시 변경한다(ArmMutation). 겹침을 호출 구조로 결정적으로 만든다.
class ScopeOpeningAllocator final : public Allocator {
public:
    explicit ScopeOpeningAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    void ArmScope(const LooseOctree& tree) noexcept {
        tree_ = &tree;
    }

    void ArmMutation(LooseOctree& tree) noexcept {
        mutated_ = &tree;
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        if (tree_ != nullptr) {
            SpatialReadScope read(*tree_);
        }
        if (mutated_ != nullptr) {
            LooseOctree* tree = mutated_;
            mutated_ = nullptr;
            (void)tree->TryAdd(UnitProxy());
        }
        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

private:
    Allocator& backing_;
    const LooseOctree* tree_ = nullptr;
    LooseOctree* mutated_ = nullptr;
};

SpatialProxyDesc UnitProxy() noexcept {
    SpatialProxyDesc desc;
    desc.bounds = Box<double>(Vec3d(-1, -1, -1), Vec3d(1, 1, 1));
    desc.filterMask = 1;
    return desc;
}

// 자식 쪽이다. Fatal이 나면 돌아오지 않는다. 돌아오면 검출이 빠졌다는 뜻이므로 0을 돌려 부모가
// 실패로 본다. 준비 실패는 0이 아닌 다른 값(20번대)으로 구분하되 부모는 메시지로 확인한다.
int SpatialIndexFatalProbe(int argc, char** argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    if (argc < 4) {
        return 20;
    }
    SystemAllocator system;
    ScopeOpeningAllocator allocator(system);
    auto created = LooseOctree::TryCreate(LooseOctreeConfig{}, allocator);
    if (!created) {
        return 21;
    }
    LooseOctree octree = Move(created.Value());
    if (strcmp(argv[3], "visitor-mutation") == 0) {
        auto added = octree.TryAdd(UnitProxy());
        if (!added) {
            return 22;
        }
        SpatialReadScope read(octree);
        const LooseOctreeReader reader = read.Reader();
        // visitor 안에서 같은 트리를 변경하는 것은 위반이다(재검색만 허용).
        (void)reader.QueryBox(UnitProxy().bounds, 1, [&](const SpatialProxyHit& hit) noexcept {
            (void)octree.TryRemove(hit.handle);
            return QueryControl::Continue;
        });
        return 0;
    }
    if (strcmp(argv[3], "scope-during-mutation") == 0) {
        // 첫 TryAdd가 레코드 저장소를 할당하는 순간 읽기 범위를 연다.
        allocator.ArmScope(octree);
        (void)octree.TryAdd(UnitProxy());
        return 0;
    }
    if (strcmp(argv[3], "mutation-overlap") == 0) {
        // 변경자끼리의 겹침: TryAdd가 할당하는 순간 같은 트리에 TryAdd를 다시 부른다.
        allocator.ArmMutation(octree);
        (void)octree.TryAdd(UnitProxy());
        return 0;
    }
    if (strcmp(argv[3], "move-with-scope") == 0) {
        SpatialReadScope read(octree);
        LooseOctree moved(Move(octree));
        return 0;
    }
    if (strcmp(argv[3], "assign-with-scope") == 0) {
        auto other = LooseOctree::TryCreate(LooseOctreeConfig{}, allocator);
        if (!other) {
            return 22;
        }
        SpatialReadScope read(octree);
        octree = Move(other.Value());
        return 0;
    }
    if (strcmp(argv[3], "destroy-with-scope") == 0) {
        // 범위보다 트리가 먼저 끝나도록 객체 수명을 직접 다룬다.
        alignas(LooseOctree) unsigned char storage[sizeof(LooseOctree)];
        LooseOctree* tree = InitializeAt<LooseOctree>(storage, Move(octree));
        SpatialReadScope read(*tree);
        DestroyAt(tree);
        return 0;
    }
    return 24;
}

const bool SpatialIndexProbeRegistered =
    rocks::test::RegisterProcessProbe("spatial-index", &SpatialIndexFatalProbe);

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
        {"--process-probe", 15}, {"spatial-index", 13}, {scenario, strlen(scenario)}};
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

} // namespace

TEST(LooseOctree_LO11_MutationInsideVisitorIsFatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }
    REQUIRE(SpatialIndexProbeRegistered);
    const ProbeRun run = RunProbe("visitor-mutation");
    REQUIRE(run.ran);
    CHECK(run.abnormal);
    CHECK(strstr(run.error, "LooseOctree mutation while a read scope is open") != nullptr);
}

TEST(LooseOctree_LO11_MutationOverlapIsFatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }
    REQUIRE(SpatialIndexProbeRegistered);
    const ProbeRun run = RunProbe("mutation-overlap");
    REQUIRE(run.ran);
    CHECK(run.abnormal);
    CHECK(strstr(run.error, "LooseOctree mutation overlaps another mutation") != nullptr);
}

// 읽기 범위가 열린 채 이동·대입·파괴하면 Fatal이다(reader가 옛 저장소를 가리키게 되므로).
TEST(LooseOctree_LO11_MoveAssignDestroyWithOpenScopeIsFatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }
    REQUIRE(SpatialIndexProbeRegistered);
    struct Case {
        const char* scenario;
        const char* message;
    };
    const Case cases[] = {
        {"move-with-scope", "LooseOctree moved while a read scope is open"},
        {"assign-with-scope", "LooseOctree overwritten while a read scope is open"},
        {"destroy-with-scope", "LooseOctree destroyed while a read scope is open"},
    };
    for (const Case& entry : cases) {
        const ProbeRun run = RunProbe(entry.scenario);
        REQUIRE(run.ran);
        CHECK(run.abnormal);
        CHECK(strstr(run.error, entry.message) != nullptr);
    }
}

TEST(LooseOctree_LO11_ReadScopeDuringMutationIsFatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }
    REQUIRE(SpatialIndexProbeRegistered);
    const ProbeRun run = RunProbe("scope-during-mutation");
    REQUIRE(run.ran);
    CHECK(run.abnormal);
    CHECK(strstr(run.error, "LooseOctree read scope opened during a mutation") != nullptr);
}

// 같은 스레드에서 읽기 범위를 겹쳐 열고(중첩 허용) 모두 닫은 뒤에는 변경할 수 있다. 위반 검출이
// 정상 경로를 막지 않는지 확인한다.
TEST(LooseOctree_LO11_NestedScopesThenMutation) {
    SystemAllocator allocator;
    auto created = LooseOctree::TryCreate(LooseOctreeConfig{}, allocator);
    REQUIRE(created);
    LooseOctree octree = Move(created.Value());
    REQUIRE(octree.TryAdd(UnitProxy()));
    {
        SpatialReadScope outer(octree);
        SpatialReadScope inner(octree);
        const LooseOctreeReader copy = inner.Reader();
        auto any = copy.AnyInBox(UnitProxy().bounds, 1);
        REQUIRE(any);
        CHECK(any.Value());
        CHECK(outer.Reader().Count() == 1);
    }
    REQUIRE(octree.TryAdd(UnitProxy()));
    SpatialReadScope read(octree);
    CHECK(read.Reader().Count() == 2);
}
