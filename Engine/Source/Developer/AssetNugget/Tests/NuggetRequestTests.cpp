#include "../../AssetDatabase/Tests/AssetDatabaseTestSupport.h"
#include "../../AssetDatabase/Tests/MemoryFileSystem.h"
#include "NuggetTestSupport.h"

#include <AssetNugget/NuggetRequest.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/ProcessProbe.h>
#include <TestSupport/Test.h>
#include <string.h>

// 에셋 너겟 요청 객체 시험이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 12, "A3c2 구현 세부"): 일괄
// Put·Get(순서·값 이동·빈 요청·다시 Start), 진행 중 Busy·취소(gate), 대기 Put byte 상한, 제출 실패
// (scheduler 시작 전·maxInFlight), required 결과, Shutdown(요청 없음·drain 0·drain 끝까지 — hook),
// scheduler Shutdown(Cancel)이 버린 항목(ticket), Fatal 셋(자식 프로세스).
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace nugget_request_tests_file {

using rocks::asset_database_test::MemoryFileSystem;
using rocks::asset_database_test::TempProject;
using rocks::nugget_test::HookFileSystem;
using rocks::nugget_test::NuggetGate;

namespace {

constexpr int64 BaseTicks = 639000000000000000ll;

class ManualClock final : public NuggetClock {
public:
    int64 NowTicks() noexcept override {
        return now;
    }

    int64 now = BaseTicks;
};

Sha256Digest Key(uint8 seed) noexcept {
    Sha256Digest key;
    for (usize index = 0; index < Sha256Digest::Bytes; ++index) {
        key.bytes[index] = static_cast<uint8>(seed * 13 + index);
    }

    return key;
}

Array<uint8> Pattern(Allocator& allocator, usize size, uint8 seed) noexcept {
    Array<uint8> bytes(allocator);
    CHECK(bytes.TryResize(size));
    for (usize index = 0; index < size; ++index) {
        bytes[index] = static_cast<uint8>(seed + index * 7);
    }

    return bytes;
}

// 값 하나짜리 Put 항목이다.
NuggetPutItem PutItem(Allocator& allocator, const Sha256Digest& key, usize size,
                      uint8 seed) noexcept {
    NuggetPutItem item(allocator);
    item.bucket = NuggetBucketTexture;
    item.key = key;
    NuggetValue value(allocator);
    value.bytes = Pattern(allocator, size, seed);
    CHECK(item.values.TryPushBack(Move(value)));
    return item;
}

bool Same(Span<const uint8> left, Span<const uint8> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 캐시 + scheduler 장면이다(hook 파일 시스템 — 실제 디스크).
struct RequestScene {
    explicit RequestScene(const char* tag, uint32 workers = 2, const NuggetCacheLimits& limits = {},
                          uint32 maxInFlight = 256) noexcept
        : project(tag), settings(allocator), cache(allocator, files, clock, limits),
          scheduler(allocator) {
        WorkerSchedulerConfig config;
        config.workerCount = workers;
        config.maxInFlight = maxInFlight;
        ready = project.made &&
                static_cast<bool>(settings.localPath.TryAssign(NuggetDefaultLocalPath)) &&
                static_cast<bool>(cache.Open(settings, project.Root())) &&
                static_cast<bool>(scheduler.Start(config));
    }

    ~RequestScene() noexcept {
        cache.Shutdown(platform::InfiniteTimeout);
        (void)scheduler.Shutdown();
    }

    // 동기 Put(준비).
    NuggetStatus Store(const Sha256Digest& key, usize size, uint8 seed) noexcept {
        NuggetPutItem item = PutItem(allocator, key, size, seed);
        return cache.Put(NuggetCachePut{item.bucket, item.key, item.values.View()}).status;
    }

    SystemAllocator allocator;
    TempProject project;
    ManualClock clock;
    HookFileSystem files;
    NuggetCacheSettings settings;
    NuggetCache cache;
    WorkerScheduler scheduler;
    bool ready = false;
};

bool WaitDone(const NuggetRequest& request) noexcept {
    auto waited = request.Handle().Wait(platform::InfiniteTimeout);
    return waited && waited.Value() == platform::WaitResult::Signaled;
}

} // namespace

// 일괄 Put·Get: Put 항목 값은 Start가 옮긴다, 결과는 항목 순서(Stored·Hit·Miss), 값 byte, Outcome
// 성공, Start 전 Results·Outcome StaleHandle, 빈 요청은 바로 완료, 끝난 객체로 다시 Start.
TEST(NuggetRequest_PutThenGet) {
    RequestScene scene("nugget-request-basic");
    REQUIRE(scene.ready);
    NuggetPutRequest put(scene.allocator);
    CHECK(put.Results().Error() == ErrorCode::StaleHandle &&
          put.Outcome().Error() == ErrorCode::StaleHandle && !put.Handle().IsValid());
    NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 300, 1),
                             PutItem(scene.allocator, Key(2), 400, 2),
                             PutItem(scene.allocator, Key(3), 500, 3)};
    // inputs·meta는 Start가 복사한다(원본은 Start 뒤 사라져도 된다).
    {
        NuggetInput input(scene.allocator);
        REQUIRE(input.bucket.TryAssign(NuggetBucketTexture));
        input.key = Key(40);
        input.digest = Key(41);
        const NuggetInput inputs[] = {Move(input)};
        const uint8 meta[] = {7, 8, 9};
        items[1].inputs = inputs;
        items[1].meta = meta;
        REQUIRE(put.Start(scene.cache, items, scene.scheduler));
    }

    CHECK(items[0].values.IsEmpty() && items[2].values.IsEmpty());
    REQUIRE(WaitDone(put));
    auto stored = put.Results();
    REQUIRE(stored && stored.Value().Size() == 3);
    for (const NuggetCachePutResult& result : stored.Value()) {
        CHECK(result.status == NuggetStatus::Stored && !result.failed);
    }

    CHECK(put.Outcome());

    NuggetGetRequest get(scene.allocator);
    const uint32 first[] = {0};
    const NuggetGetItem wanted[] = {
        {NuggetBucketTexture, Key(3)},
        {NuggetBucketTexture, Key(9)},
        {NuggetBucketTexture, Key(1), first},
        {NuggetBucketTexture, Key(2), {}, NuggetPolicy::Default | NuggetPolicy::SkipData}};
    REQUIRE(get.Start(scene.cache, wanted, scene.scheduler));
    REQUIRE(WaitDone(get));
    auto got = get.Results();
    REQUIRE(got && got.Value().Size() == 4);
    const Array<uint8> third = Pattern(scene.allocator, 500, 3);
    const Array<uint8> one = Pattern(scene.allocator, 300, 1);
    CHECK(got.Value()[0].status == NuggetStatus::Hit &&
          Same(got.Value()[0].values[0].bytes.View(), third.View()));
    CHECK(got.Value()[1].status == NuggetStatus::Miss);
    CHECK(got.Value()[2].status == NuggetStatus::Hit &&
          Same(got.Value()[2].values[0].bytes.View(), one.View()));
    CHECK(got.Value()[3].status == NuggetStatus::Hit && got.Value()[3].values.IsEmpty() &&
          got.Value()[3].record.inputs.Size() == 1 && got.Value()[3].record.meta.Size() == 3);
    CHECK(get.Outcome());

    // 두 요청이 겹친다: 먼저 낸 Put이 붙잡힌 동안 나중 Get이 끝난다(목록 중간 해제), 진행 중 Put
    // 다시 Start는 Busy.
    NuggetGate gate;
    scene.files.Arm(".tmp", platform::FileOpenMode::CreateNew, &NuggetGate::Hold, &gate);
    NuggetPutItem held[] = {PutItem(scene.allocator, Key(20), 100, 20)};
    REQUIRE(put.Start(scene.cache, held, scene.scheduler));
    REQUIRE(gate.entered.Wait() == platform::WaitResult::Signaled);
    CHECK(put.Start(scene.cache, held, scene.scheduler).Error() == ErrorCode::Busy);
    const NuggetGetItem quick[] = {{NuggetBucketTexture, Key(1)}};
    REQUIRE(get.Start(scene.cache, quick, scene.scheduler));
    REQUIRE(WaitDone(get));
    CHECK(get.Results().Value()[0].status == NuggetStatus::Hit);
    gate.release.Signal();
    REQUIRE(WaitDone(put));
    CHECK(put.Results().Value()[0].status == NuggetStatus::Stored);

    // 빈 요청, 다시 Start.
    REQUIRE(get.Start(scene.cache, Span<const NuggetGetItem>(), scene.scheduler));
    REQUIRE(WaitDone(get));
    CHECK(get.Results() && get.Results().Value().IsEmpty());
    const NuggetGetItem again[] = {{NuggetBucketTexture, Key(2)}};
    REQUIRE(get.Start(scene.cache, again, scene.scheduler));
    REQUIRE(WaitDone(get));
    CHECK(get.Results().Value()[0].status == NuggetStatus::Hit);
}

