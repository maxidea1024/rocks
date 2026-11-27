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
#include <Object/ObjectRegistry.h>
#include <Platform/Clock.h>
#include <Platform/File.h>

// asset-cook 명령 구현이다(Docs/Design/AssetSystem 13절, 10.2절 "A4e 구현 세부" — A4e1).
// 쿠킹 흐름은 CookProject(AssetCooker)가 맡고 여기서는 인수·세션·설정·너겟 캐시·출력만 다룬다.
namespace rocks::commandlet_host {

namespace {

constexpr StringView CookName = "asset-cook";
constexpr StringView CookUsage =
    "usage: asset-cook --profile=<id> [<path>...] [--verify-nuggets] "
    "[--verify-nuggets-source=local|remote] [--json] [--project=<dir>] [--wait=<seconds>]\n"
    "       asset-cook --engine --profile=<id> [--verify-nuggets] [--json] [--wait=<seconds>]\n";
// 엔진 전용 쿠킹의 Registry 문맥 id다(프로젝트 세션 없이 설정·보고서 struct를 읽고 쓴다).
constexpr ContextId EngineCookContext{0x454e47434f4f4b31ull, 1};
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
    // 엔진 전용 쿠킹(10.3.1절 A4f4 4번 — 프로젝트·경로·대조 원천과 함께 쓰지 않는다)이다.
    bool engine = false;
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

