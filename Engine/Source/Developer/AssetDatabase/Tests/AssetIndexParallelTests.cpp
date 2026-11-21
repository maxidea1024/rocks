#include "../../../Runtime/AssetRuntime/Tests/AssetFixtureValues.h"
#include "AssetDatabaseTestSupport.h"
#include "AssetIndexCompare.h"
#include "EditorPackageSetTestSupport.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetRuntime/PackageTags.h>
#include <Platform/Atomics.h>
#include <Task/WorkerScheduler.h>

// 색인 병렬 읽기 시험이다(Docs/Design/AssetSystem.ko.md 7.1절 16 A2d2): 걷기는 호출 스레드, 파일
// 읽기는 scheduler worker와 나눠도 결과가 직렬과 같다(항목·진단 모든 칸 — 같은 id·Tags 손상·읽기
// 실패·중첩 디렉터리 섞음, worker 수 1·2·4·7, 캐시 재사용, 하위 트리 갱신, 시작하지 않은 scheduler,
// worker 안에서 부름). 할당 실패는 어느 스레드의 것이든 검색 전체 실패이거나(helper 제출 실패는
// 호출 스레드가 대신해) 직렬과 같은 결과다. 파일은 디스크(PhysicalFileSystem — 동시 호출 안전)다.
using namespace rocks;
namespace asset_index_parallel_tests_file {

using namespace rocks::asset_test;
using namespace rocks::asset_database_test;
using namespace rocks::editor_set_test;

namespace {

constexpr uint64 FirstGeneratedId = 5000;
constexpr uint64 BadTagsId = 4999;

PackageReadOptions ReadOptions() noexcept {
    PackageReadOptions options;
    options.expectedKind = PackageKind::Editor;
    options.engineVersion = TestEngineVersion;
    return options;
}

// n번째 시도의 할당을 실패시키는 스레드 안전 allocator다(시도 수는 원자 — 여러 worker가 함께
// 할당한다). 어느 스레드의 시도가 n번째인지는 실행마다 다를 수 있지만 단언은 그 순서에 기대지
// 않는다.
class AtomicFailAllocator final : public Allocator {
public:
    explicit AtomicFailAllocator(Allocator& inner) noexcept : inner_(inner) {
    }

    void FailOnAttempt(uint64 attempt) noexcept {
        platform::AtomicStore(&attempts_, uint64{0});
        platform::AtomicStore(&target_, attempt);
    }

    uint64 Attempts() const noexcept {
        return platform::AtomicLoad(&attempts_);
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        const uint64 target = platform::AtomicLoad(&target_);
        if (target != 0 && platform::AtomicFetchAdd(&attempts_, uint64{1}) + 1 == target) {
            return Result<void*>::Failure(ErrorCode::OutOfMemory);
        }

        return inner_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner_.Deallocate(address);
    }

private:
    Allocator& inner_;
    uint64 attempts_ = 0;
    uint64 target_ = 0;
};

// 디스크 프로젝트에 합성 패키지 count개(중첩 디렉터리 여럿)와 같은 id 둘·Tags 손상·잘린 파일을
// 쓴다.
bool MakeProject(AssetEnv& env, const TempProject& project, usize count) noexcept {
    ObjectRegistry& registry = *env.source;
    for (usize at = 0; at < count; ++at) {
        RootRef<Package> package;
        if (MakeAsset<TestAsset>(registry, FirstGeneratedId + at, package).IsNull()) {
            return false;
        }

        const Array<uint8> bytes =
            Write(env.allocator, registry, package.Handle(), EditorOptions());
        char name[64] = {};
        (void)snprintf(name, sizeof(name), "D%zu/E%zu/P%zu.rasset", at % 5, at % 3, at);
        if (bytes.IsEmpty() ||
            !WriteContentFile(env.allocator, project, StringView(name), bytes.View())) {
            return false;
        }

        // 같은 id(같은 경로 키가 아닌 다른 이름).
        if (at == 1 && !WriteContentFile(env.allocator, project, "Copy.rasset", bytes.View())) {
            return false;
        }

        // 잘린 파일(CorruptPackage).
        if (at == 2 && !WriteContentFile(env.allocator, project, "D9/Cut.rasset",
                                         Span<const uint8>(bytes.Data(), bytes.Size() / 2))) {
            return false;
        }
    }

    RootRef<Package> bad;
    if (MakeAsset<TestAsset>(registry, BadTagsId, bad).IsNull()) {
        return false;
    }

    const Array<uint8> broken = OneFieldRecord(env.allocator, PackageTagsSchema);
    PackageWriteOptions options = EditorOptions();
    options.tags = broken.View();
    const Array<uint8> badTags = Write(env.allocator, registry, bad.Handle(), options);
    return !badTags.IsEmpty() &&
           WriteContentFile(env.allocator, project, "D0/BadTags.rasset", badTags.View()) &&
           MakeContentDirectory(env.allocator, project, "Empty/Deeper");
}

} // namespace

// 병렬 = 직렬: worker 수 1·2·4·7, 캐시 재사용, 하위 트리 갱신(RefreshPaths), 시작하지 않은
// scheduler(호출 스레드 혼자), worker 안에서 부른 검색(호출 스레드 혼자 — worker가 다른 worker
// 작업을 기다리지 않는다).
TEST(AssetIndexParallel_MatchesSerial) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TempProject project("parallel-match");
    REQUIRE(project.made);
    REQUIRE(MakeProject(env, project, 300));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    platform::FileSystem& files = platform::PhysicalFileSystem();
    AssetIndex serial(env.allocator, layout);
    REQUIRE(serial.Rebuild(files, ReadOptions()));
    REQUIRE(serial.Count() == 302);
    CHECK(!serial.Diagnostics().IsEmpty());
    Array<uint8> cacheBytes(env.allocator);
    const AssetIndexCacheKey key = MakeAssetIndexCacheKey(ReadOptions());
    REQUIRE(EncodeAssetIndexCache(serial, key, cacheBytes));
    AssetIndexCache cache(env.allocator);
    REQUIRE(cache.Parse(cacheBytes.View(), key));
    AssetIndex cachedSerial(env.allocator, layout);
    REQUIRE(cachedSerial.Rebuild(files, ReadOptions(), &cache));

