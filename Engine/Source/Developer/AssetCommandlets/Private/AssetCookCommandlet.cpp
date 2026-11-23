#include <AssetCommandlets/AssetCookCommandlet.h>
#include <AssetCommandlets/AssetCommandletSupport.h>

#include <AssetCooker/CookConfig.h>
#include <AssetCooker/CookProject.h>
#include <AssetNugget/NuggetCache.h>
#include <Core/Build/BuildInfo.h>
#include <Core/FileRead.h>
#include <Core/Text/Path.h>
#include <HostConfig/HostSettings.h>
#include <ModuleLoader/ModuleLoader.h>
#include <Platform/File.h>

// asset-cook 명령 구현이다(Docs/Design/AssetSystem.ko.md 13절, 10.2절 "A4e 구현 세부" — A4e1).
// 쿠킹 흐름은 CookProject(AssetCooker)가 맡고 여기서는 인수·세션·설정·너겟 캐시·출력만 다룬다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView CookName = "asset-cook";
constexpr StringView CookUsage =
    "usage: asset-cook --profile=<id> [<path>...] [--verify-nuggets] "
    "[--verify-nuggets-source=local|remote] [--json] [--project=<dir>] [--wait=<seconds>]\n";
// 쿠킹 설정·보고서 struct를 등록하는 모듈이다.
constexpr StringView CookerModule = "AssetCooker";
constexpr StringView VerifySourceLocal = "local";
constexpr StringView VerifySourceRemote = "remote";
// 명령 끝 너겟 캐시 종료의 Put 대기 기한이다(처리기는 동기 Get·Put만 써서 진행 중 요청이 없다 —
// 방어적 기한).
constexpr uint32 NuggetShutdownDrainMilliseconds = 10000;
// --json이 읽어 쓰는 보고서 크기 상한이다.
constexpr uint64 CookReportPrintMaxBytes = 64ull * 1024 * 1024;

// asset-cook의 인자다.
struct CookArgs {
    explicit CookArgs(Allocator& allocator) noexcept : paths(allocator) {
    }

    StringView project;
    StringView wait;
    StringView profile;
    StringView source;
    bool verify = false;
    bool json = false;
    bool help = false;
    Array<StringView> paths;
};

// 인자를 읽는다. 사용법 위반은 false, 할당 실패는 오류다.
Result<bool> ParseCook(Span<const StringView> args, CookArgs& out) noexcept {
    if (args.Size() == 1 && args[0] == "--help") {
        out.help = true;
        return Result<bool>::Success(true);
    }

    for (StringView argument : args) {
        if (!argument.StartsWith("--", StringComparison::CaseSensitive)) {
            if (argument.IsEmpty()) {
                return Result<bool>::Success(false);
            }

            if (auto pushed = out.paths.TryPushBack(argument); !pushed) {
                return Result<bool>::Failure(pushed.Error());
            }

            continue;
        }

        bool* const flags[] = {&out.verify, &out.json};
        const StringView flagNames[] = {"--verify-nuggets", "--json"};
        bool matched = false;
        for (usize index = 0; index < 2 && !matched; ++index) {
            if (argument == flagNames[index]) {
                if (*flags[index]) {
                    return Result<bool>::Success(false);
                }

                *flags[index] = true;
                matched = true;
            }
        }

        StringView* const slots[] = {&out.project, &out.wait, &out.profile, &out.source};
        const StringView names[] = {
            "--project=", "--wait=", "--profile=", "--verify-nuggets-source="};
        for (usize index = 0; index < 4 && !matched; ++index) {
            if (argument.StartsWith(names[index], StringComparison::CaseSensitive)) {
                const StringView value = argument.SliceBytes(
                    names[index].ByteCount(), argument.ByteCount() - names[index].ByteCount());
                if (!slots[index]->IsEmpty() || value.IsEmpty()) {
                    return Result<bool>::Success(false);
                }

                *slots[index] = value;
                matched = true;
            }
        }

        if (!matched) {
            return Result<bool>::Success(false);
        }
    }

    // source는 verify와 함께만, local·remote만 받는다(효과 없는 옵션을 조용히 받지 않는다).
    if (!out.source.IsEmpty() &&
        (!out.verify || (out.source != VerifySourceLocal && out.source != VerifySourceRemote))) {
        return Result<bool>::Success(false);
    }

    uint32 milliseconds = 0;
    return Result<bool>::Success(!out.profile.IsEmpty() &&
                                 ParseWaitMilliseconds(out.wait, milliseconds));
}

