#include <ShaderCompiler/ShaderLibraryCookStage.h>

#include "ShaderCompileReflection.h"

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Core/Diagnostics/Log.h>
#include <Core/FileError.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/StringBuilder.h>
#include <HostConfig/HostSettings.h>
#include <ShaderCompiler/ShaderNugget.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <TargetProfiles/TargetProfile.h>
#include <stdio.h>
#include <string.h>

// 셰이더 라이브러리 비에셋 산출물 단계(Docs/Design/ShaderSystem 16.1절 SSD44–SSD46)다. 규칙은 공개
// 헤더 주석을 따른다.
namespace rocks::shader {

namespace cook_stage_detail {

constexpr LogCategory LogShaderLibraryCook{"ShaderCompiler"};
// 경로 버퍼 byte 수다.
constexpr usize StagePathBytes = 4096;
// 단계 전용 레지스트리 문맥 id다("SHCS").
constexpr uint32 CookStageContextId = 0x53484353;
// 워커 작업 디렉터리 이름(staging 아래)이다.
constexpr char WorkerDirectoryName[] = "ShaderWorkers";
// 쿠킹하지 않는 미리보기 형식이다(12.3절 — 출시 프로필·엔진 내장 라이브러리에 넣지 않는다).
constexpr char PreviewFormatId[] = "GLES31_PC";
// 문자열 저장소 byte 수(경로 builder 인라인)다.
constexpr usize StageInlineBytes = 512;

// 원천이 넘긴 머티리얼 셰이더 묶음의 사본이다(16.5절 SSD87).
struct OwnedBundle {
    explicit OwnedBundle(Allocator& allocator) noexcept
        : formatId(allocator), source(allocator), traits(allocator), inputs(allocator) {
    }

    String formatId;
    String source;
    Array<uint8> traits;
    Array<VertexInputTypeId> inputs;
    Sha256Digest shaderSetId{};
    AssetId asset{};
};

// 원천의 출력을 사본으로 받고 진단을 쿠커 sink로 넘긴다.
class StageBundleSink final : public MaterialShaderBundleSink {
public:
    StageBundleSink(Allocator& allocator, Array<OwnedBundle>& out, CookArtifactSink& sink) noexcept
        : allocator_(allocator), out_(out), sink_(sink) {
    }

