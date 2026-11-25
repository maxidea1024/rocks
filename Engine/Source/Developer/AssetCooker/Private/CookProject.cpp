#include <AssetCooker/CookProject.generated.h>

#include <AssetCooker/CookProject.h>
#include <AssetDatabase/AtomicFileWrite.h>
#include <Core/Algorithms.h>
#include <Core/Build/BuildInfo.h>
#include <Core/FileRead.h>
#include <Core/Text/Json.h>
#include <ReflectionText/TextCodecOptions.h>
#include <ReflectionText/TextJson.h>

namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextCodecOptions;
using reflection_text::TextUse;

// 경로·보고서 조립 버퍼의 인라인 크기다(대개 allocator로 자란다).
constexpr usize CookProjectInlineBytes = 64;

Result<void> AppendDiagnostics(Array<CookDiagnostic>& out,
                               Span<const CookDiagnostic> from) noexcept {
    return out.TryAppend(from);
}

// AssetId 표준 문자열(null이면 빈 값)이다.
Result<void> AssignId(String& out, AssetId id) noexcept {
    if (id.IsNull()) {
        out.Clear();
        return Result<void>::Success();
    }

    auto text = TryFormat(out.GetAllocator(), "{}", id);
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    out = Move(text.Value());
    return Result<void>::Success();
}

// artifact 종류 이름이다(보고서 kind).
StringView ArtifactKindName(CookArtifactKind kind) noexcept {
    return kind == CookArtifactKind::ShaderLibrary ? StringView("ShaderLibrary") : StringView("");
}

// 보고서 artifact·단계 값 줄을 채운다.
Result<void> FillArtifactReport(Allocator& allocator, const CookProjectResult& result,
                                CookReport& report) noexcept {
    if (auto reserved = report.artifacts.TryReserve(result.artifacts.Size()); !reserved) {
        return reserved;
    }

    for (const CookArtifactRow& row : result.artifacts) {
        CookReportArtifact line(allocator);
        line.bytes = row.bytes;
        line.reused = row.reused;
        char hex[Sha256Digest::HexBytes + 1];
        row.sha256.ToHex(hex);
        Result<void> filled = line.kind.TryAssign(ArtifactKindName(row.kind));
        if (filled) {
            filled = line.name.TryAssign(row.name.View());
        }

        if (filled) {
            filled = line.format.TryAssign(row.formatId.View());
        }

        if (filled) {
            filled = line.sha256.TryAssign(StringView(hex));
        }

        if (!filled) {
            return filled;
        }

        (void)report.artifacts.TryPushBack(Move(line));
    }

    if (auto reserved = report.stageStats.TryReserve(result.stats.Size()); !reserved) {
        return reserved;
    }

    for (const CookArtifactStatRow& row : result.stats) {
        CookReportStat line(allocator);
        line.value = row.value;
        auto stage = TryFormat(allocator, "{}", row.stage);
        Result<void> filled =
            stage ? Result<void>::Success() : Result<void>::Failure(stage.Error());
        if (filled) {
            line.stage = Move(stage.Value());
        }

        if (filled) {
            filled = line.name.TryAssign(row.name.View());
        }

        if (!filled) {
            return filled;
        }

        (void)report.stageStats.TryPushBack(Move(line));
    }

    return Result<void>::Success();
}

// 셰이더 형식이 프로필 backend 표(graphicsBackends의 shaderFormats)에 있는지 본다.
bool ProfileHasShaderFormat(const TargetProfile& profile, StringView id) noexcept {
    for (const String& backend : profile.graphicsBackends) {
        const ShaderFormatList* list = profile.shaderFormats.Find(backend);
        if (list == nullptr) {
            continue;
        }

        for (const String& format : list->formats) {
            if (format.View() == id) {
                return true;
            }
        }
    }

    return false;
}

Result<void> CopyArtifactTo(const CookedManifestArtifact& source,
                            Array<CookedManifestArtifact>& out, Allocator& allocator) noexcept {
    CookedManifestArtifact copy(allocator);
    copy.kind = source.kind;
    copy.bytes = source.bytes;
    copy.sha256 = source.sha256;
    copy.cookKey = source.cookKey;
    if (auto assigned = copy.name.TryAssign(source.name.View()); !assigned) {
        return assigned;
    }

    if (auto assigned = copy.formatId.TryAssign(source.formatId.View()); !assigned) {
        return assigned;
    }

    return out.TryPushBack(Move(copy));
}