// 진행 중: 다시 Start Busy, Results·Outcome Busy, 취소하면 시작하지 않은 항목은 Cancelled이고
// 진행 중 항목은 끝난다(Get·Put).
TEST(NuggetRequest_BusyAndCancel) {
    RequestScene scene("nugget-request-cancel", 1);
    REQUIRE(scene.ready);
    REQUIRE(scene.Store(Key(1), 100, 1) == NuggetStatus::Stored);
    REQUIRE(scene.Store(Key(2), 100, 2) == NuggetStatus::Stored);
    // 디스크 읽기를 붙잡으려면 Memory에 없는 키가 필요하다(Store는 Memory에 넣는다) — 직접
    // 게시한다.
    String root(scene.allocator);
    REQUIRE(ResolveNuggetLocalPath(scene.settings, scene.project.Root(), root));
    {
        NuggetFileSystemBackend direct(scene.allocator, platform::PhysicalFileSystem(), root.View(),
                                       scene.clock);
        REQUIRE(direct.Open());
        const Array<uint8> bytes = Pattern(scene.allocator, 100, 5);
        const NuggetPutValue one[] = {{0, bytes.View()}};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, Key(5), one}).status ==
                NuggetStatus::Stored);
    }

    NuggetGate gate;
    scene.files.Arm(".rnug", platform::FileOpenMode::ReadExisting, &NuggetGate::Hold, &gate);
    NuggetGetRequest get(scene.allocator);
    const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(5)},
                                    {NuggetBucketTexture, Key(1)},
                                    {NuggetBucketTexture, Key(2)}};
    REQUIRE(get.Start(scene.cache, wanted, scene.scheduler));
    REQUIRE(gate.entered.Wait() == platform::WaitResult::Signaled);
    CHECK(get.Start(scene.cache, wanted, scene.scheduler).Error() == ErrorCode::Busy);
    CHECK(get.Results().Error() == ErrorCode::Busy && get.Outcome().Error() == ErrorCode::Busy);
    get.RequestCancel();
    get.RequestCancel();
    gate.release.Signal();
    REQUIRE(WaitDone(get));
    auto got = get.Results();
    REQUIRE(got);
    CHECK(got.Value()[0].status == NuggetStatus::Hit &&
          got.Value()[1].status == NuggetStatus::Cancelled &&
          got.Value()[2].status == NuggetStatus::Cancelled);

    // Put: 첫 항목이 임시 파일을 만들 때 붙잡는다.
    NuggetGate putGate;
    scene.files.Arm(".tmp", platform::FileOpenMode::CreateNew, &NuggetGate::Hold, &putGate);
    NuggetPutRequest put(scene.allocator);
    NuggetPutItem items[] = {PutItem(scene.allocator, Key(6), 100, 6),
                             PutItem(scene.allocator, Key(7), 100, 7)};
    REQUIRE(put.Start(scene.cache, items, scene.scheduler));
    REQUIRE(putGate.entered.Wait() == platform::WaitResult::Signaled);
    put.RequestCancel();
    putGate.release.Signal();
    REQUIRE(WaitDone(put));
    CHECK(put.Results().Value()[0].status == NuggetStatus::Stored &&
          put.Results().Value()[1].status == NuggetStatus::Cancelled && put.Outcome());
}

