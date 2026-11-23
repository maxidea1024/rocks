#include "EngineAssetsInternal.h"

#include <Config/CVar.h>
#include <EngineAssets/EngineAssetsSubsystem.h>

// `asset.*` CVar 정의와 등록, 쿠킹 루트·실행 프로파일 결정이다(Docs/Design/AssetSystem 11.8절
// 3). 부팅 값은 시작 때 한 번 읽고, Pump·GC 예산은 프레임마다 설정 sync 뒤에 읽는다.
namespace rocks {

namespace {

// UE s.AsyncLoadingTimeLimit와 증분 GC 시간 한도의 기본값이다(ms — CoreSettings.cpp:14,
// GarbageCollection.cpp:310).
constexpr double DefaultPumpMilliseconds = 5.0;
constexpr double DefaultGcMilliseconds = 5.0;
// UE gc.TimeBetweenPurgingPendingKillObjects 기본값이다(UnrealEngine.cpp:1777).
constexpr double DefaultGcRetrySeconds = 60.0;
constexpr double MaxGcRetrySeconds = 60.0 * 60.0 * 24.0;
constexpr double MaxBudgetMilliseconds = 1000.0 * 60.0;
constexpr int32 DefaultLoaderWorkers = 2;
constexpr int32 DefaultIoWorkers = 2;
constexpr int32 MaxWorkers = 64;
constexpr int64 MaxPackagesInFlight = int64{1} << 20;
constexpr int64 MaxCountBudget = NumericLimits<int32>::Max();

} // namespace

namespace engine_assets_detail {

constinit BootCVar<String> CookedRootVar =
    cvar::Boot("asset.cooked_root", "")
        .Help("Cooked content root folder (contains CookedManifest.rman). Empty means "
              "<executable folder>/Cooked. Fixed after startup.");

constinit BootCVar<String> RuntimeProfileVar =
    cvar::Boot("asset.runtime_profile", "")
        .Help("Runtime target profile id the cooked content must match. Empty means the build "
              "platform name (Windows, Mac or Android). Fixed after startup.");

constinit BootCVar<int32> LoaderWorkersVar =
    cvar::Boot("asset.loader.workers", DefaultLoaderWorkers)
        .Help("Worker threads for package verification and decoding. Fixed after startup.")
        .Range(1, MaxWorkers);

constinit BootCVar<int32> IoWorkersVar =
    cvar::Boot("asset.io.workers", DefaultIoWorkers)
        .Help(
            "Worker threads of the asset loader's asynchronous file IO on the worker pool backend "
            "(POSIX). The Windows native backend (IOCP) uses one completion thread and ignores "
            "this value. Fixed after startup.")
        .Range(1, MaxWorkers);

constinit BootCVar<int64> MaxPackagesInFlightVar =
    cvar::Boot("asset.loader.max_packages_in_flight",
               int64{AssetLoaderLimits::DefaultMaxPackagesInFlight})
        .Help("Upper bound of packages admitted at once (IO, worker or publish wait). Fixed after "
              "startup.")
        .Range(1, MaxPackagesInFlight);

constinit BootCVar<int64> MaxTempBytesVar =
    cvar::Boot("asset.loader.max_temp_bytes", int64(AssetLoaderLimits::DefaultMaxTempBytes))
        .Help("Upper bound of temporary bytes (file size plus decoded bulk) of admitted packages. "
              "Fixed after startup.")
        .Range(1, NumericLimits<int64>::Max());

constinit CVar<double> PumpMillisecondsVar =
    cvar::Var("asset.pump.time_ms", DefaultPumpMilliseconds)
        .Help("Time budget of one asset pump for publishing and PostLoad (milliseconds; one unit "
              "and one PostLoad always run). Applied at the next config sync.")
        .Range(0.0, MaxBudgetMilliseconds);

constinit CVar<int64> PumpMaxUnitsVar =
    cvar::Var("asset.pump.max_units", int64{0})
        .Help("Load units published per asset pump (0 means unlimited). Applied at the next config "
              "sync.")
        .Range(0, MaxCountBudget);

constinit CVar<int64> PumpMaxPostLoadsVar =
    cvar::Var("asset.pump.max_post_loads", int64{0})
        .Help("PostLoad calls per asset pump (0 means unlimited). Applied at the next config sync.")
        .Range(0, MaxCountBudget);

constinit CVar<int64> PumpMaxDiscardsVar =
    cvar::Var("asset.pump.max_discards", int64{0})
        .Help("Packages discarded per asset pump (0 means unlimited). Applied at the next config "
              "sync.")
        .Range(0, MaxCountBudget);

constinit CVar<double> GcMillisecondsVar =
    cvar::Var("asset.gc.time_ms", DefaultGcMilliseconds)
        .Help("Time budget of one asset object GC step at the end of a frame (milliseconds). "
              "Applied at the next config sync.")
        .Range(0.0, MaxBudgetMilliseconds);

constinit CVar<int64> GcMaxObjectsVar =
    cvar::Var("asset.gc.max_objects", int64{GcBudget::DefaultMaxObjects})
        .Help("Objects examined per asset object GC step at the end of a frame. Applied at the "
              "next config sync.")
        .Range(1, MaxCountBudget);

constinit CVar<double> GcRetrySecondsVar =
    cvar::Var("asset.gc.retry_seconds", DefaultGcRetrySeconds)
        .Help("Engine frame time after a finished asset object GC cycle before collecting again "
              "while discarded objects are still held (seconds). Applied at the next config "
              "sync.")
        .Range(0.0, MaxGcRetrySeconds);

uint64 MillisecondsToNanoseconds(double milliseconds) noexcept {
    constexpr double NanosecondsPerMillisecond = 1000000.0;
    return static_cast<uint64>(milliseconds * NanosecondsPerMillisecond);
}

uint32 CountOrUnlimited(int64 value, uint32 unlimited) noexcept {
    return value == 0 ? unlimited : static_cast<uint32>(value);
}

AssetManagerPumpBudget ReadPumpBudget() noexcept {
    AssetManagerPumpBudget budget;
    budget.loader.maxNanoseconds = MillisecondsToNanoseconds(PumpMillisecondsVar.Get());
    budget.loader.maxUnits =
        CountOrUnlimited(PumpMaxUnitsVar.Get(), AssetLoaderPumpBudget::UnlimitedUnits);
    budget.loader.maxPostLoads =
        CountOrUnlimited(PumpMaxPostLoadsVar.Get(), AssetLoaderPumpBudget::UnlimitedUnits);
    budget.maxDiscards =
        CountOrUnlimited(PumpMaxDiscardsVar.Get(), AssetManagerPumpBudget::UnlimitedDiscards);
    return budget;
}

GcBudget ReadGcBudget() noexcept {
    GcBudget budget;
    budget.maxNanoseconds = MillisecondsToNanoseconds(GcMillisecondsVar.Get());
    budget.maxObjects = static_cast<uint32>(GcMaxObjectsVar.Get());
    return budget;
}

} // namespace engine_assets_detail

Result<void> RegisterEngineAssetsSettings(config::ConfigContext& context) noexcept {
    using namespace engine_assets_detail;
    // 콘솔 이름 영역 `asset`을 더한다(기본 표에 없다 — TraceControl의 `trace`와 같은 방식). 이미
    // 있으면(두 번째 등록) 그대로 쓴다 — 변수 등록도 같은 핸들이면 멱등이다.
    if (auto region = context.RegisterConsoleRegion("asset");
        !region && region.Error() != ErrorCode::AlreadyExists) {
        return region;
    }

    return context.Register(CookedRootVar, RuntimeProfileVar, LoaderWorkersVar, IoWorkersVar,
                            MaxPackagesInFlightVar, MaxTempBytesVar, PumpMillisecondsVar,
                            PumpMaxUnitsVar, PumpMaxPostLoadsVar, PumpMaxDiscardsVar,
                            GcMillisecondsVar, GcMaxObjectsVar, GcRetrySecondsVar);
}

StringView ExecutableDirectoryOf(StringView executablePath) noexcept {
    const char* data = executablePath.Data();
    usize end = executablePath.ByteCount();
    while (end != 0 && data[end - 1] != '/' && data[end - 1] != '\\') {
        --end;
    }

    // 구분자 자체는 빼고 돌려준다(루트 하나뿐이면 그 구분자를 남긴다).
    if (end > 1) {
        --end;
    }

    return StringView(data, end);
}

Result<void> ResolveCookedRoot(StringView configured, StringView executableDir,
                               String& out) noexcept {
    if (!configured.IsEmpty()) {
        return out.TryAppend(configured);
    }

    if (executableDir.IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto appended = out.TryAppend(executableDir); !appended) {
        return appended;
    }

    return out.TryAppend("/Cooked");
}

StringView ResolveRuntimeProfile(StringView configured) noexcept {
    if (!configured.IsEmpty()) {
        return configured;
    }

    // 지원 플랫폼이 늘면(Linux 등) 여기서 이름을 정해야 한다 — 모르는 플랫폼은 빌드 오류다.
    static_assert(IsWindows || IsMac || IsAndroid,
                  "Add the default runtime profile id of the new platform");
    if constexpr (IsWindows) {
        return "Windows";
    } else if constexpr (IsMac) {
        return "Mac";
    } else {
        return "Android";
    }
}

} // namespace rocks
