#include "LoaderTestSupport.h"

#include <AssetTestFixtures/TestAssets.generated.h>
#include <Core/ScopeExit.h>
#include <Platform/AsyncFile.h>
#include <Platform/Clock.h>
#include <Platform/File.h>
#include <Platform/Process.h>
#include <Platform/Thread.h>
#include <Task/WorkerScheduler.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/NFailAllocator.h>

#include <stdio.h>
#include <string.h>

// 로더의 실제 경계 시험이다(Docs/Design/AssetSystem.ko.md 11.6절 4·5·7·11): 디스크 파일을
// PlatformBase AsyncReadFile로 읽는 기본 IO(조각 읽기·파일 없음·크기 다름)와 WorkerScheduler
// worker로 끝까지 로드, 동기 대기 추적(관찰자·횟수·프레임 안 금지), CancelToken 계약. 스레드가
// 실제로 돌지만 단언은 결과만 본다(완료 순서와 무관하다).
using namespace rocks;

namespace rocks::test {
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept;
} // namespace rocks::test

namespace loader_io_tests_file {

using namespace rocks::asset_test;

namespace {

// 동기 대기 기록을 모으는 관찰자다.
struct RecordingObserver final : AssetLoaderObserver {
    void OnSyncWait(const AssetLoaderSyncWait& wait) noexcept override {
        ++waits;
        lastSite = wait.site;
        lastAsset = wait.asset;
    }

    uint32 waits = 0;
    const char* lastSite = nullptr;
    AssetId lastAsset{};
};

bool WriteWholeFile(StringView path, Span<const uint8> bytes) noexcept {
    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::CreateAlways)) {
        return false;
    }

    auto written = file.WriteAll(bytes.Data(), bytes.Size());
    return written.status && written.bytes == bytes.Size() && file.Close();
}

// path의 부모 디렉터리를 만든다.
bool MakeParent(StringView path) noexcept {
    usize slash = path.ByteCount();
    while (slash > 0 && path.Data()[slash - 1] != '/') {
        --slash;
    }

    if (slash <= 1) {
        return true;
    }

    return static_cast<bool>(
        platform::MakeDirectoryTree(platform::PhysicalFileSystem(), path.Data(), slash - 1));
}

bool InFrameAlways(void*) noexcept {
    return true;
}

bool InFrameNever(void*) noexcept {
    return false;
}

} // namespace