// 대기 Put byte 상한: Start가 예약한다 — 합이 상한을 넘는 항목은 Dropped(CapacityExceeded, 실패
// 아님), 상한보다 큰 항목 하나도 Dropped, 끝나면(실행·취소) 돌려준다.
TEST(NuggetRequest_PendingPutBytes) {
    NuggetCacheLimits limits;
    limits.pendingPutBytes = 1000;
    RequestScene scene("nugget-request-pending", 2, limits);
    REQUIRE(scene.ready);
    NuggetPutRequest put(scene.allocator);
    NuggetPutItem items[] = {
        PutItem(scene.allocator, Key(1), 600, 1), PutItem(scene.allocator, Key(2), 600, 2),
        PutItem(scene.allocator, Key(3), 300, 3), PutItem(scene.allocator, Key(4), 1001, 4)};
    REQUIRE(put.Start(scene.cache, items, scene.scheduler));
    REQUIRE(WaitDone(put));
    auto results = put.Results();
    REQUIRE(results);
    CHECK(results.Value()[0].status == NuggetStatus::Stored &&
          results.Value()[1].status == NuggetStatus::Dropped &&
          results.Value()[1].error == ErrorCode::CapacityExceeded && !results.Value()[1].failed &&
          results.Value()[2].status == NuggetStatus::Stored &&
          results.Value()[3].status == NuggetStatus::Dropped &&
          results.Value()[3].error == ErrorCode::CapacityExceeded);
    CHECK(put.Outcome());
    // 돌려받았다 — 상한 그대로인 항목을 낸다.
    NuggetPutItem full[] = {PutItem(scene.allocator, Key(5), 1000, 5)};
    REQUIRE(put.Start(scene.cache, full, scene.scheduler));
    REQUIRE(WaitDone(put));
    CHECK(put.Results().Value()[0].status == NuggetStatus::Stored);
    // 취소한 항목도 돌려준다.
    NuggetGate gate;
    scene.files.Arm(".tmp", platform::FileOpenMode::CreateNew, &NuggetGate::Hold, &gate);
    NuggetPutItem pair[] = {PutItem(scene.allocator, Key(6), 500, 6),
                            PutItem(scene.allocator, Key(7), 500, 7)};
    REQUIRE(put.Start(scene.cache, pair, scene.scheduler));
    REQUIRE(gate.entered.Wait() == platform::WaitResult::Signaled);
    put.RequestCancel();
    gate.release.Signal();
    REQUIRE(WaitDone(put));
    NuggetPutItem after[] = {PutItem(scene.allocator, Key(8), 1000, 8)};
    REQUIRE(put.Start(scene.cache, after, scene.scheduler));
    REQUIRE(WaitDone(put));
    CHECK(put.Results().Value()[0].status == NuggetStatus::Stored);
}