// 설정 실패 파일의 경로다(파일 밖 판정 — id 형식·합친 값 — 은 `TargetProfiles/<id>`).
Result<void> AppendConfigFile(StringBuilderBase& out, const CookConfigContext& context,
                              StringView profile, CookConfigFile file) noexcept {
    switch (file) {
    case CookConfigFile::EngineProfile:
        return out.TryAppendFormat("{}/{}/{}.json", context.engineConfigDirectory,
                                   TargetProfileDirectory, profile);
    case CookConfigFile::ProjectProfile:
        return out.TryAppendFormat("{}/{}/{}.json", context.projectConfigDirectory,
                                   TargetProfileDirectory, profile);
    case CookConfigFile::CookSettings:
        return out.TryAppendFormat("{}/{}", context.projectConfigDirectory, CookSettingsFile);
    case CookConfigFile::None:
        break;
    }

    return out.TryAppendFormat("{}/{}", TargetProfileDirectory, profile);
}

// 설정 실패 칸의 설명이다(배열 칸은 순번을 붙인다).
Result<void> AppendConfigField(StringBuilderBase& out, const CookConfigFailure& failure) noexcept {
    switch (failure.field) {
    case CookConfigField::File:
        return out.TryAppend("the file is too large");
    case CookConfigField::Json:
        return out.TryAppendFormat("invalid JSON at {}:{}", failure.line, failure.column);
    case CookConfigField::Text:
        return out.TryAppend("invalid values");
    case CookConfigField::Id:
        return out.TryAppend("invalid id");
    case CookConfigField::BulkAlignment:
        return out.TryAppend("invalid bulkAlignment");
    case CookConfigField::AlwaysCook:
        return out.TryAppendFormat("invalid alwaysCook[{}]", failure.index);
    case CookConfigField::NeverCook:
        return out.TryAppendFormat("invalid neverCook[{}]", failure.index);
    case CookConfigField::PrimaryAssetType:
        return out.TryAppendFormat("invalid primaryAssets[{}].type", failure.index);
    case CookConfigField::PrimaryAssetDirectory:
        return out.TryAppendFormat("invalid primaryAssets[{}].directories[{}]", failure.index,
                                   failure.item);
    case CookConfigField::None:
        break;
    }

    // 칸이 없는 거부는 엔진 프로파일 파일이 없는 것이다(나머지는 읽기 오류).
    return out.TryAppend(failure.code != AssetDiagnosticCode::None ? "the file does not exist"
                                                                   : "cannot read");
}

// 설정 읽기 실패를 쓴다: 값 대입 진단 줄들과 `<코드>: <파일>: <칸> (<원인 오류>)` 한 줄이다.
Result<int32> WriteConfigFailure(CommandletOutput& output, Allocator& allocator,
                                 const CookConfigContext& context, StringView profile,
                                 const reflection_text::TextDiagnostics& diagnostics,
                                 const CookConfigFailure& failure, ErrorCode error) noexcept {
    if (error == ErrorCode::OutOfMemory) {
        return Result<int32>::Failure(error);
    }

    for (usize index = 0; index < diagnostics.Count(); ++index) {
        const auto entry = diagnostics.Get(index);
        auto written =
            Fail(output, allocator, CookName, 1, "{}: {}: {}",
                 reflection_text::TextDiagnosticCodeName(entry.code), entry.path, entry.message);
        if (!written) {
            return written;
        }
    }

    StringBuilder<LineBytes> file(allocator);
    StringBuilder<LineBytes> field(allocator);
    if (auto made = AppendConfigFile(file, context, profile, failure.file); !made) {
        return Result<int32>::Failure(made.Error());
    }

    if (auto made = AppendConfigField(field, failure); !made) {
        return Result<int32>::Failure(made.Error());
    }

    const char* code = failure.code != AssetDiagnosticCode::None ? AssetDiagnosticName(failure.code)
                                                                 : ErrorCodeName(error);
    return Fail(output, allocator, CookName, 1, "{}: {}: {} ({})", code, file.View(), field.View(),
                ErrorCodeName(error));
}