        bool* const flags[] = {&out.verify, &out.json, &out.engine};
        const StringView flagNames[] = {"--verify-nuggets", "--json", "--engine"};
        constexpr usize FlagCount = 3;
        bool matched = false;
        for (usize index = 0; index < FlagCount && !matched; ++index) {
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

    // 엔진 전용은 프로젝트·경로 인자·대조 원천(프로젝트 너겟 설정)과 함께 쓰지 않는다.
    if (out.engine && (!out.project.IsEmpty() || !out.paths.IsEmpty() || !out.source.IsEmpty())) {
        return Result<bool>::Success(false);
    }

    uint32 milliseconds = 0;
    return Result<bool>::Success(!out.profile.IsEmpty() &&
                                 ParseWaitMilliseconds(out.wait, milliseconds));
}

// `--wait` 마감 시각 하나에서 남은 밀리초다(10.3.1절 A4f4 1번).
uint32 RemainingWait(uint32 waitMilliseconds, uint64 startNanoseconds) noexcept {
    return RemainingWaitMilliseconds(waitMilliseconds, startNanoseconds,
                                     platform::MonotonicNanoseconds());
}

// 쿠킹 루트 잠금을 잡는다(실패하면 줄을 쓰고 failure에 종료 코드를 둔다).
bool LockCookRoot(CommandletOutput& output, Allocator& allocator, CookRootLock& lock,
                  StringView saved, StringView profile, StringView root, uint32 waitMilliseconds,
                  Result<int32>& failure) noexcept {
    if (auto held = lock.Acquire(allocator, saved, profile, waitMilliseconds); !held) {
        failure = held.Error() == ErrorCode::OutOfMemory ? Result<int32>::Failure(held.Error())
                  : held.Error() == ErrorCode::Busy
                      ? Fail(output, allocator, CookName, 1,
                             "Busy: {}: the cook root is locked by another process; use "
                             "--wait=<seconds>",
                             root)
                      : Fail(output, allocator, CookName, 1, "{}: {}: cannot lock the cook root",
                             ErrorCodeName(held.Error()), root);
        return false;
    }

    return true;
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
    case CookConfigField::GraphicsBackends:
        return out.TryAppendFormat("invalid graphicsBackends[{}]", failure.index);
    case CookConfigField::ShaderFormats:
        // index는 backend 번호다(목록에 없는 키만 Map 순서 번호 — 10.3.1절 판정 순서).
        return out.TryAppendFormat("invalid shaderFormats (entry {})", failure.index);
    case CookConfigField::TextureLodGroup:
        // index는 TextureGroup 값이다(12.14절 검증 — 크기·mipGen·mipLoadOptions).
        return out.TryAppendFormat("invalid textureLodGroups entry (TextureGroup value {})",
                                   failure.index);
    case CookConfigField::AudioCook: {
        // index는 AudioCookFieldIndex다(Docs/Design/AudioSystem 3.39.1절 — 이름은 그 순서).
        constexpr const char* AudioCookFieldNames[] = {
            "resampleForDevice",         "maxSampleRate", "highSampleRate",
            "mediumSampleRate",          "lowSampleRate", "minSampleRate",
            "compressionQualityModifier"};
        constexpr uint32 AudioCookFieldCount =
            sizeof(AudioCookFieldNames) / sizeof(AudioCookFieldNames[0]);
        static_assert(AudioCookFieldCount == static_cast<uint32>(AudioCookFieldIndex::Count),
                      "audioCook field names must match AudioCookFieldIndex");
        return failure.index < AudioCookFieldCount
                   ? out.TryAppendFormat("invalid audioCook.{}", AudioCookFieldNames[failure.index])
                   : out.TryAppend("invalid audioCook");
    }
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
        auto written = Fail(output, allocator, CookName, 1, "{}: {}: {}",
                            AssetTextDiagnosticName(entry.code), entry.path, entry.message);
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
// 진단은 부분 쿠킹의 인자 경로·전체 쿠킹의 `alwaysCook[<순번>]`, 그 밖은 쿠킹 루트다. roots는
// 실제로 부분 쿠킹했을 때만 인자 경로다(기존 매니페스트가 없거나 깨져 전체 쿠킹으로 바뀌면 비어
// 있다 — 그때 순번은 alwaysCook 순번이다).
Result<void> AppendTarget(StringBuilderBase& out, const AssetIndex* index, AssetId asset,
                          const CookDiagnostic& diagnostic, Span<const StringView> roots,
                          StringView root) noexcept {
    if (!asset.IsNull()) {
        if (index == nullptr) {
            return out.TryAppendFormat("{}", asset);
        }

        if (auto found = index->FindById(asset); found) {
            return out.TryAppend(found.Value()->path.View());
        }

        return out.TryAppendFormat("{}", asset);
    }

    const bool pathCode = diagnostic.code == AssetDiagnosticCode::InvalidPath ||
                          diagnostic.code == AssetDiagnosticCode::DuplicatePath;
    if (pathCode && diagnostic.setting < roots.Size()) {
        return out.TryAppend(roots[diagnostic.setting]);
    }

    if (pathCode) {
        return out.TryAppendFormat("alwaysCook[{}]", diagnostic.setting);
    }

    return out.TryAppend(root);
}

// 쿠킹 진단을 한 줄씩 쓴다.
Result<int32> WriteDiagnostics(CommandletOutput& output, Allocator& allocator,
                               const AssetIndex* index, const CookProjectResult& result,
                               Span<const StringView> paths, StringView root) noexcept {
    const Span<const StringView> roots = result.partial ? paths : Span<const StringView>{};
    for (const CookDiagnostic& diagnostic : result.diagnostics) {
        StringBuilder<LineBytes> line(allocator);
        const char* code = diagnostic.code != AssetDiagnosticCode::None
                               ? AssetDiagnosticName(diagnostic.code)
                               : ErrorCodeName(diagnostic.error);
        Result<void> made = line.TryAppendFormat("{}: ", code);
        if (made) {
            made = AppendTarget(line, index, diagnostic.asset, diagnostic, roots, root);
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
                made = AppendTarget(line, index, diagnostic.related, diagnostic, roots, root);
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

// 쿠킹 결과를 쓴다: 함수 오류는 실패 줄(단계가 멈췄으면 정체성·호출·형식), 그 밖은 진단 줄 →
// `--json`이면 보고서, 아니면 요약 줄. 종료 코드는 게시했으면 0이다.
Result<int32> WriteCookOutcome(CommandletOutput& output, Allocator& allocator,
                               platform::FileSystem& files, const AssetIndex* index,
                               Span<const StringView> paths, StringView root, StringView profile,
                               bool json, const Result<void>& cooked,
                               const CookProjectResult& result) noexcept {
    if (!cooked) {
        if (cooked.Error() == ErrorCode::OutOfMemory) {
            return Result<int32>::Failure(cooked.Error());
        }

        // 단계가 멈췄으면 그 정체성·호출·형식을 적는다(교차 검증 3 — 보고서가 없다).
        const CookArtifactFailure& stage = result.stageFailure;
        if (stage.call != CookArtifactCall::None) {
            return Fail(output, allocator, CookName, 1, "{}: {}: stage {} {}({}): the cook failed",
                        ErrorCodeName(cooked.Error()), root, stage.stage,
                        CookArtifactCallName(stage.call), stage.formatId.View());
        }

        return Fail(output, allocator, CookName, 1, "{}: {}: the cook failed",
                    ErrorCodeName(cooked.Error()), root);
    }

    if (auto written = WriteDiagnostics(output, allocator, index, result, paths, root); !written) {
        return written;
    }

    const int32 code = result.published ? 0 : 1;
    if (json) {
        StringBuilder<LineBytes> path(allocator);
        if (auto made = path.TryAppendFormat("{}/{}", root, CookReportFileName); !made) {
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
            "{}: {} {} targets {} cooked {} reused {} bytes {}{}\n", CookName, profile,
            result.published ? "published" : "not published", result.targets, result.cooked,
            result.reused, result.bytesWritten, result.partial ? " partial" : "");
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    return Write(output, CommandletStream::Output, summary.View(), code);
}

// 명령이 끝날 때 너겟 캐시를 닫는다.
struct NuggetShutdown {
    ~NuggetShutdown() noexcept {
        cache.Shutdown(NuggetShutdownDrainMilliseconds);
    }

    NuggetCache& cache;
};

// 엔진 디렉터리다(문맥 재정의가 있으면 그것 — 명령 시험, 없으면 FindEngineDirectory). 못 찾으면
// 줄을 쓰고 failure에 종료 코드를 둔다. storage는 찾은 경로의 저장소다.
bool ResolveEngineDirectory(const AssetCommandContext& command, CommandletOutput& output,
                            Allocator& allocator, Array<char>& storage, StringView& out,
                            Result<int32>& failure) noexcept {
    if (!command.engineDirectory.IsEmpty()) {
        out = command.engineDirectory;
        return true;
    }

    if (auto sized = storage.TryResize(MaxNativePathBytes() + 1); !sized) {
        failure = Result<int32>::Failure(sized.Error());
        return false;
    }

    auto found = FindEngineDirectory(Span<char>(storage.Data(), storage.Size()));
    if (!found) {
        failure = Fail(output, allocator, CookName, 1, "{}: Engine/Config: cannot find the engine",
                       ErrorCodeName(found.Error()));
        return false;
    }

    out = StringView(storage.Data(), found.Value());
    return true;
}

// 엔진 전용 쿠킹이다(10.3.1절 A4f4 4번): 프로젝트 세션 없이 에셋 타입·쿠커 모듈을 시작하고 자기
// ObjectRegistry로 엔진 프로필(프로젝트 재정의 없음)·엔진 CookSettings를 읽어 엔진 쿠킹 루트
// (`Engine/Saved/Cooked/<프로필>`)에 게시한다.
Result<int32> RunEngineCook(const AssetCommandContext& command, const CookArgs& parsed,
                            uint32 waitMilliseconds, uint64 waitStart,
                            CommandletOutput& output) noexcept {
    Allocator& allocator = *command.allocator;
    AssetCommandTypes types(allocator, *command.modules, command.typeModule);
    if (auto started = types.Start(); !started) {
        return started.Error() == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(started.Error())
                   : Fail(output, allocator, CookName, 1, "{}: {}: cannot register the asset types",
                          ErrorCodeName(started.Error()), types.Module());
    }

    if (auto loaded = command.modules->Load(CookerModule); !loaded.IsOk()) {
        const ErrorCode error =
            loaded.error != ErrorCode::None ? loaded.error : ErrorCode::Unsupported;
        return error == ErrorCode::OutOfMemory
                   ? Result<int32>::Failure(error)
                   : Fail(output, allocator, CookName, 1, "{}: {}: cannot start the cooker module",
                          ErrorCodeName(error), CookerModule);
    }

    Result<int32> failure = Result<int32>::Success(1);
    Array<char> engine(allocator);
    StringView engineDirectory;
    if (!ResolveEngineDirectory(command, output, allocator, engine, engineDirectory, failure)) {
        return failure;
    }

    StringBuilder<LineBytes> engineConfig(allocator);
    StringBuilder<LineBytes> saved(allocator);
    if (auto made = engineConfig.TryAppendFormat("{}/Config", engineDirectory); !made) {
        return Result<int32>::Failure(made.Error());
    }

    if (auto made = saved.TryAppendFormat("{}/Saved", engineDirectory); !made) {
        return Result<int32>::Failure(made.Error());
    }

    platform::FileSystem& files = platform::PhysicalFileSystem();
    ObjectRegistry registry(allocator, types.Types(), EngineCookContext);
    // 프로필은 엔진 파일만, CookSettings는 엔진 Config의 파일(있으면)이다.
    const CookConfigContext profileContext{
        &allocator,          &files,      &registry, registry.OwnerAccess(), &types.Types(),
        engineConfig.View(), StringView()};
    CookConfigContext settingsContext = profileContext;
    settingsContext.projectConfigDirectory = engineConfig.View();
    reflection_text::TextDiagnostics diagnostics(allocator);
    TargetProfile profile(allocator);
    CookSettings settings(allocator);
    CookConfigFailure configFailure;
    if (auto read =
            ReadTargetProfile(profileContext, parsed.profile, profile, diagnostics, &configFailure);
        !read) {
        return WriteConfigFailure(output, allocator, profileContext, parsed.profile, diagnostics,
                                  configFailure, read.Error());
    }

    if (auto read = ReadCookSettings(settingsContext, settings, diagnostics, &configFailure);
        !read) {
        return WriteConfigFailure(output, allocator, settingsContext, parsed.profile, diagnostics,
                                  configFailure, read.Error());
    }

    // 두 struct는 위에서 읽혔으므로 등록되어 있다 — 해시 실패는 할당 실패뿐이다.
    auto profileHash = HashTargetProfile(profileContext, profile);
    if (!profileHash) {
        return Result<int32>::Failure(profileHash.Error());
    }

    auto settingsHash = HashCookSettings(settingsContext, settings);
    if (!settingsHash) {
        return Result<int32>::Failure(settingsHash.Error());
    }

    StringBuilder<LineBytes> root(allocator);
    if (auto made = AppendCookedRoot(root, saved.View(), profile.id.View()); !made) {
        return Result<int32>::Failure(made.Error());
    }

    CookRootLock cookLock;
    if (!LockCookRoot(output, allocator, cookLock, saved.View(), profile.id.View(), root.View(),
                      RemainingWait(waitMilliseconds, waitStart), failure)) {
        return failure;
    }

    CookEngineRequest request;
    request.files = &files;
    request.savedDirectory = saved.View();
    request.cookLock = &cookLock;
    request.types = &types.Types();
    request.registry = &registry;
    request.access = registry.OwnerAccess();
    request.profile = &profile;
    request.profileSettings = profileHash.Value();
    request.settings = &settings;
    request.cookSettings = settingsHash.Value();
    request.cookedBy = build::CurrentEngineVersion();
    request.verifyNuggets = parsed.verify;
    request.stages = &CookArtifactStages();
    CookProjectResult result(allocator);
    const Result<void> cooked = CookEngine(allocator, request, result);
    return WriteCookOutcome(output, allocator, files, nullptr, Span<const StringView>(),
                            root.View(), profile.id.View(), parsed.json, cooked, result);
}

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

    // `--wait` 마감 시각은 하나다(에셋 DB 잠금 → 쿠킹 루트 잠금 — 10.3.1절 A4f4 1번).
    uint32 waitMilliseconds = 0;
    (void)ParseWaitMilliseconds(parsed.wait, waitMilliseconds);
    const uint64 waitStart = platform::MonotonicNanoseconds();
    if (parsed.engine) {
        return RunEngineCook(command, parsed, waitMilliseconds, waitStart, output);
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
    StringView engineDirectory;
    if (!ResolveEngineDirectory(command, output, allocator, engine, engineDirectory, failure)) {
        return failure;
    }

    StringBuilder<LineBytes> engineConfig(allocator);
    if (auto made = engineConfig.TryAppendFormat("{}/Config", engineDirectory); !made) {
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
    if (auto made = AppendCookedRoot(root, session.layout.SavedDirectory(), profile.id.View());
        !made) {
        return Result<int32>::Failure(made.Error());
    }

    DefaultCookProcessor processor;
    CookProcessorTable processors(allocator, session.types.Types(), processor);
    if (command.cookProcessors.registerProcessors != nullptr) {
        if (auto registered =
                command.cookProcessors.registerProcessors(command.cookProcessors.state, processors);
            !registered) {
            return registered.Error() == ErrorCode::OutOfMemory
                       ? Result<int32>::Failure(registered.Error())
                       : Fail(output, allocator, CookName, 1, "{}: cannot register cook processors",
                              ErrorCodeName(registered.Error()));
        }
    }
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
    // 비에셋 산출물 단계(셰이더 라이브러리 — 구현 모듈이 시작 hook에서 등록, 10.3.1절 A4f3 6번)다.
    request.stages = &CookArtifactStages();
    CookProjectResult result(allocator);
    CookRootLock cookLock;
    if (!LockCookRoot(output, allocator, cookLock, session.layout.SavedDirectory(),
                      profile.id.View(), root.View(), RemainingWait(waitMilliseconds, waitStart),
                      failure)) {
        return failure;
    }

    request.cookLock = &cookLock;
    // 처리기 안 병렬(텍스처 블록 압축)은 쿠킹 전용 scheduler로 한다(12.17.9 4 — 잠금을 잡은 뒤에
    // 띄운다). scratch는 명령의 allocator(프로세스 기본 — 여러 스레드 동시 호출 안전)다.
    CookWorkScheduler cookWork(allocator);
    request.scheduler = cookWork.Get();
    request.scratch = &allocator;
    const Result<void> cooked = CookProject(allocator, request, result);
    return WriteCookOutcome(output, allocator, files, &session.service->Index(),
                            parsed.paths.View(), root.View(), profile.id.View(), parsed.json,
                            cooked, result);
}

} // namespace rocks::commandlet_host