// 제출 실패: scheduler 시작 전은 Start 실패(값은 항목에 남는다), maxInFlight를 넘는 항목은 그
// 항목과 뒤 항목이 Dropped(그 오류 — 실패 아님)이고 낸 항목은 끝까지 간다(예약은 돌려준다).
TEST(NuggetRequest_SubmitFailures) {
    {
        RequestScene scene("nugget-request-nostart", 1);
        REQUIRE(scene.ready);
        REQUIRE(scene.scheduler.Shutdown());
        NuggetPutRequest put(scene.allocator);
        NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 100, 1)};
        auto started = put.Start(scene.cache, items, scene.scheduler);
        CHECK(!started && items[0].values.Size() == 1 && !put.Handle().IsValid());
        NuggetGetRequest get(scene.allocator);
        const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(1)}};
        CHECK(!get.Start(scene.cache, wanted, scene.scheduler));
    }

    NuggetCacheLimits limits;
    limits.pendingPutBytes = 1000;
    // 이벤트 1 + 항목 둘이 자리를 채운다(첫 항목은 gate에 붙잡혀 진행 중).
    RequestScene scene("nugget-request-inflight", 1, limits, 3);
    REQUIRE(scene.ready);
    NuggetGate gate;
    scene.files.Arm(".tmp", platform::FileOpenMode::CreateNew, &NuggetGate::Hold, &gate);
    NuggetPutRequest put(scene.allocator);
    NuggetPutItem items[] = {
        PutItem(scene.allocator, Key(1), 100, 1), PutItem(scene.allocator, Key(2), 100, 2),
        PutItem(scene.allocator, Key(3), 100, 3), PutItem(scene.allocator, Key(4), 100, 4)};
    REQUIRE(put.Start(scene.cache, items, scene.scheduler));
    gate.release.Signal();
    REQUIRE(WaitDone(put));
    auto results = put.Results();
    REQUIRE(results);
    CHECK(results.Value()[0].status == NuggetStatus::Stored &&
          results.Value()[1].status == NuggetStatus::Stored &&
          results.Value()[2].status == NuggetStatus::Dropped &&
          results.Value()[2].error == ErrorCode::CapacityExceeded &&
          results.Value()[3].status == NuggetStatus::Dropped &&
          results.Value()[3].error == ErrorCode::CapacityExceeded && !results.Value()[3].failed);
    // 예약을 돌려받았다.
    NuggetPutItem full[] = {PutItem(scene.allocator, Key(5), 1000, 5)};
    REQUIRE(put.Start(scene.cache, full, scene.scheduler));
    REQUIRE(WaitDone(put));
    CHECK(put.Results().Value()[0].status == NuggetStatus::Stored);
    // Get도 같다.
    NuggetGate getGate;
    scene.files.Arm(".rnug", platform::FileOpenMode::ReadExisting, &NuggetGate::Hold, &getGate);
    NuggetGetRequest get(scene.allocator);
    const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(9)},
                                    {NuggetBucketTexture, Key(10)},
                                    {NuggetBucketTexture, Key(11)},
                                    {NuggetBucketTexture, Key(12)}};
    REQUIRE(get.Start(scene.cache, wanted, scene.scheduler));
    getGate.release.Signal();
    REQUIRE(WaitDone(get));
    CHECK(get.Results().Value()[2].status == NuggetStatus::Dropped &&
          get.Results().Value()[2].error == ErrorCode::CapacityExceeded &&
          get.Results().Value()[3].status == NuggetStatus::Dropped);
}

