#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>
#include <AssetCooker/CookPublish.h>
#include <AssetRuntime/AssetManager.h>
#include <AssetRuntime/CookedPackageIo.h>
#include <AssetRuntime/PackageRangeReader.h>
#include <AssetRuntime/PackageReader.h>
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
// 않는다. `--verify-sidecars`는 매니페스트의 모든 sidecar를 전체 대조한다(20.5 7).
namespace rocks::commandlet_host {

namespace {

constexpr StringView LoadCookedName = "asset-load-cooked";
constexpr StringView LoadCookedUsage =
    "usage: asset-load-cooked --profile=<id> [<AssetId|path hint>] [--project=<dir>] "
    "[--verify-sidecars]\n"
    "  the target is required unless --verify-sidecars is given\n";
// sidecar 검증의 읽기 조각 크기다(sidecar 블록 해시 단위와 같다).
constexpr usize VerifySidecarScratchBytes = SidecarHashBlockBytes;
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
    bool verifySidecars = false;
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
        if (argument == "--verify-sidecars") {
            if (out.verifySidecars) {
                return false;
            }

            out.verifySidecars = true;
            continue;
        }

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
    if (!IsProfileIdText(out.profile) || (out.target.IsEmpty() && !out.verifySidecars)) {
        return false;
    }

    return out.target.IsEmpty() || out.target.StartsWith("/", StringComparison::CaseSensitive) ||
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

// 대상 하나를 로드하고 성공 줄을 line에 붙인다(13.1절 5). 실패는 그 줄을 쓰고 종료 코드를 돌려준다.
Result<int32> LoadCookedTarget(const AssetCommandContext& command, AssetCommandTypes& types,
                               const CookedManifest& manifest, StringView root,
                               const LoadCookedArgs& parsed, CommandletOutput& output,
                               StringBuilderBase& line) noexcept {
    Allocator& allocator = *command.allocator;
    AssetDiagnostic diagnostic;
    // 로더 구성(EngineAssets 11.8절 4 순서): IO → 실행기 → AssetManager.
    platform::AsyncIoContext io;
    if (auto started = io.Start(platform::DefaultAsyncIoBackend(), LoadCookedIoWorkers); !started) {
        return Fail(output, allocator, LoadCookedName, 1, "{}: {}: cannot start the IO context",
                    ErrorCodeName(FileErrorCode(started)), root);
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
                          root);
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
    config.loader.manifest = &manifest;
    config.loader.cookedRoot = root;
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
                   : WriteLoadFailure(output, allocator, root, diagnostic, initialized.Error());
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

    const CookedManifestEntry* entry = manifest.Find(id);
    const StringView hint = entry != nullptr ? entry->pathHint.View() : StringView();
    if (auto made = line.TryAppendFormat("{}: {} loaded {} ({}) packages {}\n", LoadCookedName,
                                         parsed.profile, id, hint, packages);
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Result<int32>::Success(0);
}

// sidecar 검증 실패 줄이다(`<진단 코드 또는 오류>: <AssetId>: error <원인> (sidecar)` — 20.5 7).
Result<int32> WriteSidecarFailure(CommandletOutput& output, Allocator& allocator, AssetId asset,
                                  const AssetDiagnostic& diagnostic, ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(error);
    }

    const char* code = diagnostic.code != AssetDiagnosticCode::None
                           ? AssetDiagnosticName(diagnostic.code)
                           : ErrorCodeName(error);
    return Fail(output, allocator, LoadCookedName, 1, "{}: {}: error {} (sidecar)", code, asset,
                ErrorCodeName(error));
}

// 매니페스트의 sidecar를 모두 대조한다(20.5 7): 패키지 파일 → 패키지 내용(Sidecar 섹션) → sidecar
// 파일 전체(VerifySidecarFile). 첫 실패에서 그 줄을 쓰고 멈춘다.
Result<int32> VerifyCookedSidecars(CommandletOutput& output, Allocator& allocator,
                                   const CookedManifest& manifest, StringView root,
                                   uint32& verified) noexcept {
    platform::FileSystem& files = platform::PhysicalFileSystem();
    Array<uint8> scratch(allocator);
    if (auto sized = scratch.TryResize(VerifySidecarScratchBytes); !sized) {
        return Result<int32>::Failure(sized.Error());
    }

    verified = 0;
    for (const CookedManifestEntry& entry : manifest.entries) {
        if (entry.sidecarBytes == 0) {
            continue;
        }

        AssetDiagnostic diagnostic;
        StringBuilder<LineBytes> packagePath(allocator);
        StringBuilder<LineBytes> sidecarPath(allocator);
        Result<void> built = packagePath.TryAppendFormat("{}/", root);
        if (built) {
            built = AppendCookedPackagePath(packagePath, entry.asset, entry.packageSha256);
        }

        if (built) {
            built = sidecarPath.TryAppendFormat("{}/", root);
        }

        if (built) {
            built = AppendCookedSidecarPath(sidecarPath, entry.asset, entry.sidecarSha256);
        }

        if (!built) {
            return Result<int32>::Failure(built.Error());
        }

        Array<uint8> bytes(allocator);
        bool existed = false;
        if (auto read =
                ReadWholeFile(files, packagePath.View(), PackageMaxFileBytes, bytes, existed);
            !read || !existed) {
            diagnostic.code = read ? AssetDiagnosticCode::MissingFile : AssetDiagnosticCode::None;
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic,
                                       read ? ErrorCode::NotFound : read.Error());
        }

        if (auto checked = CheckCookedPackageFile(entry, bytes.View(), &diagnostic); !checked) {
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic, checked.Error());
        }