// 디스크 로드(11.6절 4·5): 장면(1 → 2(bulk, Zstd), 3 ↔ 4, 1 → 3)을 임시 디렉터리에 쓰고
// AsyncCookedPackageIo(작은 조각 — 여러 번 나눠 읽는다) + WorkerScheduler worker로 WaitFor
// 로드한다. 파일이 없으면 NotFound / MissingFile, 디스크 파일 크기가 매니페스트와 다르면
// PackageBytes 손상이다. WaitFor마다 관찰자가 호출 지점·대상 에셋을 받고 상태 조회가 횟수를 센다.
// "프레임 실행 중"이면 WaitFor는 WrongContext다.
TEST(AssetLoader_LoadsFromDiskWithWorkers) {
    AssetEnv env;
    REQUIRE(env.Ready());
    // 결정적 이름(시험 이름 + 프로세스 번호)이다.
    char root[96] = {};
    const int count = snprintf(root, sizeof(root), "rocks-loader-disk-%u",
                               static_cast<unsigned>(platform::CurrentProcessId()));
    REQUIRE(count > 0 && static_cast<usize>(count) < sizeof(root));
    const StringView rootView(root, static_cast<usize>(count));
    REQUIRE(platform::MakeDirectoryTree(platform::PhysicalFileSystem(), root,
                                        static_cast<usize>(count)));
    auto removeRoot = MakeScopeExit([&]() noexcept {
        CHECK(platform::DeleteDirectoryTree(platform::PhysicalFileSystem(), root,
                                            static_cast<usize>(count)));
    });
    LoaderRig rig(env);
    rig.scene.root = rootView;
    rig.scene.Add(1);
    rig.scene.Add(2, 5000);
    rig.scene.Add(3);
    rig.scene.Add(4);
    rig.scene.Add(5);
    rig.scene.Add(6);
    // 결과 주입·짧은 읽기용 독립 패키지(크기 조회·조각 제출·완료 실패, 짧게 읽힘)다.
    for (uint64 id = 7; id <= 10; ++id) {
        rig.scene.Add(id, 3000);
    }

    rig.scene.Hard(1, 2);
    rig.scene.Hard(1, 3);
    rig.scene.Hard(3, 4);
    rig.scene.Hard(4, 3);
    REQUIRE(rig.Cook());
    for (usize index = 0; index < rig.scene.FileCount(); ++index) {
        const StringView path = rig.scene.PathAt(index);
        // 5는 쓰지 않는다(없는 파일), 6은 한 byte 더 쓴다(크기 다름).
        if (path == rig.scene.PathOf(5)) {
            continue;
        }

        REQUIRE(MakeParent(path));
        Array<uint8> bytes = Copy(env.allocator, rig.scene.FileAt(index).View());
        if (path == rig.scene.PathOf(6)) {
            REQUIRE(bytes.TryPushBack(0));
        }

        REQUIRE(WriteWholeFile(path, bytes.View()));
    }

    platform::AsyncIoContext io;
    REQUIRE(io.Start(platform::DefaultAsyncIoBackend()));
    WorkerScheduler scheduler(env.allocator);
    WorkerSchedulerConfig workers;
    workers.workerCount = 2;
    REQUIRE(scheduler.Start(workers));
    {
        constexpr uint64 SmallChunk = 777;
        AsyncCookedPackageIo files(env.allocator, io, SmallChunk);
        WorkerSchedulerLoaderWorker worker(scheduler, env.allocator);
        RecordingObserver observer;
        rig.config.io = &files;
        rig.config.worker = &worker;
        rig.config.observer = &observer;
        REQUIRE(rig.Start());
        const AssetLoadHandle chain = rig.Request(1);
        auto waited = rig.loader->WaitFor(chain, "LoaderIoTests");
        REQUIRE(waited);
        CHECK(waited.Value().state == AssetLoadState::Succeeded);
        CHECK(rig.IsLoaded(1) && rig.IsLoaded(2) && rig.IsLoaded(3) && rig.IsLoaded(4));
        CHECK(observer.waits == 1 && observer.lastAsset == FixedId(1) &&
              StringView(observer.lastSite) == StringView("LoaderIoTests"));
        // bulk가 조각 읽기를 지나 그대로다.
        TestAsset* two = Pointer<TestAsset>(*env.target, rig.AssetOf(2));
        REQUIRE(two != nullptr);
        auto package = rig.loader->FindPackage(FixedId(2));
        REQUIRE(package);
        Package* pointer = Pointer<Package>(*env.target, package.Value());
        REQUIRE(pointer != nullptr);
        auto bulk = pointer->BulkBytes(two->pixels);
        REQUIRE(bulk);
        CHECK(bulk.Value().Size() == 5000);

        const AssetLoadHandle missing = rig.Request(5);
        auto absent = rig.loader->WaitFor(missing, "LoaderIoTests.Missing");
        REQUIRE(absent);
        CHECK(absent.Value().error == ErrorCode::NotFound &&
              absent.Value().diagnostic.code == AssetDiagnosticCode::MissingFile);
        const AssetLoadHandle longer = rig.Request(6);
        auto sized = rig.loader->WaitFor(longer, "LoaderIoTests.Size");
        REQUIRE(sized);
        CHECK(sized.Value().diagnostic.code == AssetDiagnosticCode::CorruptPackage &&
              sized.Value().diagnostic.section ==
                  static_cast<uint32>(CookedManifestField::PackageBytes));
        // 이미 끝난 요청의 WaitFor도 기록한다.
        auto done = rig.loader->WaitFor(chain, "LoaderIoTests.Done");
        REQUIRE(done && done.Value().state == AssetLoadState::Succeeded);
        AssetLoaderSnapshot snapshot(env.allocator);
        REQUIRE(rig.loader->Describe(snapshot));
        CHECK(snapshot.syncWaits == 4 && observer.waits == 4);
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();

        // 프레임 밖이면(판정 함수가 거짓) 관찰자 없이도 기다린다. 조각 크기 0은 1 byte 조각이다.
        AsyncCookedPackageIo tiny(env.allocator, io, 0);
        rig.config.io = &tiny;
        rig.config.observer = nullptr;
        rig.config.inFrame = InFrameNever;
        REQUIRE(rig.Start());
        const AssetLoadHandle outside = rig.Request(2);
        auto outsideWait = rig.loader->WaitFor(outside, "LoaderIoTests.Outside");
        REQUIRE(outsideWait && outsideWait.Value().state == AssetLoadState::Succeeded);
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();

        // IO 요청 상태 할당이 실패하면 그 패키지 요청이 그 오류로 실패한다.
        test::NFailAllocator failing(env.allocator);
        AsyncCookedPackageIo starved(failing, io);
        rig.config.io = &starved;
        REQUIRE(rig.Start());
        failing.FailOnAttempt(1);
        const AssetLoadHandle refusedIo = rig.Request(2);
        auto refusedWait = rig.loader->WaitFor(refusedIo, "LoaderIoTests.Starved");
        failing.DisableFailure();
        REQUIRE(refusedWait);
        CHECK(refusedWait.Value().error == ErrorCode::OutOfMemory);
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
        rig.config.io = &files;

        // 기본 IO의 OS 오류 경로(PlatformBase 결과 주입): 크기 조회 실패(7), 둘째 조각 제출
        // 실패(8), 첫 조각 완료 실패(9) — 모두 진단 None과 FileErrorCode 대응
        // 오류(DependencyFailed)다.
        const uint64 half = rig.scene.EntryOf(8).packageBytes / 2 + 1;
        AsyncCookedPackageIo halves(env.allocator, io, half);
        rig.config.io = &halves;
        REQUIRE(rig.Start());
#if !ROCKS_BUILD_RELEASE
        const platform::FileStatus osFailure{platform::FileCode::SystemError, 0};
        struct FaultCase {
            uint64 id;
            platform::AsyncIoFaultPoint point;
            uint32 skip;
        };
        const FaultCase faults[] = {{7, platform::AsyncIoFaultPoint::Size, 0},
                                    {8, platform::AsyncIoFaultPoint::Submit, 1},
                                    {9, platform::AsyncIoFaultPoint::Completion, 0}};
        for (const FaultCase& fault : faults) {
            io.InjectFault({fault.point, osFailure, fault.skip});
            const AssetLoadHandle handle = rig.Request(fault.id);
            auto waited = rig.loader->WaitFor(handle, "LoaderIoTests.Fault");
            REQUIRE(waited);
            CHECK(waited.Value().state == AssetLoadState::Failed &&
                  waited.Value().error == ErrorCode::DependencyFailed &&
                  waited.Value().diagnostic.code == AssetDiagnosticCode::None &&
                  waited.Value().diagnostic.asset == FixedId(fault.id));
        }
#endif

        // 짧게 읽힘(11.6절 4): 완료 게시를 보류한 채 Begin(크기 확인·첫 조각 제출)까지 진행하고
        // 파일을 1/4로 다시 쓴 뒤 재개하면, 남은 조각이 짧게 읽혀 InvalidArgument(진단 None)다.
        const StringView tenPath = rig.scene.PathOf(10);
        // 조각이 둘 이상이어야 남은 조각이 줄어든 파일을 읽는다(결정적).
        REQUIRE(rig.scene.EntryOf(10).packageBytes > half);
        io.HoldCompletions();
        const AssetLoadHandle shortRead = rig.Request(10);
        REQUIRE(rig.loader->Pump());
        Array<uint8>& tenFile = rig.scene.FileOf(10);
        REQUIRE(WriteWholeFile(tenPath, tenFile.View().Slice(0, tenFile.Size() / 4)));
        REQUIRE(io.ResumeCompletions());
        auto shortWait = rig.loader->WaitFor(shortRead, "LoaderIoTests.Short");
        REQUIRE(shortWait);
        CHECK(shortWait.Value().state == AssetLoadState::Failed &&
              shortWait.Value().error == ErrorCode::InvalidArgument &&
              shortWait.Value().diagnostic.code == AssetDiagnosticCode::None &&
              shortWait.Value().diagnostic.asset == FixedId(10));
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
        rig.config.io = &files;

        // 프레임 실행 중에는 동기 대기를 거부한다.
        rig.config.inFrame = InFrameAlways;
        REQUIRE(rig.Start());
        const AssetLoadHandle refused = rig.Request(1);
        CHECK(rig.loader->WaitFor(refused, "LoaderIoTests.Frame").Error() ==
              ErrorCode::WrongContext);
        REQUIRE(rig.loader->Shutdown());
        rig.loader.Reset();
    }

    REQUIRE(scheduler.Shutdown());
    REQUIRE(io.Shutdown());
}

