#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>
#include <AssetCooker/CookPublish.h>
#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/CookedPackageIo.h>
#include <Core/FileError.h>
#include <Core/FileRead.h>
#include <Object/AssetId.h>
#include <TargetProfiles/TargetProfile.h>
#include <Core/Memory/UniquePtr.h>
#include <Platform/AsyncFile.h>
#include <Platform/File.h>
#include <Task/WorkerScheduler.h>

// asset-load-cooked 명령이다(Docs/Design/AssetSystem 13.1절 A6): 쿠킹 루트의 매니페스트로 에셋
// 하나를 단발 로드해 확인한다. 로더 구성은 EngineAssets(11.8절 4)와 같고, 잠금·서명 봉투는 보지
// 않는다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView LoadCookedName = "asset-load-cooked";
constexpr StringView LoadCookedUsage =
    "usage: asset-load-cooked --profile=<id> <AssetId|path hint> [--project=<dir>]\n";
// 도구라 로더 worker·IO worker 수를 고정한다.
constexpr uint32 LoadCookedLoaderWorkers = 1;
constexpr uint32 LoadCookedIoWorkers = 1;
// 동기 대기 위치 이름이다(로더 진단).
constexpr const char* LoadCookedSite = "asset-load-cooked";
// 에셋 Registry 문맥 id다.
constexpr ContextId LoadCookedContext{0x4c4f4144434f4f4bull, 1};

struct LoadCookedArgs {
    StringView project;
    StringView profile;
    StringView target;
    bool help = false;
};

// 프로필 id 문법(`[A-Za-z0-9_]`, 1–TargetProfileIdMaxBytes byte — TargetProfiles와 같다)이다. 값이
// 쿠킹 루트 경로 조각이라 `..`·구분자가 Saved/Cooked 밖을 가리키지 못하게 한다.
bool IsProfileIdText(StringView id) noexcept {
    if (id.IsEmpty() || id.ByteCount() > TargetProfileIdMaxBytes) {
        return false;
    }

    for (usize index = 0; index < id.ByteCount(); ++index) {
        const char c = id.Data()[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }

    return true;
}

// 인자를 읽는다. 사용법 위반은 false다.
bool ParseLoadCooked(Span<const StringView> args, LoadCookedArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return true;
    }

    for (StringView argument : args) {
        if (!argument.StartsWith("--", StringComparison::CaseSensitive)) {
            if (argument.IsEmpty() || !out.target.IsEmpty()) {
                return false;
            }

            out.target = argument;
            continue;
        }

        StringView* const slots[] = {&out.project, &out.profile};
        const StringView names[] = {"--project=", "--profile="};
        bool matched = false;
        for (usize index = 0; index < 2 && !matched; ++index) {
            if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                const StringView value = argument.SliceBytes(
                    names[index].ByteCount(), argument.ByteCount() - names[index].ByteCount());
                if (!slots[index]->IsEmpty() || value.IsEmpty()) {
                    return false;
                }

                *slots[index] = value;
                matched = true;
            }
        }

        if (!matched) {
            return false;
        }
    }

    // 경로 힌트가 아니면 36자 소문자 AssetId여야 한다(다른 명령과 같은 AssetId::TryParse — 문법
    // 위반은 사용법 오류).
    if (!IsProfileIdText(out.profile) || out.target.IsEmpty()) {
        return false;
    }

    return out.target.StartsWith("/", StringComparison::CaseSensitive) ||
           AssetId::TryParse(out.target).IsOk();
}

// 로드 실패 줄이다(`<진단 코드 또는 오류>: <대상>: error <원인>` + related).
Result<int32> WriteLoadFailure(CommandletOutput& output, Allocator& allocator, StringView target,
                               const AssetDiagnostic& diagnostic, ErrorCode error) noexcept {
    const char* code = diagnostic.code != AssetDiagnosticCode::None
                           ? AssetDiagnosticName(diagnostic.code)
                           : ErrorCodeName(error);
    if (!diagnostic.related.IsNull()) {
        return Fail(output, allocator, LoadCookedName, 1, "{}: {}: error {} (related {})", code,
                    target, ErrorCodeName(error), diagnostic.related);
    }

    return Fail(output, allocator, LoadCookedName, 1, "{}: {}: error {}", code, target,
                ErrorCodeName(error));
}

} // namespace