// staging 파일 이름의 앞뒤다(`<staging>/artifact-<번호>.tmp` — 패키지 임시 이름
// `<A>-<h>.rpkg.tmp`와 겹치지 않는다).
constexpr StringView ArtifactStagingPrefix = "artifact-";
constexpr StringView ArtifactStagingSuffix = ".tmp";

// 단계의 출력 sink다(10.3.1절 A4f3 2·3번): 단계 계약을 확인하고 게시기에 넘기며, 보고서 줄을
// 모은다. 처음 실패한 호출을 result.stageFailure에 적는다.
class ArtifactStageSink final : public CookArtifactSink {
public:
    ArtifactStageSink(Allocator& allocator, CookPublisher& publisher,
                      const CookArtifactRequest& request, const CookArtifactStage& stage,
                      CookProjectResult& result) noexcept
        : allocator_(allocator), publisher_(publisher), request_(request), kind_(stage.Kind()),
          stage_(stage.Identity().processor), result_(result), issued_(allocator),
          used_(allocator) {
    }

    Result<void> NewStagingPath(StringBuilderBase& out) noexcept override {
        StringBuilder<CookProjectInlineBytes> path(allocator_);
        Result<void> made =
            path.TryAppendFormat("{}/{}{}{}", request_.stagingDirectory, ArtifactStagingPrefix,
                                 issued_.Size(), ArtifactStagingSuffix);
        String copy(allocator_);
        if (made) {
            made = copy.TryAssign(path.View());
        }

        if (made) {
            made = issued_.TryReserve(issued_.Size() + 1);
        }

        if (made) {
            made = used_.TryReserve(used_.Size() + 1);
        }

        if (made) {
            made = out.TryAppend(path.View());
        }

        if (!made) {
            return Fail(CookArtifactCall::NewStagingPath, StringView(), made.Error());
        }

        // 예약했다.
        (void)issued_.TryPushBack(Move(copy));
        (void)used_.TryPushBack(0);
        return Result<void>::Success();
    }

    Result<void> Produce(const CookArtifactOutput& output) noexcept override {
        const usize slot = FindIssued(output.stagingFile);
        const bool valid = IsArtifactName(output.name) && IsValidShaderFormatId(output.formatId) &&
                           ProfileHasShaderFormat(*request_.profile, output.formatId) &&
                           (!request_.engineOnly || output.name == CookedArtifactEngineName) &&
                           !Seen(output.name, output.formatId) && slot != algo::NotFound &&
                           used_[slot] == 0;
        if (!valid) {
            return Fail(CookArtifactCall::Produce, output.formatId, ErrorCode::InvalidArgument);
        }

        used_[slot] = 1;
        const CookedManifestArtifact* placed = nullptr;
        if (auto put = publisher_.PutArtifact(kind_, output.name, output.formatId, output.cookKey,
                                              output.stagingFile, &placed);
            !put) {
            return Fail(CookArtifactCall::Produce, output.formatId, put.Error());
        }

        return AddRow(CookArtifactCall::Produce, *placed, false);
    }

    Result<void> Reuse(const CookedManifestArtifact& artifact) noexcept override {
        bool candidate = false;
        for (const CookedManifestArtifact& offered : request_.reusable) {
            if (offered.kind == artifact.kind && offered.name.View() == artifact.name.View() &&
                offered.formatId.View() == artifact.formatId.View() &&
                offered.bytes == artifact.bytes && offered.sha256 == artifact.sha256 &&
                offered.cookKey == artifact.cookKey) {
                candidate = true;
                break;
            }
        }

        const StringView format = artifact.formatId.View();
        if (!candidate || Seen(artifact.name.View(), format)) {
            return Fail(CookArtifactCall::Reuse, format, ErrorCode::InvalidArgument);
        }

        if (auto reused = publisher_.ReuseArtifact(artifact); !reused) {
            return Fail(CookArtifactCall::Reuse, format, reused.Error());
        }

        return AddRow(CookArtifactCall::Reuse, artifact, true);
    }