    const uint32 workerCounts[] = {1, 2, 4, 7};
    for (uint32 workers : workerCounts) {
        WorkerScheduler scheduler(env.allocator);
        WorkerSchedulerConfig config;
        config.workerCount = workers;
        REQUIRE(scheduler.Start(config));
        AssetIndex parallel(env.allocator, layout, &scheduler);
        REQUIRE(parallel.Rebuild(files, ReadOptions()));
        CHECK(SameIndex(serial, parallel));
        AssetIndex cached(env.allocator, layout, &scheduler);
        REQUIRE(cached.Rebuild(files, ReadOptions(), &cache));
        CHECK(SameIndex(cachedSerial, cached));

        // 하위 트리 갱신도 같다.
        const StringView trees[] = {"D1", "D9"};
        AssetIndexRefreshPaths paths;
        paths.subtrees = trees;
        REQUIRE(parallel.RefreshPaths(files, paths, ReadOptions()));
        CHECK(SameIndex(serial, parallel));
        REQUIRE(scheduler.Shutdown());
    }

    // 시작하지 않은 scheduler·worker 안의 검색은 호출 스레드 혼자 한다(결과 같음).
    WorkerScheduler idle(env.allocator);
    AssetIndex alone(env.allocator, layout, &idle);
    REQUIRE(alone.Rebuild(files, ReadOptions()));
    CHECK(SameIndex(serial, alone));
    WorkerScheduler scheduler(env.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 2;
    REQUIRE(scheduler.Start(config));
    AssetIndex inside(env.allocator, layout, &scheduler);
    bool rebuilt = false;
    auto body = Function<void(WorkerTaskContext&)>::TryCreate(
        env.allocator, [&](WorkerTaskContext&) noexcept {
            rebuilt = static_cast<bool>(inside.Rebuild(files, ReadOptions()));
        });
    REQUIRE(body);
    auto task = scheduler.Submit(Move(body.Value()));
    REQUIRE(task);
    REQUIRE(task.Value().Wait());
    CHECK(rebuilt && SameIndex(serial, inside));
    REQUIRE(scheduler.Shutdown());
}

// 할당 실패: 병렬 검색의 모든 할당 시도를 하나씩 실패시킨다. 검색이 실패하면 OutOfMemory이고
// 색인은 비고(Rebuild 계약), helper 제출의 할당 실패처럼 검색이 삼킨 실패면 결과는 직렬과 같다.
// 실패가 없어지면 직렬과 같다.
TEST(AssetIndexParallel_AllocationFailures) {
    AssetEnv env;
    REQUIRE(env.Ready());
    TempProject project("parallel-allocation");
    REQUIRE(project.made);
    REQUIRE(MakeProject(env, project, 24));
    ProjectLayout layout(env.allocator);
    REQUIRE(layout.Initialize(project.Root()));
    platform::FileSystem& files = platform::PhysicalFileSystem();
    AssetIndex serial(env.allocator, layout);
    REQUIRE(serial.Rebuild(files, ReadOptions()));
    WorkerScheduler scheduler(env.allocator);
    WorkerSchedulerConfig config;
    config.workerCount = 3;
    REQUIRE(scheduler.Start(config));
    AtomicFailAllocator failing(env.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        AssetIndex index(failing, layout, &scheduler);
        failing.FailOnAttempt(attempt);
        auto rebuilt = index.Rebuild(files, ReadOptions());
        const bool triggered = failing.Attempts() >= attempt;
        failing.FailOnAttempt(0);
        if (!triggered) {
            REQUIRE(rebuilt);
            CHECK(SameIndex(serial, index));
            break;
        }

        if (rebuilt) {
            // 검색이 삼킨 실패(helper 제출 — 호출 스레드가 대신 했다).
            CHECK(SameIndex(serial, index));
        } else {
            CHECK(rebuilt.Error() == ErrorCode::OutOfMemory && index.Count() == 0);
            ++failures;
        }
    }

    CHECK(failures > 0);
    REQUIRE(scheduler.Shutdown());
}

} // namespace asset_index_parallel_tests_file
