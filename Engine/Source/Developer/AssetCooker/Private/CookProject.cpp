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

// 보고서를 만들어 쿠킹 루트에 원자 교체로 쓴다.
Result<void> WriteReport(Allocator& allocator, const CookProjectRequest& request,
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

    auto lease = request.types->AcquireValueType(GeneratedType<CookReport>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    EditorPackageSet& packages = *request.packages;
    const AssignmentTarget target = AssignmentTarget::Value(packages.Registry(), packages.Access(),
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

} // namespace

Result<void> CookProject(Allocator& allocator, const CookProjectRequest& request,
                         CookProjectResult& result) noexcept {
    result.diagnostics.Clear();
    result.targets = 0;
    result.cooked = 0;
    result.reused = 0;
    result.bytesWritten = 0;
    result.partial = false;
    result.published = false;
    if (request.files == nullptr || request.layout == nullptr || request.lock == nullptr ||
        request.index == nullptr || request.packages == nullptr || request.types == nullptr ||
        request.processors == nullptr || request.profile == nullptr ||
        request.settings == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    StringBuilder<CookProjectInlineBytes> root(allocator);
    if (auto built = AppendCookedRoot(root, *request.layout, request.profile->id.View()); !built) {
        return built;
    }

    // 기존 매니페스트다(없거나 깨졌으면 없음 — 깨진 것은 경고).
    StringBuilder<CookProjectInlineBytes> manifestPath(allocator);
    const StringView manifestPieces[] = {root.View(), "/", CookedManifestFileName};
    if (auto built = manifestPath.TryAppendJoin(manifestPieces, ""); !built) {
        return built;
    }

    Array<uint8> manifestBytes(allocator);
    bool existed = false;
    if (auto read = ReadWholeFile(*request.files, manifestPath.View(), CookedManifestMaxFileBytes,
                                  manifestBytes, existed);
        !read) {
        return read;
    }

    CookedManifest existing(allocator);
    bool hasExisting = false;
    if (existed) {
        AssetDiagnostic failure;
        auto parsed = ReadCookedManifest(allocator, manifestBytes.View(), &failure);
        if (parsed) {
            existing = Move(parsed.Value());
            hasExisting = true;
        } else if (parsed.Error() == ErrorCode::OutOfMemory) {
            return Result<void>::Failure(parsed.Error());
        } else if (auto pushed = result.diagnostics.TryPushBack(
                       CookDiagnostic{failure.code, true, {}, {}, 0, parsed.Error()});
                   !pushed) {
            return pushed;
        }
    }

    // 기존 매니페스트가 없으면 부분 쿠킹은 전체 쿠킹과 같다(결정 10).
    result.partial = hasExisting && !request.paths.IsEmpty();
    const CookedManifest* previous = hasExisting ? &existing : nullptr;

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
    CookKeyRequest keyRequest;
    keyRequest.index = request.index;
    keyRequest.files = request.files;
    keyRequest.types = request.types;
    keyRequest.processors = request.processors;
    keyRequest.targets = &targets;
    keyRequest.profileSettings = request.profileSettings;
    keyRequest.projectGeneration = request.projectGeneration;
    keyRequest.cookerVersion = request.cookerVersion;
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
    CookReusePlan reuse(allocator);
    if (auto planned = PlanCookReuse(allocator, reuseRequest, reuse); !planned) {
        return planned;
    }

    CookPublishConfig publishConfig;
    publishConfig.files = request.files;
    publishConfig.layout = request.layout;
    publishConfig.profileId = request.profile->id.View();
    publishConfig.profileSettings = request.profileSettings;
    publishConfig.cookSettings = request.cookSettings;
    publishConfig.cookerVersion = request.cookerVersion;
    publishConfig.cookedBy = request.cookedBy;
    CookPublisher publisher(allocator, publishConfig);
    if (auto begun = publisher.Begin(*request.lock); !begun) {
        return begun;
    }

    for (usize at = 0; at < targets.targets.Size(); ++at) {
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

    if (!merged) {
        return merged;
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
    return WriteReport(allocator, request, result, root.View());
}

} // namespace rocks
