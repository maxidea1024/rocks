#include "EngineAssetsInternal.h"

#include <Core/App/ProcessCommandLine.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Text/StringBuilder.h>
#include <EngineAssets/EngineAssetsSubsystem.h>
#include <Platform/Atomics.h>
#include <Platform/File.h>
#include <Platform/Process.h>

// EngineAssetsSubsystem 구현이다(Docs/Design/AssetSystem 11.8절).
namespace rocks {

namespace {

constexpr LogCategory LogEngineAssets{"EngineAssets"};

// 에셋 객체 Registry 문맥 id의 상위 값이다("ROCKSAST" — 엔진 문맥·World 문맥 상위 값과 다르다).
// 하위 값은 프로세스 안 순번이다(한 프로세스의 Engine이 여럿이어도 겹치지 않는다).
constexpr uint64 AssetContextHigh = 0x524f434b53415354ull;
uint64 NextAssetContext = 0;

// 등록된 콘텐츠 서명 신뢰 정보다(SetContentTrust). 여러 스레드가 각자 엔진을 초기화해도 경쟁이 없게
// 원자적으로 쓰고 읽는다(release·acquire — 가리키는 값은 등록 전에 다 만들어져 있다).
const ContentTrust* ContentTrustSource = nullptr;

// 엔진 전용 실행의 엔진 디렉터리다(SetEngineOnlyDirectory — 엔진 Initialize 전에만 쓰고
// Initialize가 읽는다. 쓰기와 읽기가 겹치지 않는 것이 계약이라 원자 연산을 두지 않는다).
const char* EngineOnlyDirectoryData = nullptr;
usize EngineOnlyDirectoryBytes = 0;

// 봉투 검증 실패 단계 이름("content signature (<칸>)")의 버퍼 byte 수다.
constexpr usize ContentStageBytes = 64;

bool FrameRunning(void* context) noexcept {
    return static_cast<const Engine*>(context)->IsInFrame();
}

} // namespace

namespace engine_assets_detail {

// path 파일 전체를 읽는다(크기 상한 maxBytes — 넘으면 CapacityExceeded, 없으면 NotFound).
Result<void> ReadManifestFile(platform::FileSystem& files, StringView path, uint64 maxBytes,
                              Array<uint8>& out) noexcept {
    bool exists = false;
    if (auto checked = files.FileExists(path.Data(), path.ByteCount(), exists); !checked) {
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

void EngineAssetsSubsystem::SetContentTrust(const ContentTrust* trust) noexcept {
    platform::AtomicStore<platform::MemoryOrder::Release>(&ContentTrustSource, trust);
}

void EngineAssetsSubsystem::SetEngineOnlyDirectory(StringView engineDirectory) noexcept {
    EngineOnlyDirectoryData = engineDirectory.IsEmpty() ? nullptr : engineDirectory.Data();
    EngineOnlyDirectoryBytes = engineDirectory.ByteCount();
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
    auto fail = [&](StringView stage, ErrorCode error) noexcept {
        Log::Error(LogEngineAssets,
                   "EngineAssets initialization failed at {Stage}: {Error} (diagnostic {Code}, "
                   "cooked root '{Root}', profile '{Profile}')",
                   stage, ErrorCodeName(error), AssetDiagnosticName(diagnostic.code),
                   cookedRoot_.View(), profile_.View());
        Teardown();
        return Result<void>::Failure(error);
    };

    // 쿠킹 루트·프로파일(부팅 CVar — 시작 때 한 번). 엔진 전용 실행의 루트는 프로파일을 담으므로
    // 프로파일을 먼저 정한다.
    profile_.Clear();
    if (auto assigned = profile_.TryAppend(ResolveRuntimeProfile(RuntimeProfileVar.Get()));
        !assigned) {
        return fail("runtime profile", assigned.Error());
    }

    const StringView configuredRoot = CookedRootVar.Get();
    const StringView engineOnly(EngineOnlyDirectoryData,
                                EngineOnlyDirectoryData == nullptr ? 0 : EngineOnlyDirectoryBytes);
    StringView executableDir;
    if (configuredRoot.IsEmpty() && engineOnly.IsEmpty()) {
        // 진입점(Launch)이 캡처한 실행 파일 경로다(OS에서 얻은 절대 경로 — 긴 경로도 받는다).
        if (!ProcessCommandLine::IsCaptured()) {
            return fail("executable path", ErrorCode::InvalidArgument);
        }

        executableDir = ExecutableDirectoryOf(ProcessCommandLine::Get().ExecutablePath());
    }

    cookedRoot_.Clear();
    if (auto resolved = ResolveCookedRoot(configuredRoot, engineOnly, profile_.View(),
                                          executableDir, cookedRoot_);
        !resolved) {
        return fail("cooked root", resolved.Error());
    }

    // 매니페스트: 서명 봉투가 있으면 그것만 읽어 검증하고 같은 버퍼의 매니페스트 byte를 쓴다
    // (ContentSigning.ko.md 7절 4 — 봉투가 있으면 rman은 읽지 않는다).
    {
        String path(allocator);
        auto buildPath = [&](StringView name) noexcept -> Result<void> {
            path.Clear();
            if (auto built = path.TryAppend(cookedRoot_.View()); !built) {
                return built;
            }

            if (auto built = path.TryAppend("/"); !built) {
                return built;
            }

            return path.TryAppend(name);
        };
        if (auto built = buildPath(ContentEnvelopeFileName); !built) {
            return fail("manifest path", built.Error());
        }

        Array<uint8> bytes(allocator);
        Span<const uint8> manifestBytes;
        platform::FileSystem& files = platform::PhysicalFileSystem();
        auto envelope = ReadManifestFile(files, path.View(), ContentEnvelopeMaxBytes, bytes);
        if (envelope) {
            const ContentTrust* trust =
                platform::AtomicLoad<platform::MemoryOrder::Acquire>(&ContentTrustSource);
            if (trust == nullptr) {
                return fail("content trust", ErrorCode::InvalidArgument);
            }

            ContentEnvelopeInfo info;
            auto verified =
                VerifyContentEnvelope(allocator, bytes.View(), *trust, &info, &diagnostic);
            if (!verified) {
                // 걸린 봉투 칸을 단계 이름에 담는다(4절 "단계는 진단 로그에만").
                char stage[ContentStageBytes];
                FixedStringBuilder text{Span<char>(stage)};
                (void)text.TryAppendFormat(
                    "content signature ({})",
                    ContentEnvelopeFieldName(
                        static_cast<ContentEnvelopeField>(diagnostic.section)));
                return fail(text.View(), verified.Error());
            }

            manifestBytes = verified.Value();
            stats_.contentSigned = true;
            stats_.contentVersion = info.contentVersion;
        } else if (envelope.Error() != ErrorCode::NotFound) {
            return fail("content signature read", envelope.Error());
        } else if constexpr (IsReleaseBuild) {
            // Release는 서명된 콘텐츠만 쓴다(CS5).
            return fail("content signature", ErrorCode::NotFound);
        } else {
            Log::Warning(LogEngineAssets,
                         "Cooked content is not signed: '{Root}' has no {File}; reading {Manifest}",
                         cookedRoot_.View(), ContentEnvelopeFileName, CookedManifestFileName);
            if (auto built = buildPath(CookedManifestFileName); !built) {
                return fail("manifest path", built.Error());
            }

            if (auto read = ReadManifestFile(files, path.View(), CookedManifestMaxFileBytes, bytes);
                !read) {
                return fail("manifest read", read.Error());
            }

            manifestBytes = bytes.View();
        }

        auto manifest = ReadCookedManifest(allocator, manifestBytes, &diagnostic);
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
    auto streamedFiles = UniquePtr<AsyncStreamedBulkIo>::TryMake(allocator, allocator, io_);
    if (!streamedFiles) {
        return fail("streamed bulk io", streamedFiles.Error());
    }

    streamedFiles_ = Move(streamedFiles.Value());
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
    config.streamedIo = streamedFiles_.Get();
    config.loader.limits.maxPackagesInFlight = static_cast<uint32>(MaxPackagesInFlightVar.Get());
    config.loader.limits.maxTempBytes = static_cast<uint64>(MaxTempBytesVar.Get());
    config.loader.inFrame = &FrameRunning;
    config.loader.inFrameContext = &engine;
    const uint64 sequence =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextAssetContext, uint64{1});
    config.registryContext = ContextId{AssetContextHigh, sequence + 1};
    // 엔진 이름 pool을 같이 쓴다(Docs/Design/AssetSystem 11.8절 4 — 에셋·World 사이 Name 값이
    // 같다).
    config.registryOptions.names = &engine.Names();
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

    // 엔진 GC scheduler가 있으면 표식을 병렬로 한다(GC-S4) — 넘긴 조각 수를 센다.
    WorkerScheduler* scheduler = engine_->GcScheduler();
    stats_.gcParallelSteps += scheduler != nullptr ? 1u : 0u;
    auto progress =
        registry.StepCollection(access, engine_assets_detail::ReadGcBudget(), scheduler);
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

    streamedFiles_.Reset();
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

ROCKS_DEFINE_NATIVE_TYPE_KEY(rocks::EngineAssetsSubsystem)