    Result<void> Report(const CookDiagnostic& diagnostic) noexcept override {
        if (auto pushed = result_.diagnostics.TryPushBack(diagnostic); !pushed) {
            return Fail(CookArtifactCall::Report, StringView(), pushed.Error());
        }

        return Result<void>::Success();
    }

    Result<void> ReportStat(StringView name, uint64 value) noexcept override {
        CookArtifactStatRow row(allocator_);
        row.stage = stage_;
        row.value = value;
        Result<void> added = row.name.TryAssign(name);
        if (added) {
            added = result_.stats.TryPushBack(Move(row));
        }

        if (!added) {
            return Fail(CookArtifactCall::ReportStat, StringView(), added.Error());
        }

        return Result<void>::Success();
    }

    // 처음 실패를 적고 그 오류를 돌려준다(Run 자체의 오류도 여기로 적는다).
    Result<void> Fail(CookArtifactCall call, StringView format, ErrorCode error) noexcept {
        CookArtifactFailure& failure = result_.stageFailure;
        if (failure.call == CookArtifactCall::None) {
            failure.call = call;
            failure.stage = stage_;
            failure.error = error;
            // 형식 id 복사 실패(할당)는 빈 칸으로 둔다 — 원인 오류는 그대로 돌려준다.
            (void)failure.formatId.TryAssign(format);
        }

        return Result<void>::Failure(error);
    }

private:
    static bool IsArtifactName(StringView name) noexcept {
        return name == CookedArtifactEngineName || name == CookedArtifactProjectName;
    }

    usize FindIssued(StringView path) const noexcept {
        for (usize index = 0; index < issued_.Size(); ++index) {
            if (issued_[index].View() == path) {
                return index;
            }
        }

        return algo::NotFound;
    }

    // 이번 실행에 같은 (이름, 형식)이 이미 있는지 본다.
    bool Seen(StringView name, StringView format) const noexcept {
        for (const CookArtifactRow& row : result_.artifacts) {
            if (row.kind == kind_ && row.name.View() == name && row.formatId.View() == format) {
                return true;
            }
        }

        return false;
    }

    Result<void> AddRow(CookArtifactCall call, const CookedManifestArtifact& artifact,
                        bool reused) noexcept {
        CookArtifactRow row(allocator_);
        row.kind = artifact.kind;
        row.bytes = artifact.bytes;
        row.sha256 = artifact.sha256;
        row.reused = reused;
        Result<void> added = row.name.TryAssign(artifact.name.View());
        if (added) {
            added = row.formatId.TryAssign(artifact.formatId.View());
        }

        if (added) {
            added = result_.artifacts.TryPushBack(Move(row));
        }

        if (!added) {
            return Fail(call, artifact.formatId.View(), added.Error());
        }

        return Result<void>::Success();
    }

    Allocator& allocator_;
    CookPublisher& publisher_;
    const CookArtifactRequest& request_;
    CookArtifactKind kind_;
    Guid stage_;
    CookProjectResult& result_;
    // NewStagingPath가 준 경로와 Produce가 쓴 표지(0·1)다.
    Array<String> issued_;
    Array<uint8> used_;
};

// 비에셋 산출물 단계를 돈다(10.3.1절 A4f3 3번 — CookPackages 뒤, Finish 전). 셰이더 없는 프로필은
// 단계를 부르지 않고, 단계가 없으면 오류 진단(code None, UnsupportedCapability)을 남긴다.
// 단계 입력이다(프로젝트 쿠킹과 엔진 전용 쿠킹이 같은 코드를 쓴다).
struct ArtifactStageInputs {
    platform::FileSystem* files = nullptr;
    const TargetProfile* profile = nullptr;
    const CookSettings* settings = nullptr;
    Sha256Digest profileSettings{};
    uint32 cookerVersion = CookerVersion;
    NuggetCache* nuggets = nullptr;
    bool verifyNuggets = false;
    bool engineOnly = false;
    CookArtifactStageTable* stages = nullptr;
};