    Result<void> Add(const MaterialShaderBundle& bundle) noexcept override {
        if (!IsValidShaderFormatId(bundle.formatId) || bundle.materialSource.IsEmpty()) {
            Log::Error(LogShaderLibraryCook,
                       "The material shader bundle source passed a bundle with an invalid format "
                       "id or empty generated source (material {Asset})",
                       bundle.asset);
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!out_.TryEmplaceBack(allocator_)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        OwnedBundle& copy = out_[out_.Size() - 1];
        copy.shaderSetId = bundle.shaderSetId;
        copy.asset = bundle.asset;
        if (!copy.formatId.TryAssign(bundle.formatId) ||
            !copy.source.TryAssign(bundle.materialSource) ||
            !copy.traits.TryAppend(bundle.traits) ||
            !copy.inputs.TryAppend(bundle.vertexInputTypes)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        return Result<void>::Success();
    }

    Result<void> Report(const CookDiagnostic& diagnostic) noexcept override {
        Result<void> reported = sink_.Report(diagnostic);
        if (!reported) {
            forwardError_ = reported.Error();
        }

        return reported;
    }

    // 쿠커 sink가 돌려준 오류(없으면 None)다 — 단계는 그대로 Run 결과로 돌려준다.
    ErrorCode ForwardError() const noexcept {
        return forwardError_;
    }

private:
    Allocator& allocator_;
    Array<OwnedBundle>& out_;
    CookArtifactSink& sink_;
    ErrorCode forwardError_ = ErrorCode::None;
};

// 형식 목록 한 줄이다(프로필 backend 표를 이은 순서 — 진단 setting은 이 순번).
struct FormatSlot {
    StringView backend;
    StringView formatId;
    // 이 호스트가 생산 호스트 규칙에 없어 컴파일하지 않는 형식이다(UnsupportedCapability).
    bool skipped = false;
};

// 이 프로세스의 셰이더 생산 호스트다(HostConfig 플랫폼 이름 — macOS 개발 호스트는 arm64).
ShaderHost CurrentShaderHost() noexcept {
    return HostPlatformName() == StringView("Mac") ? ShaderHost::MacosArm64
                                                   : ShaderHost::WindowsX64;
}

Result<void> ReportOne(CookArtifactSink& sink, AssetDiagnosticCode code, ErrorCode error,
                       uint32 setting, AssetId asset = {}, AssetId related = {}) noexcept {
    CookDiagnostic diagnostic;
    diagnostic.code = code;
    diagnostic.error = error;
    diagnostic.setting = setting;
    diagnostic.asset = asset;
    diagnostic.related = related;
    return sink.Report(diagnostic);
}

// view(NUL 없음)를 out에 NUL 종료로 옮긴다. 넘치면 false다.
bool CopyPath(StringView view, char (&out)[StagePathBytes]) noexcept {
    if (view.ByteCount() + 1 > sizeof(out)) {
        return false;
    }

    memcpy(out, view.Data(), view.ByteCount());
    out[view.ByteCount()] = '\0';
    return true;
}

// 형식 정의 대조(SSD10 알림 (a) — 위반 하나를 이유 문구로)다. 통과하면 null이다.
const char* FormatViolation(const ShaderFormatStatus& loaded, const ShaderFormatSnapshot& snapshot,
                            StringView backend, StringView formatId) noexcept {
    if (!loaded) {
        return "the format definition could not be read";
    }

    if (snapshot.definition.state == ShaderFormatState::Placeholder) {
        return "the format is a placeholder and cannot be cooked";
    }

    if (!(StringView(ShaderBackendName(snapshot.definition.backend)) == backend)) {
        return "the format definition backend differs from the profile backend";
    }

    if (formatId == StringView(PreviewFormatId)) {
        return "GLES31_PC is a development preview format and is never cooked";
    }

    return nullptr;
}

// 형식 줄 값의 합(보고서 절)이다.
struct CookTotals {
    uint64 jobs = 0;
    uint64 entries = 0;
    uint64 codes = 0;
    uint64 hits = 0;
    uint64 reused = 0;
    uint64 bundles = 0;
    uint64 projectEntries = 0;
};

// Project 라이브러리를 만들 때만 적는 보고서 값 수(materialBundles·projectEntries)다.
constexpr usize ProjectStatCount = 2;

// 쿠킹 한 번의 상태다(Run 하나 동안).
class StageRun {
public:
    StageRun(const ShaderLibraryCookStageConfig& config, const CookArtifactRequest& request,
             CookArtifactSink& sink) noexcept
        : config_(config), request_(request), sink_(sink), allocator_(*config.allocator),
          backends_(allocator_), slots_(allocator_), snapshots_(allocator_), tree_(allocator_),
          owned_(allocator_), bundles_(allocator_), nuggetRoot_(allocator_) {
    }

    ~StageRun() noexcept {
        if (bundleTable_ != nullptr) {
            bundleTable_->EndUse();
        }

        if (ownCache_) {
            ownCache_->Shutdown(0);
        }

        if (registry_ != nullptr) {
            config_.registry->Release();
        }
    }

    StageRun(const StageRun&) = delete;
    StageRun& operator=(const StageRun&) = delete;

    Result<void> Run() noexcept {
        if (auto listed = ListFormats(); !listed || slots_.IsEmpty()) {
            return listed;
        }

        bool proceed = false;
        if (auto located = LocateEngine(proceed); !located || !proceed) {
            return located;
        }

        if (auto checked = LoadFormats(proceed); !checked || !proceed) {
            return checked;
        }

        if (auto acquired = AcquireRegistry(proceed); !acquired || !proceed) {
            return acquired;
        }

        if (auto prepared = PrepareCompile(proceed); !prepared || !proceed) {
            return prepared;
        }

        if (auto collected = CollectBundles(); !collected) {
            return collected;
        }

        return CookFormats();
    }

private:
    // 1. 형식 목록(graphicsBackends 순서, backend마다 shaderFormats 순서)이다.
    Result<void> ListFormats() noexcept {
        if (auto made = MakeCookedManifestBackends(allocator_, *request_.profile, backends_);
            !made) {
            return made;
        }

        for (const CookedManifestBackend& backend : backends_) {
            for (const String& format : backend.formats) {
                if (!slots_.TryPushBack(FormatSlot{backend.name.View(), format.View()})) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }

        return Result<void>::Success();
    }

    Result<void> LocateEngine(bool& proceed) noexcept {
        proceed = false;
        if (!config_.engineDirectory.IsEmpty()) {
            if (!CopyPath(config_.engineDirectory, engine_)) {
                return ReportEnvironment(ErrorCode::CapacityExceeded,
                                         "the engine path is too long");
            }
        } else if (auto found = FindEngineDirectory(Span<char>(engine_, sizeof(engine_))); !found) {
            return ReportEnvironment(found.Error(), "the engine folder was not found");
        }

        snprintf(formats_, sizeof(formats_), "%s/Config/ShaderFormats", engine_);
        proceed = true;
        return Result<void>::Success();
    }

    // 2. 형식 정의를 읽고 대조한다(위반은 InvalidSetting — 하나라도 있으면 컴파일하지 않는다).
    Result<void> LoadFormats(bool& proceed) noexcept {
        proceed = false;
        bool violated = false;
        for (usize index = 0; index < slots_.Size(); ++index) {
            const FormatSlot& slot = slots_[index];
            auto created = UniquePtr<ShaderFormatSnapshot>::TryMake(allocator_, allocator_);
            if (!created) {
                return Result<void>::Failure(created.Error());
            }

            const ShaderFormatStatus loaded =
                LoadShaderFormatSnapshot(formats_, slot.formatId, *created.Value());
            if (const char* reason =
                    FormatViolation(loaded, *created.Value(), slot.backend, slot.formatId)) {
                Log::Error(LogShaderLibraryCook,
                           "Shader format {Format} of backend {Backend} in profile {Profile}: "
                           "{Reason} ({Code} {Path})",
                           slot.formatId, slot.backend, request_.profile->id.View(),
                           StringView(reason), StringView(ShaderFormatErrorName(loaded.code)),
                           StringView(loaded.path));
                if (auto reported = ReportOne(sink_, AssetDiagnosticCode::InvalidSetting,
                                              ErrorCode::None, static_cast<uint32>(index));
                    !reported) {
                    return reported;
                }

                violated = true;
            }

            if (!violated && !created.Value()->definition.CanProduceOn(CurrentShaderHost())) {
                Log::Error(LogShaderLibraryCook,
                           "This host ({Host}) is not a producer of the shader format {Format}",
                           HostPlatformName(), slot.formatId);
                if (auto reported =
                        ReportOne(sink_, AssetDiagnosticCode::None,
                                  ErrorCode::UnsupportedCapability, static_cast<uint32>(index));
                    !reported) {
                    return reported;
                }

                slots_[index].skipped = true;
            }

            if (!snapshots_.TryPushBack(Move(created.Value()))) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }

        proceed = !violated;
        return Result<void>::Success();
    }

    // 3. Freeze된 등록부다. 프로젝트 소속 타입은 ML5b 전 쿠킹 오류다.
    Result<void> AcquireRegistry(bool& proceed) noexcept {
        proceed = false;
        auto acquired = config_.registry->Acquire();
        if (!acquired) {
            if (acquired.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(acquired.Error());
            }

            Log::Error(LogShaderLibraryCook,
                       "The shader types to cook could not be prepared ({Error}): {Problem}",
                       ErrorCodeName(acquired.Error()), config_.registry->Problem());
            return ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::DependencyFailed, 0);
        }

        registry_ = acquired.Value();
        bool project = false;
        for (const ShaderTypeDesc* type : registry_->Types()) {
            if (type->owner == ShaderTypeOwner::Project ||
                type->owner == ShaderTypeOwner::ProjectPlugin) {
                Log::Error(LogShaderLibraryCook,
                           "Project shader type {Type} cannot be cooked before project modules "
                           "load (ModuleLoader ML5b)",
                           type->name);
                project = true;
            }
        }

        for (const ShaderVertexInputType& input : registry_->VertexInputTypes()) {
            if (input.owner == ShaderTypeOwner::Project ||
                input.owner == ShaderTypeOwner::ProjectPlugin) {
                Log::Error(LogShaderLibraryCook,
                           "Project vertex input type {Type} cannot be cooked before project "
                           "modules load (ModuleLoader ML5b)",
                           input.name);
                project = true;
            }
        }

        if (project) {
            return ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::Unsupported, 0);
        }

        proceed = true;
        return Result<void>::Success();
    }

    // 4. 원문·워커·너겟·관리자 준비다.
    Result<void> PrepareCompile(bool& proceed) noexcept {
        proceed = false;
        char shaders[StagePathBytes];
        snprintf(shaders, sizeof(shaders), "%s/Shaders", engine_);
        if (const ShaderSourceStatus mounted =
                tree_.Mount(StringView(EngineShaderVirtualRoot), StringView(shaders));
            !mounted) {
            return ReportEnvironment(ErrorCode::NotFound, mounted.message);
        }

        for (const ShaderCookSourceDirectory& extra : config_.sourceDirectories) {
            if (const ShaderSourceStatus mounted = tree_.Mount(extra.virtualRoot, extra.directory);
                !mounted) {
                return ReportEnvironment(ErrorCode::InvalidArgument, mounted.message);
            }
        }

        if (!config_.workerExecutable.IsEmpty()) {
            if (!CopyPath(config_.workerExecutable, worker_)) {
                return ReportEnvironment(ErrorCode::CapacityExceeded,
                                         "the worker path is too long");
            }
        } else if (!compile_support::FindWorkerNextToExecutable(worker_)) {
            return ReportEnvironment(ErrorCode::NotFound,
                                     "ShaderCompileWorker is not next to this executable");
        }

        const int work = snprintf(work_, sizeof(work_), "%.*s/%s",
                                  static_cast<int>(request_.stagingDirectory.ByteCount()),
                                  request_.stagingDirectory.Data(), WorkerDirectoryName);
        if (work <= 0 || static_cast<usize>(work) >= sizeof(work_)) {
            return ReportEnvironment(ErrorCode::CapacityExceeded,
                                     "the worker work directory path is too long");
        }

        cache_ = request_.nuggets;
        if (cache_ == nullptr && request_.engineOnly) {
            // 엔진 전용 쿠킹: 엔진 너겟 root의 캐시를 이 Run 동안 연다(A4f4 3번).
            if (auto resolved = ResolveShaderNuggetRoot(StringView(), nuggetRoot_); !resolved) {
                return ReportEnvironment(resolved.Error(), "the engine nugget root was not found");
            }

            auto created =
                UniquePtr<NuggetCache>::TryMake(allocator_, allocator_, *request_.files, clock_);
            if (!created) {
                return Result<void>::Failure(created.Error());
            }

            NuggetSettingsFailure failure;
            if (auto opened = OpenShaderNuggetCache(*created.Value(), allocator_, *request_.files,
                                                    nuggetRoot_.View(), &failure);
                !opened) {
                return ReportEnvironment(opened.Error(), "the engine nugget cache could not open");
            }

            ownCache_ = Move(created.Value());
            cache_ = ownCache_.Get();
        }

        if (!reflection_.registered) {
            return ReportEnvironment(ErrorCode::InvalidArgument,
                                     "the shader compiler reflection types could not register");
        }

        proceed = true;
        return Result<void>::Success();
    }

    // 4b. 머티리얼 셰이더 묶음(16.5절 SSD87)이다. 엔진 전용이거나 원천이 없으면 Project
    // 라이브러리를 만들지 않는다. 원천 실패(쿠커 sink 오류·할당 실패 밖)는 진단 하나와 Log Error다.
    Result<void> CollectBundles() noexcept {
        project_ = false;
        if (request_.engineOnly) {
            return Result<void>::Success();
        }

        MaterialShaderBundleSource* source = config_.bundles;
        if (source == nullptr) {
            MaterialShaderBundleSourceTable& table = MaterialShaderBundleSources();
            source = table.Current();
            if (source != nullptr) {
                // 단계가 도는 동안 원천 등록·해제는 Busy다(소멸자가 EndUse).
                table.BeginUse();
                bundleTable_ = &table;
            }
        }

        if (source == nullptr) {
            Log::Info(LogShaderLibraryCook,
                      "No material shader bundle source is registered (MaterialCook is not "
                      "loaded); profile {Profile} gets no Project shader library",
                      request_.profile->id.View());
            return Result<void>::Success();
        }

        StageBundleSink bundleSink(allocator_, owned_, sink_);
        Result<void> collected = source->Collect(request_, bundleSink);
        if (!collected) {
            if (bundleSink.ForwardError() != ErrorCode::None ||
                collected.Error() == ErrorCode::OutOfMemory) {
                return collected;
            }

            Log::Error(LogShaderLibraryCook,
                       "The material shader bundle source failed ({Error}); the Project shader "
                       "library is not built",
                       ErrorCodeName(collected.Error()));
            return ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::DependencyFailed, 0);
        }

        // 사본을 다 모은 뒤에 view를 만든다(사본 배열이 자라면 문자열 위치가 바뀔 수 있다).
        if (!bundles_.TryReserve(owned_.Size())) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        for (const OwnedBundle& owned : owned_) {
            MaterialShaderBundle bundle;
            bundle.formatId = owned.formatId.View();
            bundle.shaderSetId = owned.shaderSetId;
            bundle.materialSource = owned.source.View();
            bundle.traits = owned.traits.View();
            bundle.vertexInputTypes = owned.inputs.View();
            bundle.asset = owned.asset;
            (void)bundles_.TryPushBack(bundle);
        }

        project_ = true;
        return Result<void>::Success();
    }