        PackageReadOptions read;
        read.expectedKind = PackageKind::Cooked;
        auto contents = ReadPackage(allocator, bytes.View(), read, &diagnostic);
        if (!contents) {
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic,
                                       contents.Error());
        }

        if (auto checked =
                CheckCookedPackageContents(manifest, entry, contents.Value(), &diagnostic);
            !checked) {
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic, checked.Error());
        }

        FilePackageRangeReader sidecar;
        if (auto opened = sidecar.Open(files, sidecarPath.View()); !opened) {
            diagnostic = AssetDiagnostic{};
            diagnostic.code = opened.Error() == ErrorCode::NotFound
                                  ? AssetDiagnosticCode::MissingFile
                                  : AssetDiagnosticCode::None;
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic, opened.Error());
        }

        if (auto checked =
                VerifySidecarFile(sidecar, contents.Value(), scratch.View(), &diagnostic);
            !checked) {
            return WriteSidecarFailure(output, allocator, entry.asset, diagnostic, checked.Error());
        }

        ++verified;
    }

    return Result<int32>::Success(0);
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

    // 타입은 로드에만 쓴다(sidecar 검증은 타입 축을 보지 않는다).
    AssetCommandTypes types(allocator, *command.modules, command.typeModule);
    if (auto started = parsed.target.IsEmpty() ? Result<void>::Success() : types.Start();
        !started) {
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

    StringBuilder<LineBytes> line(allocator);
    if (!parsed.target.IsEmpty()) {
        auto loaded =
            LoadCookedTarget(command, types, manifest.Value(), root.View(), parsed, output, line);
        if (!loaded || loaded.Value() != 0) {
            return loaded;
        }
    }

    if (!parsed.verifySidecars) {
        return Write(output, CommandletStream::Output, line.View(), 0);
    }

    // 로드 줄을 먼저 낸다(sidecar 실패 줄이 그 뒤에 온다 — 결과는 sidecar 검증의 종료 코드).
    if (!line.View().IsEmpty()) {
        if (auto written = Write(output, CommandletStream::Output, line.View(), 0); !written) {
            return written;
        }
    }

    uint32 verified = 0;
    auto checked = VerifyCookedSidecars(output, allocator, manifest.Value(), root.View(), verified);
    if (!checked || checked.Value() != 0) {
        return checked;
    }

    StringBuilder<LineBytes> summary(allocator);
    if (auto made = summary.TryAppendFormat("{}: {} sidecars verified {}\n", LoadCookedName,
                                            parsed.profile, verified);
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Write(output, CommandletStream::Output, summary.View(), 0);
}

} // namespace rocks::commandlet_host