Result<void> RunArtifactStages(Allocator& allocator, const ArtifactStageInputs& request,
                               StringView root, const CookedManifest* previous,
                               CookPublisher& publisher, CookProjectResult& result) noexcept {
    const TargetProfile& profile = *request.profile;
    if (profile.graphicsBackends.IsEmpty()) {
        return Result<void>::Success();
    }

    CookArtifactStage* stage =
        request.stages != nullptr ? request.stages->Find(CookArtifactKind::ShaderLibrary) : nullptr;
    if (stage == nullptr) {
        return result.diagnostics.TryPushBack(CookDiagnostic{
            AssetDiagnosticCode::None, false, {}, {}, 0, ErrorCode::UnsupportedCapability});
    }

    // 재사용 후보(10.3절 6항): PlanCookReuse와 같은 머리 조건에서 게시 파일이 맞는 같은 종류 항목.
    Array<CookedManifestArtifact> reusable(allocator);
    if (previous != nullptr && !request.verifyNuggets &&
        previous->profileId.View() == profile.id.View() &&
        previous->profileSettings == request.profileSettings &&
        previous->cookerVersion == request.cookerVersion) {
        for (const CookedManifestArtifact& old : previous->artifacts) {
            if (old.kind != stage->Kind()) {
                continue;
            }

            auto same = publisher.ArtifactFileMatches(old);
            if (!same) {
                return Result<void>::Failure(same.Error());
            }

            if (!same.Value()) {
                continue;
            }

            if (auto copied = CopyArtifactTo(old, reusable, allocator); !copied) {
                return copied;
            }
        }
    }

    StringBuilder<CookProjectInlineBytes> staging(allocator);
    const StringView stagingPieces[] = {root, "/", CookStagingDirectoryName};
    if (auto built = staging.TryAppendJoin(stagingPieces, ""); !built) {
        return built;
    }

    CookArtifactRequest artifactRequest;
    artifactRequest.allocator = &allocator;
    artifactRequest.files = request.files;
    artifactRequest.profile = request.profile;
    artifactRequest.settings = request.settings;
    artifactRequest.cookedRoot = root;
    artifactRequest.stagingDirectory = staging.View();
    artifactRequest.nuggets = request.nuggets;
    artifactRequest.engineOnly = request.engineOnly;
    artifactRequest.verifyNuggets = request.verifyNuggets;
    artifactRequest.reusable = reusable.View();
    ArtifactStageSink sink(allocator, publisher, artifactRequest, *stage, result);
    Result<void> ran = Result<void>::Success();
    {
        // 단계가 도는 동안 표 등록·해제는 Busy다(10.3.1절 A4f3 2번).
        CookArtifactStageUse use(*request.stages);
        ran = stage->Run(artifactRequest, sink);
    }

    if (!ran) {
        // sink가 이미 적었으면 그 호출이 남는다(처음 실패).
        return sink.Fail(CookArtifactCall::Run, StringView(), ran.Error());
    }

    return Result<void>::Success();
}

// 보고서 입력이다(프로젝트 쿠킹은 에디터 패키지 집합의 Registry, 엔진 전용은 요청의 Registry).
struct CookReportInputs {
    platform::FileSystem* files = nullptr;
    TypeRegistry* types = nullptr;
    ObjectRegistry* registry = nullptr;
    ObjectAccessContext access{};
    const TargetProfile* profile = nullptr;
    EngineVersion cookedBy{};
};

CookReportInputs ReportInputsOf(const CookProjectRequest& request) noexcept {
    return CookReportInputs{
        request.files,   request.types,   &request.packages->Registry(), request.packages->Access(),
        request.profile, request.cookedBy};
}