// 너겟 설정 거부 칸의 설명이다.
Result<void> AppendNuggetField(StringBuilderBase& out,
                               const NuggetSettingsFailure& failure) noexcept {
    switch (failure.field) {
    case NuggetSettingsField::File:
        return out.TryAppend("the file is too large");
    case NuggetSettingsField::Json:
        return out.TryAppendFormat("invalid JSON at {}:{}", failure.line, failure.column);
    case NuggetSettingsField::Root:
        return out.TryAppend("the root is not an object");
    case NuggetSettingsField::UnknownKey:
        return out.TryAppend("unknown key");
    case NuggetSettingsField::Backend:
        return out.TryAppend("invalid backend");
    case NuggetSettingsField::Required:
        return out.TryAppend("invalid required");
    case NuggetSettingsField::LocalPath:
        return out.TryAppend("invalid localPath");
    case NuggetSettingsField::Generation:
        return out.TryAppend("invalid generation");
    case NuggetSettingsField::None:
        break;
    }

    return out.TryAppend("cannot read");
}

// 진단 대상 텍스트다: 에셋이면 색인의 논리 경로(고유하게 없으면 AssetId), 에셋이 없는 경로
// 진단은 부분 쿠킹의 인자 경로·전체 쿠킹의 `alwaysCook[<순번>]`, 그 밖은 쿠킹 루트다.
Result<void> AppendTarget(StringBuilderBase& out, const AssetIndex& index, AssetId asset,
                          const CookDiagnostic& diagnostic, Span<const StringView> paths,
                          StringView root) noexcept {
    if (!asset.IsNull()) {
        if (auto found = index.FindById(asset); found) {
            return out.TryAppend(found.Value()->path.View());
        }

        return out.TryAppendFormat("{}", asset);
    }

    const bool pathCode = diagnostic.code == AssetDiagnosticCode::InvalidPath ||
                          diagnostic.code == AssetDiagnosticCode::DuplicatePath;
    if (pathCode && paths.Size() != 0) {
        return out.TryAppend(paths[diagnostic.setting]);
    }

    if (pathCode) {
        return out.TryAppendFormat("alwaysCook[{}]", diagnostic.setting);
    }

    return out.TryAppend(root);
}

// 쿠킹 진단을 한 줄씩 쓴다.
Result<int32> WriteDiagnostics(CommandletOutput& output, Allocator& allocator,
                               const AssetIndex& index, const CookProjectResult& result,
                               Span<const StringView> paths, StringView root) noexcept {
    for (const CookDiagnostic& diagnostic : result.diagnostics) {
        StringBuilder<LineBytes> line(allocator);
        const char* code = diagnostic.code != AssetDiagnosticCode::None
                               ? AssetDiagnosticName(diagnostic.code)
                               : ErrorCodeName(diagnostic.error);
        Result<void> made = line.TryAppendFormat("{}: ", code);
        if (made) {
            made = AppendTarget(line, index, diagnostic.asset, diagnostic, paths, root);
        }

        if (made) {
            made = line.TryAppendFormat(": {}", diagnostic.warning ? "warning" : "error");
        }

        if (made && diagnostic.code != AssetDiagnosticCode::None &&
            diagnostic.error != ErrorCode::None) {
            made = line.TryAppendFormat(" {}", ErrorCodeName(diagnostic.error));
        }

        if (made && !diagnostic.related.IsNull()) {
            made = line.TryAppend(" (related ");
            if (made) {
                made = AppendTarget(line, index, diagnostic.related, diagnostic, paths, root);
            }

            if (made) {
                made = line.TryAppend(")");
            }
        }

        if (!made) {
            return Result<int32>::Failure(made.Error());
        }

        if (auto written = Fail(output, allocator, CookName, 0, "{}", line.View()); !written) {
            return written;
        }
    }

    return Result<int32>::Success(0);
}

// 명령이 끝날 때 너겟 캐시를 닫는다.
struct NuggetShutdown {
    ~NuggetShutdown() noexcept {
        cache.Shutdown(NuggetShutdownDrainMilliseconds);
    }