namespace {

// 자식 프로세스 probe다: 초기화한 로더를 다른 스레드에서 파괴하면 Fatal이다(11.6절 15). 돌아오면
// 검출이 빠졌다는 뜻이라 0을 돌려준다.
int AssetLoaderFatalProbe(int, const char* const*) noexcept {
    rocks::test::PrepareFatalTestProcess();
    AssetEnv env;
    if (!env.Ready()) {
        return 2;
    }

    LoaderRig rig(env);
    rig.scene.Add(1);
    if (!rig.Cook() || !rig.Start()) {
        return 3;
    }

    platform::Thread thread;
    if (thread.Start(
            [](void* context) noexcept { static_cast<LoaderRig*>(context)->loader.Reset(); },
            &rig) != platform::ThreadStatus::Success) {
        return 4;
    }

    (void)thread.Join();
    return 0;
}

const bool AssetLoaderProbeRegistered =
    rocks::test::RegisterProcessProbe("asset-loader", &AssetLoaderFatalProbe);

} // namespace

// 소멸 스레드(11.6절 15): 초기화한 로더를 소유 스레드가 아닌 곳에서 파괴하면 자식 프로세스가
// Fatal로 끝나고 그 문구를 남긴다.
TEST(AssetLoader_DestroyOffOwnerThreadIsFatal) {
    REQUIRE(AssetLoaderProbeRegistered);
    char executable[4096];
    usize executableBytes = 0;
    REQUIRE(platform::GetExecutablePath(executable, sizeof(executable), executableBytes));
    const platform::ProcessArgument arguments[] = {{"--process-probe", 15}, {"asset-loader", 12}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 2;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[4096];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    REQUIRE(run.status && !run.timedOut);
    error[run.errorBytes < sizeof(error) ? run.errorBytes : sizeof(error) - 1] = '\0';
    CHECK(run.exit.kind != platform::ProcessExitKind::Exited || run.exit.code != 0);
    CHECK(strstr(error, "AssetLoader destroyed outside its owner thread") != nullptr);
}

// worker 제출 실패(11.6절 5): WorkerSchedulerLoaderWorker는 작업 묶음 할당 실패와 scheduler 거부를
// 그 오류로 돌려주고 작업을 실행하지 않는다.
TEST(AssetLoader_SchedulerWorkerReportsSubmitFailures) {
    SystemAllocator allocator;
    test::NFailAllocator failing(allocator);
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerLoaderWorker worker(scheduler, failing);
    uint32 ran = 0;
    auto job = [](void* context) noexcept { ++*static_cast<uint32*>(context); };
    // 시작 전 scheduler는 거부한다.
    CHECK(!worker.Run(job, &ran));
    WorkerSchedulerConfig config;
    config.workerCount = 1;
    REQUIRE(scheduler.Start(config));
    failing.FailOnAttempt(1);
    CHECK(worker.Run(job, &ran).Error() == ErrorCode::OutOfMemory);
    failing.DisableFailure();
    REQUIRE(scheduler.Shutdown());
    CHECK(ran == 0);
}

// CancelToken(11.6절 7): 기본 token은 취소될 수 없고, source의 token 복사·이동·대입이 같은 표시를
// 보며, source가 사라져도 token은 표시를 읽는다. 다른 스레드의 Cancel도 보인다(Join 뒤). 공유 상태
// 할당 실패는 그 오류다.
TEST(AssetLoader_CancelTokenContract) {
    SystemAllocator allocator;
    CancelToken none;
    CHECK(!none.CanBeCancelled() && !none.IsCancellationRequested());
    CancelSource empty;
    CHECK(!empty.IsValid() && !empty.IsCancellationRequested());
    empty.Cancel();
    CHECK(!empty.Token().CanBeCancelled());

    auto made = CancelSource::Create(allocator);
    REQUIRE(made);
    CancelSource source = Move(made.Value());
    CancelToken first = source.Token();
    CancelToken copied(first);
    CancelToken assigned;
    assigned = copied;
    CancelToken moved(Move(copied));
    CancelToken moveAssigned;
    moveAssigned = Move(assigned);
    // 자기 대입은 그대로다(참조를 거친다 — 계수를 놓았다 다시 잡지 않는다).
    CancelToken& alias = assigned;
    assigned = alias;
    CHECK(first.CanBeCancelled() && moved.CanBeCancelled() && moveAssigned.CanBeCancelled());
    CHECK(!copied.CanBeCancelled());
    CHECK(!first.IsCancellationRequested());

    CancelSource other;
    other = Move(source);
    CHECK(!source.IsValid() && other.IsValid() && !other.IsCancellationRequested());
    // 자기 이동 대입은 그대로다(참조를 거친다).
    CancelSource& sourceAlias = other;
    other = Move(sourceAlias);
    CHECK(other.IsValid());
    CancelToken& tokenAlias = moved;
    moved = Move(tokenAlias);
    CHECK(moved.CanBeCancelled());
    CancelSource third(Move(other));
    platform::Thread thread;
    REQUIRE(
        thread.Start([](void* context) noexcept { static_cast<CancelSource*>(context)->Cancel(); },
                     &third) == platform::ThreadStatus::Success);
    REQUIRE(thread.Join() == platform::ThreadStatus::Success);
    CHECK(third.IsCancellationRequested());
    CHECK(first.IsCancellationRequested() && moved.IsCancellationRequested() &&
          moveAssigned.IsCancellationRequested());
    // source가 사라져도 token은 읽는다.
    {
        CancelSource gone = Move(third);
    }

    CHECK(first.IsCancellationRequested());
    first = CancelToken();
    CHECK(!first.CanBeCancelled());

    test::NFailAllocator failing(allocator);
    failing.FailOnAttempt(1);
    CHECK(CancelSource::Create(failing).Error() == ErrorCode::OutOfMemory);
}

} // namespace loader_io_tests_file