// 보고서를 만들어 쿠킹 루트에 원자 교체로 쓴다.
Result<void> WriteReport(Allocator& allocator, const CookReportInputs& request,
                         const CookProjectResult& result, StringView root) noexcept {
    CookReport report(allocator);
    char buildId[Sha256Digest::HexBytes + 1];
    build::ToolBuildId().ToHex(buildId);
    char version[EngineVersion::TextBytes];
    request.cookedBy.ToChars(version);
    Result<void> step = report.toolBuildId.TryAssign(StringView(buildId));
    if (step) {
        step = report.engineVersion.TryAssign(StringView(version));
    }

    if (step) {
        step = report.profile.TryAssign(request.profile->id.View());
    }

    if (step) {
        step = report.diagnostics.TryReserve(result.diagnostics.Size());
    }

    if (!step) {
        return step;
    }

    report.partial = result.partial;
    report.published = result.published;
    report.targets = static_cast<uint32>(result.targets);
    report.cooked = static_cast<uint32>(result.cooked);
    report.reused = static_cast<uint32>(result.reused);
    report.bytesWritten = result.bytesWritten;
    for (const CookDiagnostic& diagnostic : result.diagnostics) {
        CookReportDiagnostic line(allocator);
        line.setting = diagnostic.setting;
        Result<void> filled = line.code.TryAssign(StringView(AssetDiagnosticName(diagnostic.code)));
        if (filled) {
            filled = line.level.TryAssign(diagnostic.warning ? "warning" : "error");
        }

        if (filled) {
            filled = AssignId(line.asset, diagnostic.asset);
        }

        if (filled) {
            filled = AssignId(line.related, diagnostic.related);
        }

        if (filled) {
            filled = line.error.TryAssign(StringView(ErrorCodeName(diagnostic.error)));
        }

        if (!filled) {
            return filled;
        }

        (void)report.diagnostics.TryPushBack(Move(line));
    }

    if (auto filled = FillArtifactReport(allocator, result, report); !filled) {
        return filled;
    }

    auto lease = request.types->AcquireValueType(GeneratedType<CookReport>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    const AssignmentTarget target = AssignmentTarget::Value(*request.registry, request.access,
                                                            lease.Value(), &report, allocator);
    StringBuilder<CookProjectInlineBytes> text(allocator);
    JsonWriter writer(text);
    TextCodecOptions options;
    options.use = TextUse::Report;
    // 내보내기가 성공하면 Finish는 끝 줄바꿈 append만 남는다(실패는 할당 실패뿐) — 한 갈래로 본다.
    Result<void> exported = reflection_text::ExportJson(target, writer, options);
    if (exported) {
        exported = writer.Finish();
    }

    if (!exported) {
        return exported;
    }

    const StringView json = text.View();
    auto written = WriteFileAtomically(
        *request.files, allocator, root, CookReportFileName,
        Span<const uint8>(reinterpret_cast<const uint8*>(json.Data()), json.ByteCount()));
    return written ? Result<void>::Success() : Result<void>::Failure(written.Error());
}

void ResetCookResult(CookProjectResult& result) noexcept {
    result.diagnostics.Clear();
    result.targets = 0;
    result.cooked = 0;
    result.reused = 0;
    result.bytesWritten = 0;
    result.partial = false;
    result.published = false;
    result.artifacts.Clear();
    result.stats.Clear();
    result.stageFailure.call = CookArtifactCall::None;
    result.stageFailure.stage = Guid{};
    result.stageFailure.formatId.Clear();
    result.stageFailure.error = ErrorCode::None;
}

// 기존 매니페스트를 읽는다(없거나 깨졌으면 hasExisting 거짓 — 깨진 것은 경고 진단).
Result<void> ReadPreviousManifest(Allocator& allocator, platform::FileSystem& files,
                                  StringView root, uint64 limitBytes, CookedManifest& existing,
                                  bool& hasExisting, CookProjectResult& result) noexcept {
    hasExisting = false;
    StringBuilder<CookProjectInlineBytes> manifestPath(allocator);
    const StringView manifestPieces[] = {root, "/", CookedManifestFileName};
    if (auto built = manifestPath.TryAppendJoin(manifestPieces, ""); !built) {
        return built;
    }

    Array<uint8> manifestBytes(allocator);
    bool existed = false;
    // 깨진 기존 매니페스트(상한 초과 포함)는 경고 뒤 없는 것으로 본다 — 사용자가 지우기 전까지
    // 쿠킹이 멈추지 않게 한다.
    AssetDiagnosticCode brokenCode = AssetDiagnosticCode::CorruptPackage;
    ErrorCode broken = ErrorCode::None;
    if (auto read = ReadWholeFile(files, manifestPath.View(), limitBytes, manifestBytes, existed);
        !read) {
        if (read.Error() != ErrorCode::CapacityExceeded) {
            return read;
        }

        broken = read.Error();
    } else if (existed) {
        AssetDiagnostic failure;
        auto parsed = ReadCookedManifest(allocator, manifestBytes.View(), &failure);
        if (parsed) {
            existing = Move(parsed.Value());
            hasExisting = true;
        } else if (parsed.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(parsed.Error());
        } else {
            brokenCode = failure.code;
            broken = parsed.Error();
        }
    }

    if (broken != ErrorCode::None) {
        return result.diagnostics.TryPushBack(CookDiagnostic{brokenCode, true, {}, {}, 0, broken});
    }

    return Result<void>::Success();
}

ArtifactStageInputs StageInputsOf(const CookProjectRequest& request) noexcept {
    ArtifactStageInputs inputs;
    inputs.files = request.files;
    inputs.profile = request.profile;
    inputs.settings = request.settings;
    inputs.profileSettings = request.profileSettings;
    inputs.cookerVersion = request.cookerVersion;
    inputs.nuggets = request.nuggets;
    inputs.verifyNuggets = request.verifyNuggets;
    inputs.stages = request.stages;
    return inputs;
}

} // namespace