// required: failed 항목이 있으면 Outcome DependencyFailed(Put 쓰기 실패·Get 읽기 실패), required가
// 아니면 성공, 실패 아닌 Dropped(CapacityExceeded)는 required여도 성공.
TEST(NuggetRequest_RequiredOutcome) {
    SystemAllocator allocator;
    TempProject project("nugget-request-required");
    REQUIRE(project.made);
    ManualClock clock;
    MemoryFileSystem memory(allocator, project.Root());
    REQUIRE(memory.Ready());
    WorkerScheduler scheduler(allocator);
    REQUIRE(scheduler.Start(WorkerSchedulerConfig{2}));
    const bool modes[] = {true, false};
    for (bool required : modes) {
        NuggetCacheSettings settings(allocator);
        REQUIRE(settings.localPath.TryAssign(required ? "Required" : "Optional"));
        settings.required = required;
        NuggetCacheLimits limits;
        limits.pendingPutBytes = 500;
        NuggetCache cache(allocator, memory, clock, limits);
        REQUIRE(cache.Open(settings, project.Root()));
        memory.faults.create = platform::FileStatus{platform::FileCode::AccessDenied, 5};
        {
            NuggetPutRequest put(allocator);
            NuggetPutItem items[] = {PutItem(allocator, Key(1), 100, 1)};
            REQUIRE(put.Start(cache, items, scheduler));
            REQUIRE(WaitDone(put));
            CHECK(put.Results().Value()[0].failed);
            CHECK(static_cast<bool>(put.Outcome()) == !required);
            CHECK(required ? put.Outcome().Error() == ErrorCode::DependencyFailed : true);
        }

        memory.faults.create = {};
        {
            NuggetPutRequest put(allocator);
            NuggetPutItem items[] = {PutItem(allocator, Key(2), 600, 2)};
            REQUIRE(put.Start(cache, items, scheduler));
            REQUIRE(WaitDone(put));
            CHECK(put.Results().Value()[0].status == NuggetStatus::Dropped && put.Outcome());
        }

        // 읽기 실패(Memory에 없는 키를 직접 게시하고 읽기 열기를 막는다).
        String root(allocator);
        REQUIRE(ResolveNuggetLocalPath(settings, project.Root(), root));
        NuggetFileSystemBackend direct(allocator, memory, root.View(), clock);
        REQUIRE(direct.Open());
        const Array<uint8> bytes = Pattern(allocator, 100, 3);
        const NuggetPutValue one[] = {{0, bytes.View()}};
        REQUIRE(direct.Put(NuggetBackendPut{NuggetBucketTexture, Key(3), one}).status ==
                NuggetStatus::Stored);
        memory.faults.openRead = platform::FileStatus{platform::FileCode::AccessDenied, 5};
        memory.faults.readSuffix = ".rnug";
        {
            NuggetGetRequest get(allocator);
            const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(3)}};
            REQUIRE(get.Start(cache, wanted, scheduler));
            REQUIRE(WaitDone(get));
            CHECK(get.Results().Value()[0].failed);
            CHECK(static_cast<bool>(get.Outcome()) == !required);
        }

        memory.faults.openRead = {};
        memory.faults.readSuffix = nullptr;
        cache.Shutdown(0);
    }

    REQUIRE(scheduler.Shutdown());
}

namespace {

// Shutdown을 부르는 스레드다.
struct ShutdownThread {
    static void Entry(void* context) noexcept {
        auto* self = static_cast<ShutdownThread*>(context);
        self->cache->Shutdown(self->drainMs);
    }

    NuggetCache* cache = nullptr;
    uint32 drainMs = 0;
};

// Shutdown 지점에서 gate를 놓는 hook이다.
struct ReleaseAt {
    static void Callback(void* context, NuggetCacheStep step) noexcept {
        auto* self = static_cast<ReleaseAt*>(context);
        if (step == self->step) {
            self->gate->release.Signal();
        }

        self->seen[static_cast<uint32>(step)] = true;
    }

    NuggetCacheStep step = NuggetCacheStep::AfterDrain;
    NuggetGate* gate = nullptr;
    bool seen[2] = {};
};

} // namespace