    ShaderCompileManagerConfig ManagerConfig(NuggetCache* cache) noexcept {
        ShaderCompileManagerConfig config;
        config.allocator = &allocator_;
        config.tree = &tree_;
        config.reflection = reflection_.Reflection();
        config.cache = cache;
        config.workerExecutable = StringView(worker_);
        config.workRoot = StringView(work_);
        config.workerCount = config_.workerCount;
        return config;
    }

    // 5–8. 형식마다 라이브러리를 만들고 재사용·게시한다.
    Result<void> CookFormats() noexcept {
        ShaderCompileManager manager(ManagerConfig(cache_));
        if (auto started = manager.Start(); !started) {
            manager.Shutdown();
            return ReportEnvironment(started.Error(), "the shader workers could not start");
        }

        Result<void> result = Result<void>::Success();
        CookTotals totals;
        for (usize index = 0; index < slots_.Size() && result; ++index) {
            if (slots_[index].skipped) {
                continue;
            }

            result = CookFormat(manager, index, ShaderLibraryKind::Engine, totals);
            if (result && project_) {
                result = CookFormat(manager, index, ShaderLibraryKind::Project, totals);
            }
        }

        manager.Shutdown();
        if (!result) {
            return result;
        }

        const struct {
            const char* name;
            uint64 value;
        } stats[] = {
            {"formats", slots_.Size()},          {"jobs", totals.jobs},
            {"entries", totals.entries},         {"codes", totals.codes},
            {"cacheHits", totals.hits},          {"reused", totals.reused},
            {"materialBundles", totals.bundles}, {"projectEntries", totals.projectEntries}};
        // Project 라이브러리 값은 그것을 만들 때만 적는다(엔진 전용·원천 없음 보고서는 그대로다).
        const usize statCount = project_ ? sizeof(stats) / sizeof(stats[0])
                                         : sizeof(stats) / sizeof(stats[0]) - ProjectStatCount;
        for (usize index = 0; index < statCount; ++index) {
            if (auto reported = sink_.ReportStat(StringView(stats[index].name), stats[index].value);
                !reported) {
                return reported;
            }
        }

        return Result<void>::Success();
    }