Result<void> CookProject(Allocator& allocator, const CookProjectRequest& request,
                         CookProjectResult& result) noexcept {
    ResetCookResult(result);
    if (request.files == nullptr || request.layout == nullptr || request.lock == nullptr ||
        request.cookLock == nullptr || request.index == nullptr || request.packages == nullptr ||
        request.types == nullptr || request.processors == nullptr || request.profile == nullptr ||
        request.settings == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 두 잠금(에셋 DB → 쿠킹 루트)을 모두 쥐었는지 본다(10.3.1절 A4f4 2번).
    const StringView saved = request.layout->SavedDirectory();
    if (!request.lock->IsHeldFor(*request.layout) ||
        !request.cookLock->IsHeldFor(saved, request.profile->id.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<CookProjectInlineBytes> root(allocator);
    if (auto built = AppendCookedRoot(root, saved, request.profile->id.View()); !built) {
        return built;
    }

    CookedManifest existing(allocator);
    bool hasExisting = false;
    if (auto read = ReadPreviousManifest(allocator, *request.files, root.View(),
                                         request.manifestLimitBytes, existing, hasExisting, result);
        !read) {
        return read;
    }

    // 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹과 같다(결정 10).
    result.partial = hasExisting && !request.paths.IsEmpty();
    const CookedManifest* previous = hasExisting ? &existing : nullptr;
    if (result.partial) {
        // 병합 호환(프로파일·출력 설정·CookSettings·쿠커 버전)을 쿠킹 전에 본다 — 다르면 쿠킹하지
        // 않고 IncompatibleCook으로 끝낸다(Finish까지 가서 헛되이 쿠킹하지 않는다).
        CookedManifest header(allocator);
        if (auto assigned = header.profileId.TryAssign(request.profile->id.View()); !assigned) {
            return assigned;
        }

        header.profileSettings = request.profileSettings;
        header.cookSettings = request.cookSettings;
        header.cookerVersion = request.cookerVersion;
        if (auto compatible = CheckCookedManifestMerge(existing, header); !compatible) {
            if (auto pushed = result.diagnostics.TryPushBack(CookDiagnostic{
                    AssetDiagnosticCode::IncompatibleCook, false, {}, {}, 0, compatible.Error()});
                !pushed) {
                return pushed;
            }

            return WriteReport(allocator, ReportInputsOf(request), result, root.View());
        }
    }

    CookTargetRequest targetRequest;
    targetRequest.index = request.index;
    targetRequest.types = request.types;
    targetRequest.settings = request.settings;
    targetRequest.paths = result.partial ? request.paths : Span<const StringView>();
    targetRequest.existing = result.partial ? previous : nullptr;
    CookTargetSet targets(allocator);
    if (auto computed = ComputeCookTargets(allocator, targetRequest, targets); !computed) {
        return computed;
    }

    result.targets = targets.targets.Size();
    // 클래스 기본값 표(A5c): 등록된 구체 클래스의 레코드를 한 번 모두 만든다. 레코드 실패 클래스는
    // 쓰는 대상이 없어도 보고서 경고로 적는다(쓰는 대상은 cookKey 오류 — 아래 키 진단).
    CookClassDefaultsTable classDefaults(allocator);
    if (auto built = BuildCookClassDefaults(allocator, *request.types, classDefaults); !built) {
        return built;
    }

    CookKeyRequest keyRequest;
    keyRequest.index = request.index;
    keyRequest.files = request.files;
    keyRequest.types = request.types;
    keyRequest.processors = request.processors;
    keyRequest.targets = &targets;
    keyRequest.profileSettings = request.profileSettings;
    keyRequest.projectGeneration = request.projectGeneration;
    keyRequest.cookerVersion = request.cookerVersion;
    keyRequest.classDefaults = &classDefaults;
    CookKeySet keys(allocator);
    if (auto computed = ComputeCookKeys(allocator, keyRequest, keys); !computed) {
        return computed;
    }

    CookReuseRequest reuseRequest;
    reuseRequest.files = request.files;
    reuseRequest.cookedRoot = root.View();
    reuseRequest.existing = previous;
    reuseRequest.targets = &targets;
    reuseRequest.keys = &keys;
    reuseRequest.profileId = request.profile->id.View();
    reuseRequest.profileSettings = request.profileSettings;
    reuseRequest.cookerVersion = request.cookerVersion;
    // verify 쿠킹은 재사용하지 않는다(재사용 대상은 처리기를 거치지 않는다 — 계획은 빈 채로 둔다).
    CookReusePlan reuse(allocator);
    if (!request.verifyNuggets) {
        if (auto planned = PlanCookReuse(allocator, reuseRequest, reuse); !planned) {
            return planned;
        }
    }

    // 매니페스트 backend 표(10.3.1절 A4f2 8번 — 검증한 프로필이라 InvalidArgument는 나지 않는다).
    Array<CookedManifestBackend> backends(allocator);
    if (auto made = MakeCookedManifestBackends(allocator, *request.profile, backends); !made) {
        return made;
    }

    CookPublishConfig publishConfig;
    publishConfig.backends = Span<const CookedManifestBackend>(backends.Data(), backends.Size());
    publishConfig.files = request.files;
    publishConfig.savedDirectory = saved;
    publishConfig.profileId = request.profile->id.View();
    publishConfig.profileSettings = request.profileSettings;
    publishConfig.cookSettings = request.cookSettings;
    publishConfig.cookerVersion = request.cookerVersion;
    publishConfig.cookedBy = request.cookedBy;
    CookPublisher publisher(allocator, publishConfig);
    if (auto begun = publisher.Begin(*request.cookLock); !begun) {
        return begun;
    }

    for (usize at = 0; at < reuse.reuse.Size(); ++at) {
        if (reuse.reuse[at] == 0) {
            continue;
        }

        // 대상은 이 색인의 고유 항목이다(ComputeCookTargets).
        const AssetIndexEntry& entry = *request.index->FindById(targets.targets[at].id).Value();
        if (auto reused =
                publisher.Reuse(*reuse.entries[at], targets.targets[at], entry.path.View());
            !reused) {
            return reused;
        }
    }

    result.reused = reuse.count;
    CookRunRequest runRequest;
    runRequest.index = request.index;
    runRequest.packages = request.packages;
    runRequest.processors = request.processors;
    runRequest.targets = &targets;
    runRequest.keys = &keys;
    runRequest.profile = request.profile;
    runRequest.profileSettings = request.profileSettings;
    runRequest.cookerVersion = request.cookerVersion;
    runRequest.maxOpenPackages = request.maxOpenPackages;
    runRequest.reuse = reuse.reuse.View();
    runRequest.nuggets = request.nuggets;
    runRequest.verifyNuggets = request.verifyNuggets;
    runRequest.output = &publisher;
    CookRunResult run(allocator);
    if (auto cooked = CookPackages(allocator, runRequest, run); !cooked) {
        return cooked;
    }

    result.cooked = run.cooked;
    Result<void> merged = AppendDiagnostics(result.diagnostics, targets.diagnostics.View());
    if (merged) {
        merged = AppendDiagnostics(result.diagnostics, keys.diagnostics.View());
    }

    if (merged) {
        merged = AppendDiagnostics(result.diagnostics, run.diagnostics.View());
    }

    for (const CookClassDefaults& row : classDefaults.rows) {
        if (merged && row.availability == ClassDefaultsAvailability::Failed) {
            merged = result.diagnostics.TryPushBack(
                CookDiagnostic{AssetDiagnosticCode::InvalidClassDefaults,
                               true,
                               {},
                               AssetId{row.type},
                               0,
                               row.diagnostic.error});
        }
    }

    if (!merged) {
        return merged;
    }

    // 비에셋 산출물 단계는 에셋 오류가 있어도 돈다(보고서가 오류를 한 번에 모은다 — 10.3.1절 A4f3
    // 3번).
    if (auto staged = RunArtifactStages(allocator, StageInputsOf(request), root.View(), previous,
                                        publisher, result);
        !staged) {
        return staged;
    }

    // 오류 진단이 하나라도 있으면 게시하지 않는다(10절).
    if (!result.HasErrors()) {
        if (auto finished =
                publisher.Finish(result.partial ? previous : nullptr, targets.removed.View());
            !finished) {
            return finished;
        }
    }

    if (auto appended = AppendDiagnostics(result.diagnostics, publisher.Diagnostics()); !appended) {
        return appended;
    }

    result.published = publisher.Published();
    result.bytesWritten = publisher.BytesWritten();
    algo::Sort(result.diagnostics.View(), CookDiagnosticBefore);
    return WriteReport(allocator, ReportInputsOf(request), result, root.View());
}

Result<void> CookEngine(Allocator& allocator, const CookEngineRequest& request,
                        CookProjectResult& result) noexcept {
    ResetCookResult(result);
    if (request.files == nullptr || request.savedDirectory.IsEmpty() ||
        request.cookLock == nullptr || request.types == nullptr || request.registry == nullptr ||
        request.profile == nullptr || request.settings == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const StringView profileId = request.profile->id.View();
    if (!request.cookLock->IsHeldFor(request.savedDirectory, profileId)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<CookProjectInlineBytes> root(allocator);
    if (auto built = AppendCookedRoot(root, request.savedDirectory, profileId); !built) {
        return built;
    }

    CookedManifest existing(allocator);
    bool hasExisting = false;
    if (auto read = ReadPreviousManifest(allocator, *request.files, root.View(),
                                         request.manifestLimitBytes, existing, hasExisting, result);
        !read) {
        return read;
    }

    Array<CookedManifestBackend> backends(allocator);
    if (auto made = MakeCookedManifestBackends(allocator, *request.profile, backends); !made) {
        return made;
    }

    CookPublishConfig publishConfig;
    publishConfig.files = request.files;
    publishConfig.savedDirectory = request.savedDirectory;
    publishConfig.profileId = profileId;
    publishConfig.profileSettings = request.profileSettings;
    publishConfig.cookSettings = request.cookSettings;
    publishConfig.cookerVersion = request.cookerVersion;
    publishConfig.cookedBy = request.cookedBy;
    publishConfig.backends = Span<const CookedManifestBackend>(backends.Data(), backends.Size());
    CookPublisher publisher(allocator, publishConfig);
    if (auto begun = publisher.Begin(*request.cookLock); !begun) {
        return begun;
    }

    // 엔진 전용: `Engine` artifact만, 너겟은 단계가 엔진 root로 연다(nuggets null — 10.3.1절 A4f4
    // 3번).
    ArtifactStageInputs inputs;
    inputs.files = request.files;
    inputs.profile = request.profile;
    inputs.settings = request.settings;
    inputs.profileSettings = request.profileSettings;
    inputs.cookerVersion = request.cookerVersion;
    inputs.verifyNuggets = request.verifyNuggets;
    inputs.engineOnly = true;
    inputs.stages = request.stages;
    if (auto staged = RunArtifactStages(allocator, inputs, root.View(),
                                        hasExisting ? &existing : nullptr, publisher, result);
        !staged) {
        return staged;
    }

    // 늘 전체 쿠킹이다(병합 없음 — 가리키지 않는 `Shaders/` 파일을 지운다).
    if (!result.HasErrors()) {
        if (auto finished = publisher.Finish(nullptr, {}); !finished) {
            return finished;
        }
    }

    if (auto appended = AppendDiagnostics(result.diagnostics, publisher.Diagnostics()); !appended) {
        return appended;
    }

    result.published = publisher.Published();
    result.bytesWritten = publisher.BytesWritten();
    algo::Sort(result.diagnostics.View(), CookDiagnosticBefore);
    const CookReportInputs report{request.files,  request.types,   request.registry,
                                  request.access, request.profile, request.cookedBy};
    return WriteReport(allocator, report, result, root.View());
}

} // namespace rocks