// Shutdown: 요청 없음(이후 Start는 모든 항목 Cancelled·바로 완료, 동기 Get·Put Cancelled, 두 번째
// 호출 무시), drain 0(진행 중 Put은 끝냄·시작 안 한 Put·Get Cancelled), drain 끝까지(Put은 모두
// 끝냄·Get Cancelled).
TEST(NuggetRequest_Shutdown) {
    {
        RequestScene scene("nugget-request-shutdown-idle");
        REQUIRE(scene.ready);
        scene.cache.Shutdown(0);
        CHECK(scene.cache.IsShutDown());
        scene.cache.Shutdown(0);
        NuggetPutRequest put(scene.allocator);
        NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 100, 1)};
        REQUIRE(put.Start(scene.cache, items, scene.scheduler));
        REQUIRE(WaitDone(put));
        CHECK(put.Results().Value()[0].status == NuggetStatus::Cancelled && put.Outcome());
        NuggetGetRequest get(scene.allocator);
        const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(1)}};
        REQUIRE(get.Start(scene.cache, wanted, scene.scheduler));
        REQUIRE(WaitDone(get));
        CHECK(get.Results().Value()[0].status == NuggetStatus::Cancelled);
        CHECK(scene.cache.Get(NuggetCacheGet{NuggetBucketTexture, Key(1)}).status ==
              NuggetStatus::Cancelled);
        NuggetPutItem direct = PutItem(scene.allocator, Key(2), 100, 2);
        CHECK(scene.cache.Put(NuggetCachePut{direct.bucket, direct.key, direct.values.View()})
                  .status == NuggetStatus::Cancelled);
    }

    struct Case {
        uint32 drainMs;
        NuggetCacheStep release;
        NuggetStatus secondPut;
    };
    // drain 1 ms: 진행 중 Put이 gate에 붙잡혀 기한이 지난다(gate는 AfterDrain에서 놓는다 — 결과는
    // 시간과 무관하다).
    const Case cases[] = {
        {0, NuggetCacheStep::AfterDrain, NuggetStatus::Cancelled},
        {1, NuggetCacheStep::AfterDrain, NuggetStatus::Cancelled},
        {platform::InfiniteTimeout, NuggetCacheStep::AfterCancelGets, NuggetStatus::Stored}};
    for (const Case& item : cases) {
        RequestScene scene(item.drainMs == 0   ? "nugget-request-shutdown-zero"
                           : item.drainMs == 1 ? "nugget-request-shutdown-short"
                                               : "nugget-request-shutdown-drain",
                           1);
        REQUIRE(scene.ready);
        NuggetGate gate;
        scene.files.Arm(".tmp", platform::FileOpenMode::CreateNew, &NuggetGate::Hold, &gate);
        NuggetPutRequest put(scene.allocator);
        NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 100, 1),
                                 PutItem(scene.allocator, Key(2), 100, 2)};
        REQUIRE(put.Start(scene.cache, items, scene.scheduler));
        REQUIRE(gate.entered.Wait() == platform::WaitResult::Signaled);
        NuggetGetRequest get(scene.allocator);
        const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(1)}};
        REQUIRE(get.Start(scene.cache, wanted, scene.scheduler));
        ReleaseAt hook;
        hook.step = item.release;
        hook.gate = &gate;
        scene.cache.SetHooks(NuggetCacheHooks{&ReleaseAt::Callback, &hook});
        ShutdownThread worker;
        worker.cache = &scene.cache;
        worker.drainMs = item.drainMs;
        platform::Thread thread;
        REQUIRE(thread.Start(&ShutdownThread::Entry, &worker) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        CHECK(hook.seen[0] && hook.seen[1]);
        // Shutdown이 돌아오면 모든 요청의 항목이 끝났다.
        REQUIRE(WaitDone(put) && WaitDone(get));
        CHECK(put.Results().Value()[0].status == NuggetStatus::Stored &&
              put.Results().Value()[1].status == item.secondPut &&
              get.Results().Value()[0].status == NuggetStatus::Cancelled);
    }
}

// scheduler Shutdown(Cancel)이 시작하지 않은 항목의 body를 버리면 capture의 ticket이 그 항목을
// Cancelled로 끝내고 캐시 진행 중 목록에서 빠진다(이후 캐시 Shutdown이 기다리지 않는다).
TEST(NuggetRequest_AbandonedItems) {
    RequestScene scene("nugget-request-abandoned", 1);
    REQUIRE(scene.ready);
    WorkerScheduler local(scene.allocator);
    REQUIRE(local.Start(WorkerSchedulerConfig{1}));
    // worker 하나를 취소 요청이 올 때까지 붙잡는다(결과는 순서와 무관하다 — 취소 요청을 보고만
    // 끝난다).
    platform::ManualResetEvent never;
    auto blocker = Function<void(WorkerTaskContext&)>::TryCreate(
        scene.allocator, [&never](WorkerTaskContext& context) noexcept {
            constexpr uint32 PollMs = 5;
            while (!context.IsCancellationRequested()) {
                (void)never.Wait(PollMs);
            }
        });
    REQUIRE(blocker);
    REQUIRE(local.Submit(Move(blocker.Value())));
    NuggetPutRequest put(scene.allocator);
    NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 100, 1),
                             PutItem(scene.allocator, Key(2), 100, 2)};
    REQUIRE(put.Start(scene.cache, items, local));
    REQUIRE(local.Shutdown(WorkerShutdownMode::Cancel));
    auto results = put.Results();
    REQUIRE(results);
    CHECK(results.Value()[0].status == NuggetStatus::Cancelled &&
          results.Value()[1].status == NuggetStatus::Cancelled);
    // 진행 중 목록에서 빠졌다 — 캐시 Shutdown은 바로 돌아온다.
    scene.cache.Shutdown(platform::InfiniteTimeout);
    CHECK(scene.cache.IsShutDown());
}

namespace {

// 요청을 소멸하는 쪽이 취소를 요청할 때까지 기다렸다가 놓아 주는 스레드다(취소 요청은 소멸자가
// 기다리기 직전에 건다 — 그 뒤에만 blocker를 놓는다).
struct ReleaseAfterCancel {
    static void Entry(void* context) noexcept {
        auto* self = static_cast<ReleaseAfterCancel*>(context);
        constexpr uint32 PollMs = 1;
        platform::ManualResetEvent never;
        while (!self->request->IsCancelRequested()) {
            (void)never.Wait(PollMs);
        }

        self->release->Signal();
    }

    const NuggetRequest* request = nullptr;
    platform::ManualResetEvent* release = nullptr;
};

} // namespace