    ShaderLibraryCookDesc Desc(const ShaderFormatSnapshot* const* format,
                               ShaderCompileManager& manager, ShaderLibraryKind library) noexcept {
        ShaderLibraryCookDesc desc;
        desc.allocator = &allocator_;
        desc.registry = registry_;
        desc.formats = Span<const ShaderFormatSnapshot* const>(format, 1);
        desc.library = library;
        if (library == ShaderLibraryKind::Project) {
            desc.materialBundles = bundles_.View();
        }

        desc.includeDevelopment = request_.settings->cookDevelopmentShaders;
        desc.manager = &manager;
        desc.files = request_.files;
        return desc;
    }

    Result<void> CookFormat(ShaderCompileManager& manager, usize index, ShaderLibraryKind library,
                            CookTotals& totals) noexcept {
        const ShaderFormatSnapshot* const format = snapshots_[index].Get();
        const StringView formatId = slots_[index].formatId;
        const StringView name = library == ShaderLibraryKind::Engine ? CookedArtifactEngineName
                                                                     : CookedArtifactProjectName;
        const ShaderLibraryCookDesc desc = Desc(&format, manager, library);
        ShaderLibraryCookReport report(allocator_);
        Array<uint8> bytes(allocator_);
        const Result<void> built = CompileShaderLibrary(desc, report, bytes);
        if (report.formats.Size() == 1) {
            const ShaderLibraryCookFormat& line = report.formats[0];
            totals.jobs += line.jobs;
            totals.entries += line.entries;
            totals.codes += line.codes;
            totals.hits += line.cacheHits;
            totals.bundles += line.materialBundles;
            totals.projectEntries += library == ShaderLibraryKind::Project ? line.entries : 0;
        }

        if (!built) {
            if (built.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(built.Error());
            }

            if (report.formats.Size() != 1) {
                // 열거·컴파일 자체의 오류(관리자 계약 — 등록부·입력 결함)다.
                return Result<void>::Failure(built.Error());
            }

            return ReportFailure(index, report);
        }

        const ShaderLibraryCookFormat& line = report.formats[0];
        if (request_.verifyNuggets && cache_ != nullptr) {
            if (auto verified = Verify(index, desc, bytes); !verified) {
                return verified;
            }
        }

        for (const CookedManifestArtifact& candidate : request_.reusable) {
            if (candidate.kind == CookArtifactKind::ShaderLibrary &&
                candidate.name.View() == name && candidate.formatId.View() == formatId &&
                candidate.cookKey == line.cookKey) {
                ++totals.reused;
                return sink_.Reuse(candidate);
            }
        }

        StringBuilder<StageInlineBytes> path(allocator_);
        if (auto made = sink_.NewStagingPath(path); !made) {
            return made;
        }

        if (auto written = WriteStaging(path.View(), bytes.View()); !written) {
            return written;
        }

        CookArtifactOutput output;
        output.name = name;
        output.formatId = formatId;
        output.cookKey = line.cookKey;
        output.stagingFile = path.View();
        return sink_.Produce(output);
    }

