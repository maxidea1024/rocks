#include "EngineAssetsInternal.h"

#include <Core/App/ProcessCommandLine.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <Platform/Atomics.h>
#include <Platform/File.h>
#include <Platform/Process.h>

// EngineAssetsSubsystem 구현이다(Docs/Design/AssetSystem.ko.md 11.8절).
namespace rocks {

namespace {

constexpr LogCategory LogEngineAssets{"EngineAssets"};

// 에셋 객체 Registry 문맥 id의 상위 값이다("ROCKSAST" — 엔진 문맥·World 문맥 상위 값과 다르다).
// 하위 값은 프로세스 안 순번이다(한 프로세스의 Engine이 여럿이어도 겹치지 않는다).
constexpr uint64 AssetContextHigh = 0x524f434b53415354ull;
uint64 NextAssetContext = 0;

bool FrameRunning(void* context) noexcept {
    return static_cast<const Engine*>(context)->IsInFrame();
}

} // namespace

namespace engine_assets_detail {

// path 파일 전체를 읽는다(크기 상한 maxBytes — 넘으면 CapacityExceeded, 없으면 NotFound).
Result<void> ReadManifestFile(platform::FileSystem& files, StringView path, uint64 maxBytes,
                              Array<uint8>& out) noexcept {
    bool exists = false;
    if (auto checked = files.Exists(path.Data(), path.ByteCount(), exists); !checked) {
        return Result<void>::Failure(FileErrorCode(checked));
    }

    if (!exists) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    platform::File file;
    if (auto opened =
            file.Open(files, path.Data(), path.ByteCount(), platform::FileOpenMode::ReadExisting);
        !opened) {
        return Result<void>::Failure(FileErrorCode(opened));
    }

    uint64 size = 0;
    platform::FileStatus status = file.Size(size);
    ErrorCode error = status ? ErrorCode::None : FileErrorCode(status);
    if (error == ErrorCode::None && size > maxBytes) {
        error = ErrorCode::CapacityExceeded;
    }

    if (error == ErrorCode::None) {
        if (auto resized = out.TryResize(static_cast<usize>(size)); !resized) {
            error = resized.Error();
        }
    }

    if (error == ErrorCode::None && size != 0) {
        status = file.ReadExact(out.Data(), static_cast<usize>(size)).status;
        error = status ? ErrorCode::None : FileErrorCode(status);
    }

    const platform::FileStatus closed = file.Close();
    if (error == ErrorCode::None && !closed) {
        error = FileErrorCode(closed);
    }

    return error == ErrorCode::None ? Result<void>::Success() : Result<void>::Failure(error);
}

} // namespace engine_assets_detail

EngineAssetsSubsystem::EngineAssetsSubsystem(Allocator& allocator) noexcept
    : allocator_(&allocator), cookedRoot_(allocator), profile_(allocator) {
}

EngineAssetsSubsystem::~EngineAssetsSubsystem() {
    Teardown();
}

Result<void> EngineAssetsSubsystem::Initialize(Engine& engine) noexcept {
    using namespace engine_assets_detail;
    engine_ = &engine;
    Allocator& allocator = engine.GetAllocator();
    allocator_ = &allocator;
    if (engine.Settings() == nullptr) {
        Log::Error(LogEngineAssets, "EngineAssets requires an engine config context");
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AssetDiagnostic diagnostic;
    auto fail = [&](const char* stage, ErrorCode error) noexcept {
        Log::Error(LogEngineAssets,
                   "EngineAssets initialization failed at {Stage}: {Error} (diagnostic {Code}, "
                   "cooked root '{Root}', profile '{Profile}')",
                   stage, ErrorCodeName(error), AssetDiagnosticName(diagnostic.code),
                   cookedRoot_.View(), profile_.View());
        Teardown();
        return Result<void>::Failure(error);
    };

    // 쿠킹 루트·프로파일(부팅 CVar — 시작 때 한 번).
    const StringView configuredRoot = CookedRootVar.Get();
    StringView executableDir;
    if (configuredRoot.IsEmpty()) {
        // 진입점(Launch)이 캡처한 실행 파일 경로다(OS에서 얻은 절대 경로 — 긴 경로도 받는다).
        if (!ProcessCommandLine::IsCaptured()) {
            return fail("executable path", ErrorCode::InvalidArgument);
        }

        executableDir = ExecutableDirectoryOf(ProcessCommandLine::Get().ExecutablePath());
    }

    cookedRoot_.Clear();
    if (auto resolved = ResolveCookedRoot(configuredRoot, executableDir, cookedRoot_); !resolved) {
        return fail("cooked root", resolved.Error());
    }

    profile_.Clear();
    if (auto assigned = profile_.TryAppend(ResolveRuntimeProfile(RuntimeProfileVar.Get()));
        !assigned) {
        return fail("runtime profile", assigned.Error());
    }

    // 매니페스트(A1f에서 서명 봉투로 바뀐다).
    {
        String path(allocator);
        if (auto built = path.TryAppend(cookedRoot_.View()); !built) {
            return fail("manifest path", built.Error());
        }

        if (auto built = path.TryAppend("/"); !built) {
            return fail("manifest path", built.Error());
        }

        if (auto built = path.TryAppend(CookedManifestFileName); !built) {
            return fail("manifest path", built.Error());
        }

        Array<uint8> bytes(allocator);
        if (auto read = ReadManifestFile(platform::PhysicalFileSystem(), path.View(),
                                         CookedManifestMaxFileBytes, bytes);
            !read) {
            return fail("manifest read", read.Error());
        }

        auto manifest = ReadCookedManifest(allocator, bytes.View(), &diagnostic);
        if (!manifest) {
            return fail("manifest parse", manifest.Error());
        }

        auto made = UniquePtr<CookedManifest>::TryMake(allocator, Move(manifest.Value()));
        if (!made) {
            return fail("manifest", made.Error());
        }

        manifest_ = Move(made.Value());
    }

    // 실행기·IO(11.8절 4 순서).
    if (auto started =
            io_.Start(platform::DefaultAsyncIoBackend(), static_cast<uint32>(IoWorkersVar.Get()));
        !started) {
        return fail("io context", FileErrorCode(started));
    }

    ioStarted_ = true;
    auto files = UniquePtr<AsyncCookedPackageIo>::TryMake(allocator, allocator, io_);
    if (!files) {
        return fail("package io", files.Error());
    }

    files_ = Move(files.Value());
    auto scheduler = UniquePtr<WorkerScheduler>::TryMake(allocator, allocator);
    if (!scheduler) {
        return fail("loader scheduler", scheduler.Error());
    }

    scheduler_ = Move(scheduler.Value());
    WorkerSchedulerConfig schedulerConfig;
    schedulerConfig.workerCount = static_cast<uint32>(LoaderWorkersVar.Get());
    if (auto started = scheduler_->Start(schedulerConfig); !started) {
        return fail("loader scheduler", started.Error());
    }

    auto worker =
        UniquePtr<WorkerSchedulerLoaderWorker>::TryMake(allocator, *scheduler_, allocator);
    if (!worker) {
        return fail("loader worker", worker.Error());
    }

    worker_ = Move(worker.Value());
    stats_.loaderWorkers = scheduler_->Stats().workerCount;

    // AssetManager.
    AssetManagerConfig config;
    config.loader.allocator = &allocator;
    config.loader.types = &engine.Types();
    config.loader.manifest = manifest_.Get();
    config.loader.cookedRoot = cookedRoot_.View();
    config.loader.runtimeProfile = profile_.View();
    config.loader.io = files_.Get();
    config.loader.worker = worker_.Get();
    config.loader.limits.maxPackagesInFlight = static_cast<uint32>(MaxPackagesInFlightVar.Get());
    config.loader.limits.maxTempBytes = static_cast<uint64>(MaxTempBytesVar.Get());
    config.loader.inFrame = &FrameRunning;
    config.loader.inFrameContext = &engine;
    const uint64 sequence =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextAssetContext, uint64{1});
    config.registryContext = ContextId{AssetContextHigh, sequence + 1};
    auto manager = UniquePtr<AssetManager>::TryMake(allocator, config);
    if (!manager) {
        return fail("asset manager", manager.Error());
    }

    manager_ = Move(manager.Value());
    if (auto initialized = manager_->Initialize(&diagnostic); !initialized) {
        return fail("asset manager", initialized.Error());
    }

    Log::Info(LogEngineAssets,
              "EngineAssets started: cooked root '{Root}', profile '{Profile}', {Entries} entries",
              cookedRoot_.View(), profile_.View(), manifest_->entries.Size());
    return Result<void>::Success();
}

// 프레임·종료 단계는 Initialize가 성공한 뒤에만 불린다(엔진 규칙) — manager_가 있다.
void EngineAssetsSubsystem::OnFrameBegin(Engine&) noexcept {
    auto pumped = manager_->Pump(engine_assets_detail::ReadPumpBudget());
    if (!pumped) {
        // 프레임 안 Pump가 실패하는 경우(Shutdown 뒤 ShuttingDown 등)는 되돌아오지 않는다 — 처음
        // 한 번만 기록하고 횟수를 센다(11.8절 5).
        if (stats_.pumpFailures++ == 0) {
            Log::Error(LogEngineAssets, "Asset pump failed: {Error}",
                       ErrorCodeName(pumped.Error()));
        }

        return;
    }

    // 에셋 Registry 객체는 Discard로만 죽는다(6.1절) — Discard한 패키지의 로더 RootRef를 놓았을
    // 때만 수집한다(그 전에는 회수할 수 없다 — 11.7절 3).
    if (pumped.Value().releasedPackages != 0) {
        stats_.gcPending = true;
        if (manager_->Registry().CollectionPhase() != GcPhase::Idle) {
            releasedDuringCycle_ = true;
        }
    }
}

void EngineAssetsSubsystem::OnFrameEnd(Engine& engine) noexcept {
    if (!stats_.gcPending && stats_.lingering) {
        lingerSeconds_ += engine.Time().deltaSeconds;
        if (lingerSeconds_ >= engine_assets_detail::GcRetrySecondsVar.Get()) {
            stats_.lingering = false;
            stats_.gcPending = true;
        }
    }

    StepGarbageCollection();
}

void EngineAssetsSubsystem::StepGarbageCollection() noexcept {
    if (!stats_.gcPending) {
        return;
    }

    ObjectRegistry& registry = manager_->Registry();
    const ObjectAccessContext access = registry.OwnerAccess();
    auto failed = [this](ErrorCode error) noexcept {
        if (error == ErrorCode::Busy) {
            // 열린 생성 트랜잭션·Borrow — 정상이다(다음 프레임에 다시).
            ++stats_.gcBusySkips;
            return;
        }

        // 할당 실패 등은 그 프레임만의 일이라 매번 기록한다(다음 프레임에 다시 한다).
        ++stats_.gcFailures;
        Log::Error(LogEngineAssets, "Asset object GC step failed: {Error}", ErrorCodeName(error));
    };
    if (registry.CollectionPhase() == GcPhase::Idle) {
        if (auto begun = registry.BeginCollection(access); !begun) {
            failed(begun.Error());
            return;
        }

        releasedDuringCycle_ = false;
    }

    auto progress = registry.StepCollection(access, engine_assets_detail::ReadGcBudget(),
                                            engine_->GcScheduler());
    if (!progress) {
        failed(progress.Error());
        return;
    }

    if (progress.Value().IsComplete()) {
        ++stats_.gcCycles;
        // cycle 시작 뒤 RootRef를 놓은 패키지가 더 있었으면 한 번 더 돈다.
        stats_.gcPending = releasedDuringCycle_;
        releasedDuringCycle_ = false;
        // Discard된 객체가 남았으면(쥔 쪽이 있다) 재시도 시간 뒤에 다시 돈다.
        stats_.lingering = registry.Stats().destroyRequested != 0;
        lingerSeconds_ = 0.0;
    }
}

Result<void> EngineAssetsSubsystem::CompleteInFlightWork(Engine&) noexcept {
    auto shut = manager_->Shutdown();
    if (!shut) {
        Log::Error(LogEngineAssets, "AssetManager shutdown failed: {Error}",
                   ErrorCodeName(shut.Error()));
    }

    return shut;
}

void EngineAssetsSubsystem::Deinitialize(Engine&) noexcept {
    Teardown();
}

void EngineAssetsSubsystem::Teardown() noexcept {
    // 11.8절 4의 역순이다. AssetManager 파괴는 살아 있는 handle 객체가 있으면 Fatal이다(계약).
    manager_.Reset();
    worker_.Reset();
    if (scheduler_.Get() != nullptr) {
        if (auto stopped = scheduler_->Shutdown(); !stopped) {
            Log::Error(LogEngineAssets, "Asset loader scheduler shutdown failed: {Error}",
                       ErrorCodeName(stopped.Error()));
        }

        scheduler_.Reset();
    }

    files_.Reset();
    if (ioStarted_) {
        if (auto stopped = io_.Shutdown(); !stopped) {
            Log::Error(LogEngineAssets, "Asset IO context shutdown failed: {Error}",
                       ErrorCodeName(FileErrorCode(stopped)));
        }

        ioStarted_ = false;
    }

    manifest_.Reset();
}

EngineSubsystemType EngineAssetsSubsystemType() noexcept {
    EngineSubsystemType type;
    type.key = SubsystemKeyOf<EngineAssetsSubsystem>();
    type.name = "EngineAssets";
    type.create = [](Allocator& allocator) noexcept -> Result<UniquePtr<EngineSubsystem>> {
        auto created = UniquePtr<EngineAssetsSubsystem>::TryMake(allocator, allocator);
        if (!created) {
            return Result<UniquePtr<EngineSubsystem>>::Failure(created.Error());
        }

        return Result<UniquePtr<EngineSubsystem>>::Success(
            UniquePtr<EngineSubsystem>(Move(created.Value())));
    };
    return type;
}

} // namespace rocks