// 진행 중 요청을 worker가 아닌 스레드에서 소멸하면 취소를 요청하고 완료를 기다린다(시작하지 않은
// 항목은 취소, 예약한 대기 Put byte는 돌려받는다).
TEST(NuggetRequest_DestroyWhileRunning) {
    NuggetCacheLimits limits;
    limits.pendingPutBytes = 1000;
    RequestScene scene("nugget-request-destroy", 1, limits);
    REQUIRE(scene.ready);
    platform::ManualResetEvent release;
    auto blocker = Function<void(WorkerTaskContext&)>::TryCreate(
        scene.allocator, [&release](WorkerTaskContext&) noexcept { (void)release.Wait(); });
    REQUIRE(blocker);
    REQUIRE(scene.scheduler.Submit(Move(blocker.Value())));
    auto request = UniquePtr<NuggetPutRequest>::TryMake(scene.allocator, scene.allocator);
    REQUIRE(request);
    NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 1000, 1)};
    REQUIRE(request.Value()->Start(scene.cache, items, scene.scheduler));
    CHECK(!request.Value()->IsCancelRequested());
    ReleaseAfterCancel helper;
    helper.request = request.Value().Get();
    helper.release = &release;
    platform::Thread thread;
    REQUIRE(thread.Start(&ReleaseAfterCancel::Entry, &helper) == platform::ThreadStatus::Success);
    request.Value().Reset();
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    // 예약을 돌려받았다.
    NuggetPutRequest after(scene.allocator);
    NuggetPutItem full[] = {PutItem(scene.allocator, Key(2), 1000, 2)};
    REQUIRE(after.Start(scene.cache, full, scene.scheduler));
    REQUIRE(WaitDone(after));
    CHECK(after.Results().Value()[0].status == NuggetStatus::Stored);
}

// 할당 실패(요청 allocator): Start의 결과 slot·입력 복사(bucket·값 선택·inputs·meta)·예약 표·항목
// capture — 복사 실패는 Start 실패(OutOfMemory, Put 값은 항목에 남는다), capture 실패는 그 항목과
// 뒤 항목 Dropped(OutOfMemory). 실패 없는 결과는 기준과 같다.
TEST(NuggetRequest_AllocationFailures) {
    RequestScene scene("nugget-request-oom");
    REQUIRE(scene.ready);
    test::NFailAllocator failing(scene.allocator);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        NuggetInput input(scene.allocator);
        REQUIRE(input.bucket.TryAssign(NuggetBucketTexture));
        input.key = Key(50);
        input.digest = Key(51);
        const NuggetInput inputs[] = {Move(input)};
        const uint8 meta[] = {1, 2};
        NuggetPutItem items[] = {PutItem(scene.allocator, Key(1), 100, 1),
                                 PutItem(scene.allocator, Key(2), 100, 2)};
        items[0].inputs = inputs;
        items[0].meta = meta;
        NuggetPutRequest put(failing);
        failing.FailOnAttempt(attempt);
        auto started = put.Start(scene.cache, items, scene.scheduler);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!started) {
            CHECK(started.Error() == ErrorCode::OutOfMemory && items[0].values.Size() == 1 &&
                  items[1].values.Size() == 1);
            ++failures;
            continue;
        }

        REQUIRE(WaitDone(put));
        auto results = put.Results();
        REQUIRE(results);
        if (!failed) {
            CHECK((results.Value()[0].status == NuggetStatus::Stored ||
                   results.Value()[0].status == NuggetStatus::Exists) &&
                  (results.Value()[1].status == NuggetStatus::Stored ||
                   results.Value()[1].status == NuggetStatus::Exists));
            break;
        }

        for (const NuggetCachePutResult& result : results.Value()) {
            CHECK(result.status == NuggetStatus::Stored || result.status == NuggetStatus::Exists ||
                  (result.status == NuggetStatus::Dropped &&
                   result.error == ErrorCode::OutOfMemory && !result.failed));
        }

        ++failures;
    }

    for (uint64 attempt = 1;; ++attempt) {
        const uint32 first[] = {0};
        const NuggetGetItem wanted[] = {{NuggetBucketTexture, Key(1), first},
                                        {NuggetBucketTexture, Key(2)}};
        NuggetGetRequest get(failing);
        failing.FailOnAttempt(attempt);
        auto started = get.Start(scene.cache, wanted, scene.scheduler);
        const bool failed = failing.Attempts() >= attempt;
        failing.DisableFailure();
        if (!started) {
            CHECK(started.Error() == ErrorCode::OutOfMemory);
            ++failures;
            continue;
        }

        REQUIRE(WaitDone(get));
        auto results = get.Results();
        REQUIRE(results);
        if (!failed) {
            CHECK(results.Value()[0].status == NuggetStatus::Hit &&
                  results.Value()[1].status == NuggetStatus::Hit);
            break;
        }

        for (const NuggetCacheGetResult& result : results.Value()) {
            CHECK(result.status == NuggetStatus::Hit || (result.status == NuggetStatus::Dropped &&
                                                         result.error == ErrorCode::OutOfMemory));
        }

        ++failures;
    }

    CHECK(failures >= 10);
}