    // 묶음 문제마다 진단 하나를 낸다(16.5절 SSD88 — 문구는 CompileShaderLibrary가 Log로 남겼다).
    Result<void> ReportBundleProblems(usize index, const ShaderLibraryCookReport& report) noexcept {
        for (const ShaderLibraryBundleProblem& problem : report.bundleProblems) {
            const bool collision = problem.kind == ShaderLibraryBundleProblemKind::Collision;
            if (auto reported =
                    ReportOne(sink_, AssetDiagnosticCode::None,
                              collision ? ErrorCode::AlreadyExists : ErrorCode::InvalidArgument,
                              static_cast<uint32>(index), problem.asset, problem.otherAsset);
                !reported) {
                return reported;
            }
        }

        return Result<void>::Success();
    }

    // job 실패를 Log에 남기고 형식마다 진단 하나를 낸다(6번). 머티리얼 job 실패는 머티리얼마다
    // 진단 하나(asset)다(16.5절 SSD88).
    Result<void> ReportFailure(usize index, const ShaderLibraryCookReport& report) noexcept {
        const ShaderLibraryCookFormat& line = report.formats[0];
        if (line.outcome == ShaderLibraryCookOutcome::BundleFailed) {
            return ReportBundleProblems(index, report);
        }

        if (line.outcome != ShaderLibraryCookOutcome::JobsFailed) {
            Log::Error(LogShaderLibraryCook,
                       "The {Format} shader library could not be built ({Outcome}): {Reason}",
                       StringView(line.formatId),
                       StringView(ShaderLibraryCookOutcomeName(line.outcome)),
                       StringView(line.reason));
            return ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::InvalidArgument,
                             static_cast<uint32>(index));
        }