Result<int32> RunAssetLoadCookedCommand(void* context, Span<const StringView> args,
                                        CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    LoadCookedArgs parsed;
    if (!ParseLoadCooked(args, parsed)) {
        return Write(output, CommandletStream::Error, LoadCookedUsage, 2);
    }

    if (parsed.help) {
        return Write(output, CommandletStream::Output, LoadCookedUsage, 0);
    }

    ProjectLayout layout(allocator);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenProject(output, allocator, LoadCookedName, parsed.project, layout, failure)) {
        return failure;
    }

    AssetCommandTypes types(allocator, *command.modules, command.typeModule);
    if (auto started = types.Start(); !started) {
        return started.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(started.Error())
                   : Fail(output, allocator, LoadCookedName, 1,
                          "{}: {}: cannot register the asset types", ErrorCodeName(started.Error()),
                          types.Module());
    }

    StringBuilder<LineBytes> root(allocator);
    StringBuilder<LineBytes> path(allocator);
    if (auto made = AppendCookedRoot(root, layout.SavedDirectory(), parsed.profile); !made) {
        return Result<int32>::Failure(made.Error());
    }

    if (auto made = path.TryAppendFormat("{}/{}", root.View(), CookedManifestFileName); !made) {
        return Result<int32>::Failure(made.Error());
    }

    // 매니페스트(서명 봉투는 보지 않는다 — 13.1절 2).
    platform::FileSystem& files = platform::PhysicalFileSystem();
    Array<uint8> bytes(allocator);
    bool existed = false;
    if (auto read = ReadWholeFile(files, path.View(), CookedManifestMaxFileBytes, bytes, existed);
        !read || !existed) {
        const ErrorCode error = read ? ErrorCode::NotFound : read.Error();
        return error == ErrorCode::OutOfMemory ? Result<int32>::Failure(error)
                                               : Fail(output, allocator, LoadCookedName, 1,
                                                      "{}: {}: cannot read the cooked manifest",
                                                      ErrorCodeName(error), root.View());
    }

    AssetDiagnostic diagnostic;
    auto manifest = ReadCookedManifest(allocator, bytes.View(), &diagnostic);
    if (!manifest) {
        return manifest.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(manifest.Error())
                   : WriteLoadFailure(output, allocator, root.View(), diagnostic, manifest.Error());
    }

    // 로더 구성(EngineAssets 11.8절 4 순서): IO → 실행기 → AssetManager.
    platform::AsyncIoContext io;
    if (auto started = io.Start(platform::DefaultAsyncIoBackend(), LoadCookedIoWorkers); !started) {
        return Fail(output, allocator, LoadCookedName, 1, "{}: {}: cannot start the IO context",
                    ErrorCodeName(FileErrorCode(started)), root.View());
    }

    struct IoScope {
        platform::AsyncIoContext& io;
        ~IoScope() noexcept {
            (void)io.Shutdown();
        }
    } ioScope{io};

    AsyncCookedPackageIo packageIo(allocator, io);
    WorkerScheduler scheduler(allocator);
    WorkerSchedulerConfig schedulerConfig;
    schedulerConfig.workerCount = LoadCookedLoaderWorkers;
    if (auto started = scheduler.Start(schedulerConfig); !started) {
        return started.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(started.Error())
                   : Fail(output, allocator, LoadCookedName, 1,
                          "{}: {}: cannot start the loader workers", ErrorCodeName(started.Error()),
                          root.View());
    }

    struct SchedulerScope {
        WorkerScheduler& scheduler;
        ~SchedulerScope() noexcept {
            (void)scheduler.Shutdown();
        }
    } schedulerScope{scheduler};

    WorkerSchedulerLoaderWorker worker(scheduler, allocator);
    AssetManagerConfig config;
    config.loader.allocator = &allocator;
    config.loader.types = &types.Types();
    config.loader.manifest = &manifest.Value();
    config.loader.cookedRoot = root.View();
    config.loader.runtimeProfile = parsed.profile;
    config.loader.io = &packageIo;
    config.loader.worker = &worker;
    config.registryContext = LoadCookedContext;
    auto created = UniquePtr<AssetManager>::TryMake(allocator, config);
    if (!created) {
        return Result<int32>::Failure(created.Error());
    }

    AssetManager& manager = *created.Value();
    struct ManagerScope {
        AssetManager& manager;
        ~ManagerScope() noexcept {
            (void)manager.Shutdown();
        }
    } managerScope{manager};

    // 단계마다 진단을 비운다(앞 단계의 칸이 섞이지 않게).
    diagnostic = AssetDiagnostic{};
    if (auto initialized = manager.Initialize(&diagnostic); !initialized) {
        return initialized.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(initialized.Error())
                   : WriteLoadFailure(output, allocator, root.View(), diagnostic,
                                      initialized.Error());
    }

    // 대상: `/`로 시작하면 경로 힌트(caseless), 아니면 AssetId(문법은 인자 단계에서 봤다).
    AssetId id{};
    if (parsed.target.StartsWith("/", StringComparison::CaseSensitive)) {
        auto found = manager.Loader().FindByPath(parsed.target);
        if (!found) {
            return found.Error() == ErrorCode::OutOfMemory
                       ? Result<int32>::Failure(found.Error())
                       : Fail(output, allocator, LoadCookedName, 1, "{}: {}: error {}",
                              ErrorCodeName(found.Error()), parsed.target,
                              ErrorCodeName(found.Error()));
        }

        id = found.Value();
    } else {
        id = AssetId::TryParse(parsed.target).Value();
    }

    AssetRequest request;
    request.id = id;
    uint32 packages = 0;
    {
        diagnostic = AssetDiagnostic{};
        auto lease = manager.LoadBlocking(request, LoadCookedSite, &diagnostic);
        if (!lease) {
            return lease.Error() == ErrorCode::OutOfMemory
                       ? Result<int32>::Failure(lease.Error())
                       : WriteLoadFailure(output, allocator, parsed.target, diagnostic,
                                          lease.Error());
        }

        // 공개 패키지 수는 로더 상태다(요청 하나라 대상과 Hard 의존 폐포 — 13.1절 5).
        AssetManagerSnapshot snapshot(allocator);
        if (auto described = manager.Describe(snapshot); !described) {
            return Result<int32>::Failure(described.Error());
        }

        packages = snapshot.loader.loadedPackages;
    }

    const CookedManifestEntry* entry = manifest.Value().Find(id);
    const StringView hint = entry != nullptr ? entry->pathHint.View() : StringView();
    StringBuilder<LineBytes> line(allocator);
    if (auto made = line.TryAppendFormat("{}: {} loaded {} ({}) packages {}\n", LoadCookedName,
                                         parsed.profile, id, hint, packages);
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Write(output, CommandletStream::Output, line.View(), 0);
}

} // namespace rocks::commandlet_host