    NuggetCache& cache;
};

} // namespace

Result<int32> RunAssetCookCommand(void* context, Span<const StringView> args,
                                  CommandletOutput& output) noexcept {
    const AssetCommandContext& command = *static_cast<const AssetCommandContext*>(context);
    Allocator& allocator = *command.allocator;
    CookArgs parsed(allocator);
    auto valid = ParseCook(args, parsed);
    if (!valid) {
        return Result<int32>::Failure(valid.Error());
    }

    if (parsed.help) {
        return Write(output, CommandletStream::Output, CookUsage, 0);
    }

    if (!valid.Value()) {
        return Write(output, CommandletStream::Error, CookUsage, 2);
    }

    // 원격 대조는 Rockpile backend가 생기기 전까지 받지 않는다(9.1절 결정 16).
    if (parsed.source == VerifySourceRemote) {
        return Fail(output, allocator, CookName, 1,
                    "Unsupported: remote: the Rockpile nugget backend is not available");
    }

    EditSession session(command);
    Result<int32> failure = Result<int32>::Success(1);
    if (!OpenSession(output, allocator, CookName, parsed.project, parsed.wait, true, session,
                     failure)) {
        return failure;
    }

    // 세션 등록기가 붙어 있으므로 쿠커 모듈의 타입(설정·보고서 struct)이 세션 Registry에 들어간다.
    if (auto loaded = command.modules->Load(CookerModule); !loaded.IsOk()) {
        const ErrorCode error =
            loaded.error != ErrorCode::None ? loaded.error : ErrorCode::Unsupported;
        return error == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(error)
                   : Fail(output, allocator, CookName, 1, "{}: {}: cannot start the cooker module",
                          ErrorCodeName(error), CookerModule);
    }

    Array<char> engine(allocator);
    if (auto sized = engine.TryResize(MaxNativePathBytes() + 1); !sized) {
        return Result<int32>::Failure(sized.Error());
    }

    auto found = FindEngineDirectory(Span<char>(engine.Data(), engine.Size()));
    if (!found) {
        return Fail(output, allocator, CookName, 1, "{}: Engine/Config: cannot find the engine",
                    ErrorCodeName(found.Error()));
    }

    StringBuilder<LineBytes> engineConfig(allocator);
    if (auto made =
            engineConfig.TryAppendFormat("{}/Config", StringView(engine.Data(), found.Value()));
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    EditorPackageSet& packages = session.service->Packages();
    CookConfigContext config{&allocator,
                             &files,
                             &packages.Registry(),
                             packages.Access(),
                             &session.types.Types(),
                             engineConfig.View(),
                             session.layout.ConfigDirectory()};
    TargetProfile profile(allocator);
    CookSettings settings(allocator);
    CookConfigFailure configFailure;
    if (auto read =
            ReadTargetProfile(config, parsed.profile, profile, session.diagnostics, &configFailure);
        !read) {
        return WriteConfigFailure(output, allocator, config, parsed.profile, session.diagnostics,
                                  configFailure, read.Error());
    }

    if (auto read = ReadCookSettings(config, settings, session.diagnostics, &configFailure);
        !read) {
        return WriteConfigFailure(output, allocator, config, parsed.profile, session.diagnostics,
                                  configFailure, read.Error());
    }

    // 두 struct는 위에서 읽혔으므로 등록되어 있다 — 해시 실패는 할당 실패뿐이다.
    auto profileHash = HashTargetProfile(config, profile);
    if (!profileHash) {
        return Result<int32>::Failure(profileHash.Error());
    }

    auto settingsHash = HashCookSettings(config, settings);
    if (!settingsHash) {
        return Result<int32>::Failure(settingsHash.Error());
    }

    NuggetSettingsFailure nuggetFailure;
    auto nuggetSettings =
        ReadNuggetCacheSettings(allocator, files, session.layout.Root(), &nuggetFailure);
    if (!nuggetSettings) {
        if (nuggetSettings.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(nuggetSettings.Error());
        }

        StringBuilder<LineBytes> field(allocator);
        if (auto made = AppendNuggetField(field, nuggetFailure); !made) {
            return Result<int32>::Failure(made.Error());
        }

        // 거부 칸이 없으면 읽기 오류다(코드 자리에 원인 오류).
        const char* code = nuggetFailure.field != NuggetSettingsField::None
                               ? AssetDiagnosticName(AssetDiagnosticCode::InvalidSetting)
                               : ErrorCodeName(nuggetSettings.Error());
        return Fail(output, allocator, CookName, 1, "{}: {}/{}: {} ({})", code,
                    session.layout.Root(), NuggetSettingsFile, field.View(),
                    ErrorCodeName(nuggetSettings.Error()));
    }

    String resolved(allocator);
    if (auto made = ResolveNuggetLocalPath(nuggetSettings.Value(), session.layout.Root(), resolved);
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    NuggetSystemClock clock;
    NuggetCache nuggets(allocator, files, clock);
    NuggetShutdown shutdown{nuggets};
    if (auto opened = nuggets.Open(nuggetSettings.Value(), session.layout.Root()); !opened) {
        return opened.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(opened.Error())
                   : Fail(output, allocator, CookName, 1, "{}: {}: cannot open the nugget cache",
                          ErrorCodeName(opened.Error()), resolved.View());
    }

    if (nuggets.OpenError() != ErrorCode::None) {
        if (auto written =
                Fail(output, allocator, CookName, 0, "{}: {}: warning NuggetCacheDisabled",
                     ErrorCodeName(nuggets.OpenError()), resolved.View());
            !written) {
            return written;
        }
    }

    StringBuilder<LineBytes> root(allocator);
    if (auto made = AppendCookedRoot(root, session.layout, profile.id.View()); !made) {
        return Result<int32>::Failure(made.Error());
    }

    DefaultCookProcessor processor;
    CookProcessorTable processors(allocator, session.types.Types(), processor);
    CookProjectRequest request;
    request.files = &files;
    request.layout = &session.layout;
    request.lock = &session.lock;
    request.index = &session.service->Index();
    request.packages = &packages;
    request.types = &session.types.Types();
    request.processors = &processors;
    request.profile = &profile;
    request.profileSettings = profileHash.Value();
    request.settings = &settings;
    request.cookSettings = settingsHash.Value();
    request.paths = parsed.paths.View();
    request.projectGeneration = nuggets.ProjectGeneration();
    request.cookedBy = build::CurrentEngineVersion();
    request.nuggets = &nuggets;
    request.verifyNuggets = parsed.verify;
    CookProjectResult result(allocator);
    if (auto cooked = CookProject(allocator, request, result); !cooked) {
        return cooked.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(cooked.Error())
                   : Fail(output, allocator, CookName, 1, "{}: {}: the cook failed",
                          ErrorCodeName(cooked.Error()), root.View());
    }

    if (auto written = WriteDiagnostics(output, allocator, session.service->Index(), result,
                                        parsed.paths.View(), root.View());
        !written) {
        return written;
    }

    const int32 code = result.published ? 0 : 1;
    if (parsed.json) {
        StringBuilder<LineBytes> path(allocator);
        if (auto made = path.TryAppendFormat("{}/{}", root.View(), CookReportFileName); !made) {
            return Result<int32>::Failure(made.Error());
        }

        Array<uint8> report(allocator);
        bool existed = false;
        if (auto read = ReadWholeFile(files, path.View(), CookReportPrintMaxBytes, report, existed);
            !read || !existed) {
            const ErrorCode error = read ? ErrorCode::NotFound : read.Error();
            return error == ErrorCode::OutOfMemory
                       ? Result<int32>::Failure(error)
                       : Fail(output, allocator, CookName, 1, "{}: {}: cannot read the cook report",
                              ErrorCodeName(error), path.View());
        }

        return Write(output, CommandletStream::Output,
                     StringView(reinterpret_cast<const char*>(report.Data()), report.Size()), code);
    }

    StringBuilder<LineBytes> summary(allocator);
    if (auto made = summary.TryAppendFormat(
            "{}: {} {} targets {} cooked {} reused {} bytes {}{}\n", CookName, profile.id.View(),
            result.published ? "published" : "not published", result.targets, result.cooked,
            result.reused, result.bytesWritten, result.partial ? " partial" : "");
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Write(output, CommandletStream::Output, summary.View(), code);
}

} // namespace rocks::commandlet_host