        bool unsupportedOnly = true;
        bool engineFailures = false;
        for (const ShaderLibraryCookJobFailure& failure : report.failures) {
            unsupportedOnly =
                unsupportedOnly && failure.status == ShaderCompileStatus::UnsupportedCapability;
            engineFailures = engineFailures || failure.asset.IsNull();
            Log::Error(LogShaderLibraryCook,
                       "Shader job {Format} {Type} (vertex input {VertexInput}, permutation "
                       "{Permutation}, bundle {Bundle}, material {Asset}) failed: {Status}\n"
                       "{Diagnostics}",
                       StringView(failure.formatId), StringView(failure.typeName),
                       failure.vertexInputType, failure.permutationId, failure.bundleId,
                       failure.asset, StringView(ShaderCompileStatusName(failure.status)),
                       failure.diagnostics.View());
        }

        // 머티리얼 job 실패는 머티리얼마다 진단 하나다(실패 줄 순서 — 결정적 job 순서).
        for (usize failureIndex = 0; failureIndex < report.failures.Size(); ++failureIndex) {
            const ShaderLibraryCookJobFailure& failure = report.failures[failureIndex];
            bool seen = failure.asset.IsNull();
            for (usize earlier = 0; earlier < failureIndex && !seen; ++earlier) {
                seen = report.failures[earlier].asset == failure.asset;
            }

            if (seen) {
                continue;
            }

            if (auto reported =
                    ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::DependencyFailed,
                              static_cast<uint32>(index), failure.asset);
                !reported) {
                return reported;
            }
        }