namespace {

// 자식 쪽이다. Fatal이 나면 돌아오지 않는다. 돌아오면 검출이 빠졌다는 뜻이므로 0을 돌려 부모가
// 실패로 본다.
int RequestFatalProbe(int argc, const char* const* argv) noexcept {
    rocks::test::PrepareFatalTestProcess();
    constexpr int UsageExit = 20;
    if (argc < 4) {
        return UsageExit;
    }

    const char* scenario = argv[3];
    SystemAllocator allocator;
    ManualClock clock;
    // off backend — Fatal로 끝나면 정리가 돌지 않으므로 파일을 만들지 않는다.
    NuggetCacheSettings settings(allocator);
    settings.backend = NuggetBackendKind::Off;

    WorkerScheduler scheduler(allocator);
    if (!scheduler.Start(WorkerSchedulerConfig{1})) {
        return UsageExit;
    }

    struct Context {
        NuggetCache* cache;
        WorkerScheduler* scheduler;
        Allocator* allocator;
        bool shutdown;
    };
    auto onWorker = [](Context context) noexcept {
        // worker 하나뿐이라 이 요청의 항목은 이 body가 끝날 때까지 시작하지 못한다(진행 중).
        NuggetPutRequest put(*context.allocator);
        NuggetPutItem items[] = {PutItem(*context.allocator, Key(1), 10, 1)};
        (void)put.Start(*context.cache, items, *context.scheduler);
        if (context.shutdown) {
            context.cache->Shutdown(0);
        }
    };
    if (strcmp(scenario, "destroy-on-worker") == 0 || strcmp(scenario, "shutdown-on-worker") == 0) {
        NuggetCache cache(allocator, platform::PhysicalFileSystem(), clock);
        if (!cache.Open(settings, StringView())) {
            return UsageExit;
        }

        Context context{&cache, &scheduler, &allocator,
                        strcmp(scenario, "shutdown-on-worker") == 0};
        auto body = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [context, onWorker](WorkerTaskContext&) noexcept { onWorker(context); });
        if (!body) {
            return UsageExit;
        }

        auto submitted = scheduler.Submit(Move(body.Value()));
        if (!submitted) {
            return UsageExit;
        }

        (void)submitted.Value().Wait();
        return 0;
    }

    if (strcmp(scenario, "destroy-cache") == 0) {
        platform::ManualResetEvent never;
        auto blocker = Function<void(WorkerTaskContext&)>::TryCreate(
            allocator, [&never](WorkerTaskContext&) noexcept { (void)never.Wait(); });
        if (!blocker || !scheduler.Submit(Move(blocker.Value()))) {
            return UsageExit;
        }

        NuggetPutRequest put(allocator);
        {
            NuggetCache cache(allocator, platform::PhysicalFileSystem(), clock);
            if (!cache.Open(settings, StringView())) {
                return UsageExit;
            }

            NuggetPutItem items[] = {PutItem(allocator, Key(1), 10, 1)};
            if (!put.Start(cache, items, scheduler)) {
                return UsageExit;
            }
        }

        return 0;
    }

    return UsageExit;
}

const bool RequestFatalProbeRegistered =
    rocks::test::RegisterProcessProbe("nugget-request-fatal", &RequestFatalProbe);

struct ProbeRun {
    bool ran = false;
    bool abnormal = false;
    char error[4096] = {};
};

// 자식을 띄워 끝날 때까지 기다린다(시간 한도 없음 — 기계 부하로 결과가 바뀌지 않게).
ProbeRun RunProbe(const char* scenario) noexcept {
    ProbeRun result;
    char executable[4096];
    usize executableBytes = 0;
    if (!platform::GetExecutablePath(executable, sizeof(executable), executableBytes)) {
        return result;
    }

    const platform::ProcessArgument arguments[] = {
        {"--process-probe", 15}, {"nugget-request-fatal", 20}, {scenario, strlen(scenario)}};
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

// Fatal(자식 프로세스): 같은 scheduler worker에서 진행 중 요청 소멸, 진행 중 요청의 scheduler
// worker에서 Shutdown, 진행 중 요청이 있는 캐시 소멸 — 모두 비정상 종료하고 그 문구를 낸다.
TEST(NuggetRequest_Fatal) {
    if (!rocks::test::ProcessSpawnExpected()) {
        return;
    }

    REQUIRE(RequestFatalProbeRegistered);
    struct Case {
        const char* scenario;
        const char* message;
    };
    const Case cases[] = {
        {"destroy-on-worker", "NuggetRequest destroyed on a worker of its scheduler while running"},
        {"shutdown-on-worker", "NuggetCache::Shutdown called on a worker of a pending request"},
        {"destroy-cache", "NuggetCache destroyed with requests in flight"}};
    for (const Case& item : cases) {
        const ProbeRun run = RunProbe(item.scenario);
        CHECK(run.ran && run.abnormal && strstr(run.error, item.message) != nullptr);
    }
}

} // namespace nugget_request_tests_file