        if (!engineFailures) {
            return Result<void>::Success();
        }

        if (unsupportedOnly) {
            Log::Error(LogShaderLibraryCook,
                       "This host cannot produce the shader format {Format} (a tool of the "
                       "format is not available here)",
                       StringView(line.formatId));
        }

        return ReportOne(sink_, AssetDiagnosticCode::None,
                         unsupportedOnly ? ErrorCode::UnsupportedCapability
                                         : ErrorCode::DependencyFailed,
                         static_cast<uint32>(index));
    }

    // 7. 너겟 없이 다시 만든 byte와 대조한다.
    Result<void> Verify(usize index, const ShaderLibraryCookDesc& cached,
                        const Array<uint8>& bytes) noexcept {
        ShaderCompileManager fresh(ManagerConfig(nullptr));
        if (auto started = fresh.Start(); !started) {
            fresh.Shutdown();
            return ReportEnvironment(started.Error(), "the verify shader workers could not start");
        }

        ShaderLibraryCookDesc desc = cached;
        desc.manager = &fresh;
        ShaderLibraryCookReport report(allocator_);
        Array<uint8> again(allocator_);
        const Result<void> built = CompileShaderLibrary(desc, report, again);
        fresh.Shutdown();
        if (!built) {
            if (built.Error() == ErrorCode::OutOfMemory) {
                return Result<void>::Failure(built.Error());
            }

            return ReportFailure(index, report);
        }

        if (again.Size() == bytes.Size() && memcmp(again.Data(), bytes.Data(), bytes.Size()) == 0) {
            return Result<void>::Success();
        }

        Log::Error(LogShaderLibraryCook,
                   "The {Format} shader library built from asset nuggets differs from a fresh "
                   "compile (nondeterministic shader output or a stale nugget)",
                   slots_[index].formatId);
        return ReportOne(sink_, AssetDiagnosticCode::NuggetMismatch, ErrorCode::InvalidArgument,
                         static_cast<uint32>(index));
    }

    Result<void> WriteStaging(StringView path, Span<const uint8> bytes) noexcept {
        platform::File file;
        platform::FileStatus status = file.Open(*request_.files, path.Data(), path.ByteCount(),
                                                platform::FileOpenMode::CreateAlways);
        if (status) {
            status = file.WriteAll(bytes.Data(), bytes.Size()).status;
        }

        const platform::FileStatus closed = file.IsOpen() ? file.Close() : platform::FileStatus{};
        if (!status || !closed) {
            return Result<void>::Failure(FileErrorCode(!status ? status : closed));
        }

        return Result<void>::Success();
    }

    // 환경 실패(엔진 폴더·워커·너겟·원문)는 Log와 진단 하나로 남기고 성공으로 끝낸다.
    Result<void> ReportEnvironment(ErrorCode error, const char* reason) noexcept {
        if (error == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(error);
        }

        Log::Error(LogShaderLibraryCook, "Shader libraries cannot be cooked ({Error}): {Reason}",
                   ErrorCodeName(error), StringView(reason));
        return ReportOne(sink_, AssetDiagnosticCode::None, ErrorCode::DependencyFailed, 0);
    }

    const ShaderLibraryCookStageConfig& config_;
    const CookArtifactRequest& request_;
    CookArtifactSink& sink_;
    Allocator& allocator_;
    Array<CookedManifestBackend> backends_;
    Array<FormatSlot> slots_;
    Array<UniquePtr<ShaderFormatSnapshot>> snapshots_;
    ShaderSourceTree tree_;
    compile_support::KeyReflection reflection_{allocator_, CookStageContextId};
    const ShaderTypeRegistry* registry_ = nullptr;
    // 머티리얼 셰이더 묶음 사본과 그 view(16.5절 SSD87)다. project_가 참이면 Project 라이브러리를
    // 만든다.
    Array<OwnedBundle> owned_;
    Array<MaterialShaderBundle> bundles_;
    MaterialShaderBundleSourceTable* bundleTable_ = nullptr;
    bool project_ = false;
    NuggetSystemClock clock_;
    String nuggetRoot_;
    UniquePtr<NuggetCache> ownCache_;
    NuggetCache* cache_ = nullptr;
    char engine_[StagePathBytes] = {};
    char formats_[StagePathBytes] = {};
    char worker_[StagePathBytes] = {};
    char work_[StagePathBytes] = {};
};

} // namespace cook_stage_detail

ShaderLibraryCookStage::ShaderLibraryCookStage(const ShaderLibraryCookStageConfig& config) noexcept
    : config_(config) {
}

Result<void> ShaderLibraryCookStage::Run(const CookArtifactRequest& request,
                                         CookArtifactSink& sink) noexcept {
    if (config_.allocator == nullptr || config_.registry == nullptr || request.profile == nullptr ||
        request.settings == nullptr || request.files == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    cook_stage_detail::StageRun run(config_, request, sink);
    return run.Run();
}

} // namespace rocks::shader
