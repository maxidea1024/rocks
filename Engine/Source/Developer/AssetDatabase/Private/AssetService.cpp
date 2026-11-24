#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Assert.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Object/AssetId.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <ReflectionText/TextJson.h>
#include <ReflectionText/TextLines.h>

// 에셋 서비스 API다(Docs/Design/AssetSystem 13절·7.1절 15 A2c3).
namespace rocks {

namespace {

using asset_database_detail::AfterComponents;
using asset_database_detail::IsSameOrUnder;
using asset_database_detail::LogicalComponents;
using reflection_text::AssignmentTarget;
using reflection_text::TextAssignment;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;
using reflection_text::TextUse;

// JSON 출력의 inline byte 수다(넘으면 allocator로 자란다).
constexpr usize InfoJsonBytes = 256;

EditorPackageSetConfig MakeSetConfig(const AssetServiceConfig& config, const ProjectLayout& layout,
                                     AssetIndex& index) noexcept {
    EditorPackageSetConfig packages = config.packages;
    packages.layout = &layout;
    packages.index = &index;
    return packages;
}

// 공개 전 대표 에셋에 대입을 모두 검증·게시하는 초기화다(asset-new). 대입 경로는 override 집합이다
// (A5b — Create가 공개 전에 설치한다).
class AssignInitializer final : public EditorAssetInitializer {
public:
    AssignInitializer(Allocator& allocator, Span<const TextAssignment> assignments,
                      const TextCodecOptions& options, TextDiagnostics& diagnostics) noexcept
        : assignments_(assignments), options_(options), diagnostics_(diagnostics),
          overrides_(allocator) {
    }

    Result<void> Initialize(ConstructionContext& transaction,
                            ObjectHandle asset) noexcept override {
        auto prepared = reflection_text::PrepareAssignments(
            AssignmentTarget::Staged(transaction, asset), assignments_, options_, diagnostics_);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        if (!assignments_.IsEmpty()) {
            auto lease = transaction.AcquireType(asset);
            if (!lease) {
                return Result<void>::Failure(lease.Error());
            }

            if (auto planned = asset_database_detail::PlanAssignmentOverrides(
                    lease.Value(), assignments_, overrides_);
                !planned) {
                return planned;
            }
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

    const OverrideSet* InitialOverrides() const noexcept override {
        return &overrides_;
    }

private:
    Span<const TextAssignment> assignments_;
    const TextCodecOptions& options_;
    TextDiagnostics& diagnostics_;
    OverrideSet overrides_;
};

} // namespace

namespace asset_database_detail {

bool UnitsOverlap(const OverrideSet& left, const OverrideSet& right) noexcept {
    // 한쪽이 다른 쪽의 접두인지다(같은 단위 포함).
    auto prefixOf = [](Span<const Guid> shorter, Span<const Guid> longer) noexcept {
        if (shorter.Size() > longer.Size()) {
            return false;
        }

        for (usize index = 0; index < shorter.Size(); ++index) {
            if (!(shorter[index] == longer[index])) {
                return false;
            }
        }

        return true;
    };

    for (usize l = 0; l < left.Count(); ++l) {
        for (usize r = 0; r < right.Count(); ++r) {
            if (prefixOf(left.At(l), right.At(r)) || prefixOf(right.At(r), left.At(l))) {
                return true;
            }
        }
    }

    return false;
}

Result<void> PlanAssignmentOverrides(const TypeLease& lease,
                                     Span<const reflection_text::TextAssignment> assignments,
                                     OverrideSet& pending) noexcept {
    for (const reflection_text::TextAssignment& assignment : assignments) {
        const StringView text = assignment.path;
        usize end = text.ByteCount();
        for (usize index = 0; index < text.ByteCount(); ++index) {
            if (text.Data()[index] == '[') {
                end = index;
                break;
            }
        }

        // 해석되는 가장 긴 앞부분을 찾는다(검증된 대입이라 첫 필드는 늘 해석된다).
        PropertyPathBuffer path;
        bool parsed = false;
        while (end > 0 && !parsed) {
            parsed = static_cast<bool>(ParsePropertyPath(lease, text.SliceBytes(0, end), path));
            if (!parsed) {
                usize dot = end;
                while (dot > 0 && text.Data()[dot - 1] != '.') {
                    --dot;
                }

                end = dot > 0 ? dot - 1 : 0;
            }
        }

        // PrepareAssignments가 검증한 대입이라 첫 필드는 늘 해석된다(여기 오지 않는다).
        if (!parsed) {
            continue;
        }

        if (auto added =
                AddOverrideUnits(lease, Span<const PathStep>(path.steps, path.count), pending);
            !added && added.Error() == ErrorCode::OutOfMemory) {
            return added;
        }
    }

    return Result<void>::Success();
}

Result<void> CollectAssignedFields(Span<const reflection_text::TextAssignment> assignments,
                                   Array<StringView>& out) noexcept {
    for (const reflection_text::TextAssignment& assignment : assignments) {
        const StringView path = assignment.path;
        usize end = path.ByteCount();
        for (usize index = 0; index < path.ByteCount(); ++index) {
            const char c = path.Data()[index];
            if (c == '.' || c == '[') {
                end = index;
                break;
            }
        }

        const StringView name = path.SliceBytes(0, end);
        bool seen = false;
        for (StringView other : out) {
            seen = seen || other == name;
        }

        if (!seen) {
            if (auto pushed = out.TryPushBack(name); !pushed) {
                return pushed;
            }
        }
    }

    return Result<void>::Success();
}

} // namespace asset_database_detail

AssetService::AssetService(Allocator& allocator, const ProjectLayout& layout,
                           const AssetServiceConfig& config) noexcept
    : allocator_(&allocator), layout_(&layout), index_(allocator, layout, config.scheduler),
      set_(allocator, MakeSetConfig(config, layout, index_)), read_(config.packages.read),
      types_(config.packages.types), files_(config.packages.files),
      cacheFiles_(config.cacheFiles != nullptr ? config.cacheFiles : config.packages.files),
      useCache_(config.useCache), aliases_(allocator, layout), aliasLookup_(*this),
      clock_(config.clock), aliasDays_(config.aliasDays), aliasSearch_(config.aliasSearch),
      importers_(config.importers) {
    read_.expectedKind = PackageKind::Editor;
}

Result<void> AssetService::Initialize() noexcept {
    if (initialized_ || types_ == nullptr || files_ == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    AssetIndexCache cache(*allocator_);
    bool cached = false;
    if (useCache_) {
        auto loaded = cache.Load(*cacheFiles_, *layout_, read_);
        if (!loaded) {
            return Result<void>::Failure(loaded.Error());
        }

        cached = loaded.Value();
    }

    if (auto rebuilt = index_.Rebuild(*files_, read_, cached ? &cache : nullptr); !rebuilt) {
        return rebuilt;
    }

    if (auto opened = set_.Initialize(); !opened) {
        return opened;
    }

    initialized_ = true;
    return Result<void>::Success();
}

namespace {

// 이동하는 패키지 하나다(파일이 있으면 그 색인 항목, 없으면 저장하지 않은 새 패키지).
struct MovedPackage {
    explicit MovedPackage(Allocator& allocator) noexcept : oldPath(allocator), newPath(allocator) {
    }

    MovedPackage(MovedPackage&&) noexcept = default;
    MovedPackage& operator=(MovedPackage&&) noexcept = default;

    AssetId id{};
    String oldPath;
    String newPath;
    bool saved = false;
};

} // namespace

Result<void> AssetService::Move(const AssetDatabaseLock& lock, StringView source, StringView target,
                                AssetMoveResult& result, Array<EditorWarning>* warnings,
                                AssetDiagnostic* failure) noexcept {
    result = AssetMoveResult{};
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto from = ParseLogicalPath(*allocator_, source);
    auto to = ParseLogicalPath(*allocator_, target);
    if (!from || !to) {
        const ErrorCode error = !from ? from.Error() : to.Error();
        return FailAsset<void>(failure,
                               error == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               error);
    }

    const bool caseOnly = from.Value().Key() == to.Value().Key();
    if (!caseOnly && IsSameOrUnder(to.Value().Key(), from.Value().Key())) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    const StringView content = layout_->ContentDirectory();
    // Content 상대 OS 이름(파일은 `.rasset`까지, 디렉터리는 확장자 없이)이다.
    auto relativeOf = [&](const LogicalPath& path, bool directory, String& out) noexcept {
        String file(*allocator_);
        Result<void> made = layout_->TryMakeFilePath(path, file);
        if (!made) {
            return made;
        }

        const usize trim = directory ? EditorPackageExtension.ByteCount() : 0;
        return out.TryAssign(StringView(file.View().Data() + content.ByteCount() + 1,
                                        file.View().ByteCount() - content.ByteCount() - 1 - trim));
    };
    auto pathFailure = [&](ErrorCode error) noexcept {
        return FailAsset<void>(failure,
                               error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None
                                                               : AssetDiagnosticCode::InvalidPath,
                               error);
    };
    auto occupied = [&](AssetId related) noexcept {
        return FailAsset<void>(failure, AssetDiagnosticCode::DuplicatePath,
                               ErrorCode::AlreadyExists, {}, 0, 0, related);
    };

    // 원본 종류: 색인의 패키지, 저장하지 않은 새 패키지, 논리 디렉터리.
    Array<MovedPackage> moved(*allocator_);
    String fromRelative(*allocator_);
    String toRelative(*allocator_);
    bool directory = false;
    auto entry = index_.FindByPath(from.Value());
    if (!entry && entry.Error() == ErrorCode::AlreadyExists) {
        return FailAsset<void>(failure, AssetDiagnosticCode::DuplicatePath,
                               ErrorCode::AlreadyExists);
    }

    const usize components = LogicalComponents(from.Value().Text());
    auto addMoved = [&](AssetId id, StringView oldPath, bool saved) noexcept -> Result<void> {
        MovedPackage package(*allocator_);
        package.id = id;
        package.saved = saved;
        Result<void> step = package.oldPath.TryAssign(oldPath);
        if (step) {
            step = package.newPath.TryAssign(to.Value().Text());
        }

        if (step) {
            step = package.newPath.TryAppend(AfterComponents(oldPath, components));
        }

        if (step) {
            step = moved.TryPushBack(rocks::Move(package));
        }

        return step;
    };
    if (entry) {
        if (auto step = fromRelative.TryAssign(entry.Value()->fileName.View()); !step) {
            return step;
        }

        if (auto step = addMoved(entry.Value()->id, entry.Value()->path.View(), true); !step) {
            return step;
        }
    } else {
        // 저장하지 않은 새 패키지(같은 키) 또는 그 아래(디렉터리).
        for (usize index = 0; index < set_.Count(); ++index) {
            const EditorPackageInfo& info = set_.At(index);
            if (!info.unsaved) {
                continue;
            }

            auto path = ParseLogicalPath(*allocator_, info.logicalPath.View());
            if (!path) {
                return Result<void>::Failure(path.Error());
            }

            if (IsSameOrUnder(path.Value().Key(), from.Value().Key())) {
                directory = directory || path.Value().Key() != from.Value().Key();
                if (auto step = addMoved(info.id, path.Value().Text(), false); !step) {
                    return step;
                }
            }
        }

        Array<const AssetIndexEntry*> under(*allocator_);
        if (auto listed = index_.ListUnder(from.Value().Text(), true, under); !listed) {
            return listed;
        }

        for (const AssetIndexEntry* item : under) {
            directory = true;
            if (auto step = addMoved(item->id, item->path.View(), true); !step) {
                return step;
            }
        }

        // 원본의 OS 이름: 항목이 있으면 그 파일 이름의 앞 구성 요소(실제 철자), 없으면 배치 규칙
        // (새 패키지 하나는 파일 이름 `.rasset`, 디렉터리는 디렉터리 이름 — 대소문자 구분 FS에서
        // 철자가 다른 항목 없는 디렉터리는 찾지 못한다, 한계).
        if (!under.IsEmpty()) {
            const StringView file = under[0]->fileName.View();
            const StringView rest = AfterComponents(file, components - 1);
            if (auto step = fromRelative.TryAssign(
                    StringView(file.Data(), file.ByteCount() - rest.ByteCount()));
                !step) {
                return step;
            }

            // 논리 디렉터리가 대소문자만 다른 OS 디렉터리 둘 이상(대소문자 구분 FS)에 걸치면 한
            // 번의 OS 이동으로 옮길 수 없다(경로 키 충돌과 같은 거부 — 하나로 합친 뒤 옮긴다).
            for (const AssetIndexEntry* item : under) {
                if (!IsSameOrUnder(item->fileName.View(), fromRelative.View())) {
                    return FailAsset<void>(failure, AssetDiagnosticCode::DuplicatePath,
                                           ErrorCode::AlreadyExists, item->id);
                }
            }
        } else if (auto made = relativeOf(from.Value(), directory || moved.IsEmpty(), fromRelative);
                   !made) {
            return pathFailure(made.Error());
        }

        if (moved.IsEmpty()) {
            // 패키지가 없는 디렉터리도 옮긴다(빈 디렉터리·패키지 아닌 파일).
            String full(*allocator_);
            const StringView pieces[] = {content, "/", fromRelative.View()};
            for (StringView piece : pieces) {
                if (auto appended = full.TryAppend(piece); !appended) {
                    return appended;
                }
            }

            platform::FileInfo info;
            if (!files_->Stat(full.View().Data(), full.View().ByteCount(), info) ||
                info.type != platform::FileType::Directory) {
                return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::NotFound);
            }

            directory = true;
        }
    }

    if (auto made = relativeOf(to.Value(), directory, toRelative); !made) {
        return pathFailure(made.Error());
    }

    // 대상이 비어 있어야 한다(대소문자만 바꾸는 이름 변경 밖): 색인·Content 이름·새 패키지.
    if (!caseOnly) {
        auto existing = index_.FindByPath(to.Value());
        Array<const AssetIndexEntry*> under(*allocator_);
        if (auto listed = index_.ListUnder(to.Value().Text(), true, under); !listed) {
            return listed;
        }

        if (existing || existing.Error() == ErrorCode::AlreadyExists || !under.IsEmpty()) {
            return occupied(existing ? existing.Value()->id : AssetId{});
        }

        for (usize index = 0; index < set_.Count(); ++index) {
            if (!set_.At(index).unsaved) {
                continue;
            }

            auto path = ParseLogicalPath(*allocator_, set_.At(index).logicalPath.View());
            if (!path) {
                return Result<void>::Failure(path.Error());
            }

            if (IsSameOrUnder(path.Value().Key(), to.Value().Key())) {
                return occupied(set_.At(index).id);
            }
        }

        String full(*allocator_);
        const StringView pieces[] = {content, "/", toRelative.View()};
        for (StringView piece : pieces) {
            if (auto appended = full.TryAppend(piece); !appended) {
                return appended;
            }
        }

        platform::FileInfo info;
        const platform::FileStatus stat =
            files_->Stat(full.View().Data(), full.View().ByteCount(), info);
        if (stat) {
            return occupied({});
        }

        if (stat.code != platform::FileCode::NotFound) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, FileErrorCode(stat));
        }
    }

    // 별칭: 옮긴 패키지마다 옛 경로 → id(대소문자만이면 없음), 새 경로의 별칭이 같은 에셋이면
    // 지우고(되돌아오기 — UE TryMoveRedirector), 다른 에셋이면 점유.
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    AssetClock& clock = clock_ != nullptr ? *clock_ : systemClock_;
    auto now = DateTime::TryFromTicks(clock.NowTicks());
    if (!now) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, now.Error());
    }

    Array<AssetAliasInput> adds(*allocator_);
    Array<String> removeKeys(*allocator_);
    for (const MovedPackage& package : moved) {
        auto newPath = ParseLogicalPath(*allocator_, package.newPath.View());
        String check(*allocator_);
        Result<void> made = newPath ? layout_->TryMakeFilePath(newPath.Value(), check)
                                    : Result<void>::Failure(newPath.Error());
        if (!made) {
            return FailAsset<void>(failure,
                                   made.Error() == ErrorCode::OutOfMemory
                                       ? AssetDiagnosticCode::None
                                       : AssetDiagnosticCode::InvalidPath,
                                   made.Error(), package.id);
        }

        const usize alias = aliases.Value()->Find(newPath.Value().Key());
        if (alias != AssetAliasNone) {
            if (aliases.Value()->At(alias).asset != package.id) {
                return occupied(aliases.Value()->At(alias).asset);
            }

            String key(*allocator_);
            if (auto assigned = key.TryAssign(newPath.Value().Key()); !assigned) {
                return assigned;
            }

            if (auto pushed = removeKeys.TryPushBack(rocks::Move(key)); !pushed) {
                return pushed;
            }
        }

        if (package.saved && !caseOnly) {
            AssetAliasInput input;
            input.path = package.oldPath.View();
            input.asset = package.id;
            input.created = now.Value();
            input.days = aliasDays_;
            if (auto pushed = adds.TryPushBack(input); !pushed) {
                return pushed;
            }
        }
    }

    // 준비: 집합 경로 표와 색인(옛 이름의 지금 내용을 새 이름으로 — 이동 전에 읽는다).
    EditorRelocation relocation(*allocator_);
    if (auto prepared = set_.PrepareRelocate(from.Value(), to.Value(), fromRelative.View(),
                                             toRelative.View(), relocation, failure);
        !prepared) {
        return prepared;
    }

    // OS 이름은 별칭을 쓰기 전에 만든다(그 뒤의 실패는 OS 단계뿐 — 되돌리기가 그것만 다룬다).
    String fromFull(*allocator_);
    String toFull(*allocator_);
    const StringView fromPieces[] = {content, "/", fromRelative.View()};
    const StringView toPieces[] = {content, "/", toRelative.View()};
    for (StringView piece : fromPieces) {
        if (auto appended = fromFull.TryAppend(piece); !appended) {
            return appended;
        }
    }

    for (StringView piece : toPieces) {
        if (auto appended = toFull.TryAppend(piece); !appended) {
            return appended;
        }
    }

    // 원본이 디스크에 없으면(저장하지 않은 새 패키지뿐) OS 이동과 색인 갱신이 없다. 파일이 있어야
    // 할 패키지(색인 항목)가 있는데 없으면 MissingFile, Stat 오류는 그 오류다.
    platform::FileInfo sourceInfo;
    const platform::FileStatus sourceStat =
        files_->Stat(fromFull.View().Data(), fromFull.View().ByteCount(), sourceInfo);
    const bool onDisk = static_cast<bool>(sourceStat);
    // 저장하지 않은 새 패키지 하나인데 그 파일 이름이 디스크에 있으면 색인이 낡았다(색인에 없는
    // 파일) — 그 파일을 옮기지 않고 거부한다(asset-index-rebuild 뒤 다시 한다).
    if (onDisk && !entry && !directory) {
        return occupied({});
    }

    if (!onDisk) {
        if (sourceStat.code != platform::FileCode::NotFound) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, FileErrorCode(sourceStat));
        }

        for (const MovedPackage& package : moved) {
            if (package.saved) {
                return FailAsset<void>(failure, AssetDiagnosticCode::MissingFile,
                                       ErrorCode::NotFound, package.id);
            }
        }
    }

    AssetIndexRefresh plan(*allocator_);
    const AssetIndexMove indexMove{fromRelative.View(), toRelative.View()};
    AssetIndexRefreshPaths paths;
    if (onDisk) {
        paths.moves = Span<const AssetIndexMove>(&indexMove, 1);
    }

    if (auto prepared = index_.PrepareRefreshPaths(*files_, paths, read_, plan); !prepared) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, prepared.Error());
    }

    // 열린 패키지를 파일에서 뗀다(읽지 않은 bulk를 메모리로 — 이동 뒤 옛 경로를 읽지 않게).
    if (auto detached = set_.DetachUnder(fromRelative.View(), failure); !detached) {
        return detached;
    }

    // 별칭을 먼저 쓴다(OS 이동이 실패하면 되돌린다 — 이동 뒤 쓰기가 실패하면 고칠 길이 없다).
    Array<StringView> removeViews(*allocator_);
    for (const String& key : removeKeys) {
        if (auto pushed = removeViews.TryPushBack(key.View()); !pushed) {
            return pushed;
        }
    }

    // 경고 자리를 미리 잡는다(경고는 이 호출에 많아야 하나 — 성공의 NotDurable 또는 OS 실패의
    // AliasNotRestored, 바꾼 뒤에는 할당 실패가 없어야 한다).
    if (warnings != nullptr) {
        if (auto reserved = warnings->TryReserve(warnings->Size() + 1); !reserved) {
            return reserved;
        }
    }

    const bool writeAliases = !adds.IsEmpty() || !removeViews.IsEmpty();
    bool notDurable = false;
    if (writeAliases) {
        if (auto written = aliases.Value()->Write(*files_, lock, removeViews.View(), adds.View(),
                                                  notDurable, failure);
            !written) {
            return written;
        }
    }

    if (onDisk) {
        // 대상 부모(Content 또는 그 아래)를 만든다. toFull은 언제나 Content 뒤 '/'를 품는다.
        ErrorCode osError = ErrorCode::None;
        usize parent = toFull.View().ByteCount() - 1;
        while (toFull.View().Data()[parent] != '/') {
            --parent;
        }

        if (auto made = platform::MakeDirectoryTree(*files_, toFull.View().Data(), parent); !made) {
            osError = FileErrorCode(made);
        }

        if (osError == ErrorCode::None) {
            if (auto renamed = files_->Move(fromFull.View().Data(), fromFull.View().ByteCount(),
                                            toFull.View().Data(), toFull.View().ByteCount(), false);
                !renamed) {
                osError = FileErrorCode(renamed);
            }
        }

        if (osError != ErrorCode::None) {
            // 별칭을 되돌린다(되돌리지 못한 별칭은 옮기지 못한 옛 경로 → 그 에셋 — 색인이 이겨
            // 해롭지 않다, 경고로 알린다).
            if (writeAliases) {
                if (auto restored = aliases.Value()->Restore(*files_, lock);
                    !restored && warnings != nullptr) {
                    EditorWarning warning;
                    warning.kind = EditorWarningKind::AliasNotRestored;
                    warning.diagnostic.error = restored.Error();
                    // 자리는 위에서 잡았다.
                    (void)warnings->TryPushBack(warning);
                }
            }

            return FailAsset<void>(failure, AssetDiagnosticCode::None, osError);
        }
    }

    // 확정(실패 없음).
    set_.CommitRelocate(relocation);
    index_.CommitRefresh(plan);
    if (notDurable && warnings != nullptr) {
        EditorWarning warning;
        warning.kind = EditorWarningKind::NotDurable;
        warning.diagnostic.code = AssetDiagnosticCode::NotDurable;
        // 자리는 위에서 잡았다.
        (void)warnings->TryPushBack(warning);
    }

    result.packages = moved.Size();
    result.aliases = adds.Size();
    return Result<void>::Success();
}

Result<void> AssetService::Delete(const AssetDatabaseLock& lock, StringView target, bool force,
                                  AssetDeleteResult& result, Array<EditorWarning>* warnings,
                                  AssetDiagnostic* failure) noexcept {
    result.id = AssetId{};
    result.path.Clear();
    result.aliases = 0;
    result.referencerPaths.Clear();
    result.referencerIds.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 대상: 색인 항목 또는 저장하지 않은 새 패키지(별칭으로 풀지 않는다 — 지금 이름으로 고른다).
    const AssetIndexEntry* entry = nullptr;
    const EditorPackageInfo* unsaved = nullptr;
    if (target.StartsWith("/", StringComparison::CaseSensitive)) {
        auto path = ParseLogicalPath(*allocator_, target);
        if (!path) {
            return FailAsset<void>(failure,
                                   path.Error() == ErrorCode::InvalidArgument
                                       ? AssetDiagnosticCode::InvalidPath
                                       : AssetDiagnosticCode::None,
                                   path.Error());
        }

        auto found = index_.FindByPath(path.Value());
        if (found) {
            entry = found.Value();
        } else if (found.Error() == ErrorCode::AlreadyExists) {
            return FailAsset<void>(failure, AssetDiagnosticCode::DuplicatePath,
                                   ErrorCode::AlreadyExists);
        } else {
            for (usize index = 0; index < set_.Count() && unsaved == nullptr; ++index) {
                const EditorPackageInfo& info = set_.At(index);
                if (!info.unsaved) {
                    continue;
                }

                auto other = ParseLogicalPath(*allocator_, info.logicalPath.View());
                if (!other) {
                    return Result<void>::Failure(other.Error());
                }

                if (other.Value().Key() == path.Value().Key()) {
                    unsaved = &info;
                }
            }
        }
    } else {
        auto parsed = AssetId::TryParse(target);
        if (!parsed || parsed.Value().IsNull()) {
            return FailAsset<void>(failure, AssetDiagnosticCode::InvalidPath,
                                   ErrorCode::InvalidArgument);
        }

        auto found = index_.FindById(parsed.Value());
        if (found) {
            entry = found.Value();
        } else if (found.Error() == ErrorCode::AlreadyExists) {
            return FailAsset<void>(failure, AssetDiagnosticCode::DuplicateAssetId,
                                   ErrorCode::AlreadyExists, parsed.Value());
        } else {
            const EditorPackageInfo* info = set_.Find(parsed.Value());
            unsaved = info != nullptr && info->unsaved ? info : nullptr;
        }
    }

    if (entry == nullptr && unsaved == nullptr) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::NotFound);
    }

    // 경로로 고른 항목도 id가 겹치면 거부한다(어느 파일인지 id로 가릴 수 없다 — 먼저
    // asset-fix-duplicate-id).
    if (entry != nullptr && entry->duplicateId) {
        return FailAsset<void>(failure, AssetDiagnosticCode::DuplicateAssetId,
                               ErrorCode::AlreadyExists, entry->id);
    }

    const AssetId id = entry != nullptr ? entry->id : unsaved->id;
    result.id = id;
    if (auto assigned = result.path.TryAssign(entry != nullptr ? entry->path.View()
                                                               : unsaved->logicalPath.View());
        !assigned) {
        return assigned;
    }

    // 디스크 참조자(색인 — 모든 종류). 자신은 나오지 않는다(writer가 자기 의존을 빼고 reader가
    // 거부한다 — 5.4절 Dependencies).
    if (entry != nullptr) {
        Array<const AssetIndexEntry*> referencers(*allocator_);
        if (auto listed = index_.Referencers(id, AllDependencyKinds, referencers); !listed) {
            return listed;
        }

        for (const AssetIndexEntry* referencer : referencers) {
            String path(*allocator_);
            if (auto assigned = path.TryAssign(referencer->path.View()); !assigned) {
                return assigned;
            }

            if (auto pushed = result.referencerPaths.TryPushBack(rocks::Move(path)); !pushed) {
                return pushed;
            }

            if (auto pushed = result.referencerIds.TryPushBack(referencer->id); !pushed) {
                return pushed;
            }
        }

        if (!result.referencerIds.IsEmpty() && !force) {
            return FailAsset<void>(failure, AssetDiagnosticCode::ReferencedBy,
                                   ErrorCode::InvalidArgument, id, 0, 0, result.referencerIds[0]);
        }
    }

    // 메모리 참조(다른 열린 패키지 — force여도 거부: 끊긴 참조가 저장되지 않게).
    AssetId firstOpen;
    auto incoming = set_.IncomingReferences(id, &firstOpen);
    if (!incoming) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, incoming.Error(), id);
    }

    if (incoming.Value() != 0) {
        return FailAsset<void>(failure, AssetDiagnosticCode::ReferencedBy, ErrorCode::Busy, id, 0,
                               0, firstOpen);
    }

    // 대상을 가리키는 별칭(갈 곳이 없어진다 — 지운다).
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    Array<usize> aliasRows(*allocator_);
    for (usize row = 0; row < aliases.Value()->Count(); ++row) {
        if (aliases.Value()->At(row).asset == id) {
            if (auto pushed = aliasRows.TryPushBack(row); !pushed) {
                return pushed;
            }
        }
    }

    // 지울 별칭 키·경로 사본(쓰기가 표를 바꾼다)과 파일 이름·색인 준비는 바꾸기 전에 만든다.
    Array<String> aliasKeys(*allocator_);
    Array<String> aliasPaths(*allocator_);
    Array<StringView> removeViews(*allocator_);
    for (usize row : aliasRows) {
        String key(*allocator_);
        String path(*allocator_);
        Result<void> step = key.TryAssign(aliases.Value()->At(row).key.View());
        if (step) {
            step = path.TryAssign(aliases.Value()->At(row).path.View());
        }

        if (step) {
            step = aliasKeys.TryPushBack(rocks::Move(key));
        }

        if (step) {
            step = aliasPaths.TryPushBack(rocks::Move(path));
        }

        if (!step) {
            return step;
        }
    }

    for (const String& key : aliasKeys) {
        if (auto pushed = removeViews.TryPushBack(key.View()); !pushed) {
            return pushed;
        }
    }

    String full(*allocator_);
    AssetIndexRefresh plan(*allocator_);
    if (entry != nullptr) {
        const StringView pieces[] = {layout_->ContentDirectory(), "/", entry->fileName.View()};
        for (StringView piece : pieces) {
            if (auto appended = full.TryAppend(piece); !appended) {
                return appended;
            }
        }

        const StringView removed = entry->fileName.View();
        AssetIndexRefreshPaths paths;
        paths.removed = Span<const StringView>(&removed, 1);
        if (auto prepared = index_.PrepareRefreshPaths(*files_, paths, read_, plan); !prepared) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, prepared.Error(), id);
        }
    }

    // 경고 자리(지운 별칭마다 하나 + NotDurable 또는 AliasNotRestored 하나 — 바꾼 뒤에는 할당
    // 실패가 없다).
    if (warnings != nullptr) {
        if (auto reserved = warnings->TryReserve(warnings->Size() + aliasKeys.Size() + 1);
            !reserved) {
            return reserved;
        }
    }

    bool notDurable = false;
    const bool writeAliases = !removeViews.IsEmpty();
    if (writeAliases) {
        if (auto written =
                aliases.Value()->Write(*files_, lock, removeViews.View(), {}, notDurable, failure);
            !written) {
            return written;
        }
    }

    // 닫기·지우기 실패 뒤 별칭을 되돌린다(되돌리지 못하면 경고 — 남은 별칭은 아직 있는 에셋을
    // 가리킨다).
    auto restoreAliases = [&]() noexcept {
        if (!writeAliases) {
            return;
        }

        if (auto restored = aliases.Value()->Restore(*files_, lock);
            !restored && warnings != nullptr) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::AliasNotRestored;
            warning.diagnostic.error = restored.Error();
            warning.diagnostic.asset = id;
            // 자리는 위에서 잡았다.
            (void)warnings->TryPushBack(warning);
        }
    };

    // 집합에서 닫는다(Discard는 실패할 수 있어 파일을 지운 뒤로 미루지 않는다).
    if (set_.Find(id) != nullptr) {
        if (auto closed = set_.Close(id, failure); !closed) {
            restoreAliases();
            return closed;
        }
    }

    if (entry != nullptr) {
        // 파일이 이미 없으면(낡은 색인 — 밖에서 지웠다) 바라는 상태라 성공이다(색인을 확정한다).
        if (auto deleted = files_->Delete(full.View().Data(), full.View().ByteCount());
            !deleted && deleted.code != platform::FileCode::NotFound) {
            restoreAliases();
            return FailAsset<void>(failure, AssetDiagnosticCode::None, FileErrorCode(deleted), id);
        }

        index_.CommitRefresh(plan);
    }

    // 확정 뒤 경고(자리는 위에서 잡았다).
    if (warnings != nullptr) {
        for (const String& path : aliasPaths) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::AliasRemoved;
            warning.diagnostic.asset = id;
            for (usize index = 0; index < path.View().ByteCount(); ++index) {
                warning.text[index] = path.View().Data()[index];
            }

            (void)warnings->TryPushBack(warning);
        }

        if (notDurable) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::NotDurable;
            warning.diagnostic.code = AssetDiagnosticCode::NotDurable;
            (void)warnings->TryPushBack(warning);
        }
    }

    result.aliases = aliasKeys.Size();
    return Result<void>::Success();
}

Result<void> AssetService::DeleteOrphan(const AssetDatabaseLock& lock, StringView path,
                                        AssetDeleteResult& result, Array<EditorWarning>* warnings,
                                        AssetDiagnostic* failure) noexcept {
    result.id = AssetId{};
    result.path.Clear();
    result.aliases = 0;
    result.referencerPaths.Clear();
    result.referencerIds.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(*allocator_, path);
    if (!parsed) {
        return FailAsset<void>(failure,
                               parsed.Error() == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               parsed.Error());
    }

    auto entry = index_.FindByPath(parsed.Value());
    if (!entry) {
        return FailAsset<void>(failure,
                               entry.Error() == ErrorCode::AlreadyExists
                                   ? AssetDiagnosticCode::DuplicatePath
                                   : AssetDiagnosticCode::None,
                               entry.Error());
    }

    // 고아가 아니거나 판정할 수 없으면(읽지 못한 기록·패키지) 지우지 않는다.
    const AssetOrphanReason reason = index_.OrphanReason(*entry.Value());
    if (reason == AssetOrphanReason::None || reason == AssetOrphanReason::Undetermined) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument,
                               entry.Value()->id);
    }

    return Delete(lock, parsed.Value().Text(), false, result, warnings, failure);
}

Result<void> AssetService::FixDuplicateId(const AssetDatabaseLock& lock, StringView path,
                                          AssetFixIdResult& result, Array<EditorWarning>* warnings,
                                          AssetDiagnostic* failure) noexcept {
    result.path.Clear();
    result.oldId = AssetId{};
    result.newId = AssetId{};
    result.remaining.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(*allocator_, path);
    if (!parsed) {
        return FailAsset<void>(failure,
                               parsed.Error() == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               parsed.Error());
    }

    auto entry = index_.FindByPath(parsed.Value());
    if (!entry) {
        return FailAsset<void>(failure,
                               entry.Error() == ErrorCode::AlreadyExists
                                   ? AssetDiagnosticCode::DuplicatePath
                                   : AssetDiagnosticCode::None,
                               entry.Error());
    }

    const AssetId oldId = entry.Value()->id;
    result.oldId = oldId;
    if (auto assigned = result.path.TryAssign(entry.Value()->path.View()); !assigned) {
        return assigned;
    }

    if (!entry.Value()->duplicateId) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument,
                               oldId);
    }

    // 옛 id를 쓰는 남은 항목(경로 키 순)과 고칠 파일 이름 사본(쓰기 뒤 항목이 바뀐다).
    for (usize index = 0; index < index_.Count(); ++index) {
        const AssetIndexEntry& other = index_.At(index);
        if (!(other.id == oldId) || &other == entry.Value()) {
            continue;
        }

        String remaining(*allocator_);
        if (auto assigned = remaining.TryAssign(other.path.View()); !assigned) {
            return assigned;
        }

        if (auto pushed = result.remaining.TryPushBack(rocks::Move(remaining)); !pushed) {
            return pushed;
        }
    }

    String fileName(*allocator_);
    if (auto assigned = fileName.TryAssign(entry.Value()->fileName.View()); !assigned) {
        return assigned;
    }

    auto made = AssetId::TryNew();
    if (!made) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, made.Error(), oldId);
    }

    if (auto fixed = set_.FixDuplicateId(fileName.View(), made.Value(), lock, warnings, failure);
        !fixed) {
        return fixed;
    }

    result.newId = made.Value();
    return Result<void>::Success();
}

Result<void> AssetService::Duplicate(const AssetDatabaseLock& lock, StringView source,
                                     StringView target, AssetDuplicateResult& result,
                                     Array<EditorWarning>* warnings,
                                     AssetDiagnostic* failure) noexcept {
    result.id = AssetId{};
    result.path.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto to = ParseLogicalPath(*allocator_, target);
    if (!to) {
        return FailAsset<void>(failure,
                               to.Error() == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               to.Error());
    }

    // 별칭 점유(어느 에셋의 별칭이든 — 사본은 새 에셋이다).
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    const usize alias = aliases.Value()->Find(to.Value().Key());
    if (alias != AssetAliasNone) {
        return FailAsset<void>(failure, AssetDiagnosticCode::DuplicatePath,
                               ErrorCode::AlreadyExists, {}, 0, 0,
                               aliases.Value()->At(alias).asset);
    }

    AssetId id;
    if (auto opened = OpenTarget(source, id, warnings, failure); !opened) {
        return Result<void>::Failure(opened.Error());
    }

    auto made = AssetId::TryNew();
    if (!made) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, made.Error(), id);
    }

    if (auto assigned = result.path.TryAssign(to.Value().Text()); !assigned) {
        return assigned;
    }

    if (auto duplicated = set_.Duplicate(id, to.Value(), made.Value(), lock, warnings, failure);
        !duplicated) {
        return duplicated;
    }

    result.id = made.Value();
    return Result<void>::Success();
}

Result<AssetAliasTable*> AssetService::Aliases(AssetDiagnostic* failure) noexcept {
    if (!initialized_) {
        return FailAsset<AssetAliasTable*>(failure, AssetDiagnosticCode::None,
                                           ErrorCode::InvalidArgument);
    }

    if (!aliases_.IsLoaded()) {
        if (auto loaded = aliases_.Load(*files_, failure); !loaded) {
            return Result<AssetAliasTable*>::Failure(loaded.Error());
        }
    }

    return Result<AssetAliasTable*>::Success(&aliases_);
}

Result<AssetId> AssetService::AliasLookup::ResolveAlias(const LogicalPath& path,
                                                        Array<EditorWarning>* warnings) noexcept {
    return service_->ResolveAliasPath(path, warnings, nullptr);
}

Result<AssetId> AssetService::ResolveAliasPath(const LogicalPath& path,
                                               Array<EditorWarning>* warnings,
                                               AssetDiagnostic* failure) noexcept {
    AssetDiagnostic unreadable;
    auto aliases = Aliases(&unreadable);
    if (!aliases) {
        if (failure != nullptr) {
            *failure = unreadable;
        }

        // 원인을 경고로도 남긴다(참조 값 해석은 진단 칸 없이 부른다 — 대입 진단 Unresolved만으로는
        // 원인을 알 수 없다).
        if (warnings != nullptr && aliases.Error() != ErrorCode::OutOfMemory) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::AliasTableUnreadable;
            warning.diagnostic = unreadable;
            if (auto pushed = warnings->TryPushBack(warning); !pushed) {
                return Result<AssetId>::Failure(pushed.Error());
            }
        }

        return Result<AssetId>::Failure(aliases.Error());
    }

    const usize at = aliases.Value()->Find(path.Key());
    if (at == AssetAliasNone) {
        return Result<AssetId>::Failure(ErrorCode::NotFound);
    }

    const AssetId target = aliases.Value()->At(at).asset;
    if (warnings != nullptr) {
        EditorWarning warning;
        warning.kind = EditorWarningKind::AliasResolved;
        warning.diagnostic.code = AssetDiagnosticCode::AliasResolved;
        warning.diagnostic.asset = target;
        warning.diagnostic.detail = at;
        // 옛 경로 텍스트를 경고에 담는다(뒤에 별칭 표가 바뀌어도 그대로다 — 논리 경로 상한 안).
        const StringView old = aliases.Value()->At(at).path.View();
        for (usize index = 0; index < old.ByteCount(); ++index) {
            warning.text[index] = old.Data()[index];
        }
        if (auto pushed = warnings->TryPushBack(warning); !pushed) {
            return Result<AssetId>::Failure(pushed.Error());
        }
    }

    return Result<AssetId>::Success(target);
}

Result<void> AssetService::AliasRemove(const AssetDatabaseLock& lock, StringView path,
                                       bool& notDurable, AssetDiagnostic* failure) noexcept {
    notDurable = false;
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    auto parsed = ParseLogicalPath(*allocator_, path);
    if (!parsed) {
        return FailAsset<void>(failure,
                               parsed.Error() == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               parsed.Error());
    }

    if (aliases.Value()->Find(parsed.Value().Key()) == AssetAliasNone) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::NotFound);
    }

    const StringView keys[] = {parsed.Value().Key()};
    return aliases.Value()->Write(*files_, lock, keys, {}, notDurable, failure);
}

Result<void> AssetService::AliasPrune(const AssetDatabaseLock& lock, AssetAliasPruneReport& report,
                                      AssetDiagnostic* failure) noexcept {
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    AssetClock& clock = clock_ != nullptr ? *clock_ : systemClock_;
    return aliases.Value()->Prune(*files_, lock, clock.NowTicks(), aliasSearch_, report, failure);
}

Result<void> AssetService::FlushCache(const AssetDatabaseLock& lock) noexcept {
    if (!initialized_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return WriteAssetIndexCache(*cacheFiles_, *layout_, lock, index_, read_);
}

Result<TypeLease> AssetService::FindClass(StringView name, bool requireAsset,
                                          AssetDiagnostic* failure) noexcept {
    auto lease = types_->AcquireByToolName(Span<const char>(name.Data(), name.ByteCount()));
    if (!lease) {
        return FailAsset<TypeLease>(failure, AssetDiagnosticCode::None, lease.Error());
    }

    const TypeLease& type = lease.Value();
    const bool asset = type.Kind() == TypeKind::Class && type.IsA(GeneratedType<Asset>::Id());
    const bool abstract = HasFlag(type.Schema().flags, TypeFlags::Abstract);
    if (type.Kind() != TypeKind::Class || (requireAsset && (!asset || abstract))) {
        return FailAsset<TypeLease>(failure, AssetDiagnosticCode::TypeMismatch,
                                    ErrorCode::InvalidArgument);
    }

    return lease;
}

Result<ObjectHandle> AssetService::OpenTarget(StringView target, AssetId& id,
                                              Array<EditorWarning>* warnings,
                                              AssetDiagnostic* failure) noexcept {
    Result<ObjectHandle> opened = Result<ObjectHandle>::Failure(ErrorCode::InvalidArgument);
    if (target.StartsWith("/", StringComparison::CaseSensitive)) {
        auto path = ParseLogicalPath(*allocator_, target);
        if (!path) {
            return FailAsset<ObjectHandle>(failure,
                                           path.Error() == ErrorCode::InvalidArgument
                                               ? AssetDiagnosticCode::InvalidPath
                                               : AssetDiagnosticCode::None,
                                           path.Error());
        }

        // 저장하지 않은 새 패키지는 색인에 없다 — 같은 경로 키면 그 패키지다(색인과 겹치지 않는다
        // — Create가 거부한다).
        for (usize index = 0; index < set_.Count(); ++index) {
            const EditorPackageInfo& info = set_.At(index);
            if (!info.unsaved) {
                continue;
            }

            auto other = ParseLogicalPath(*allocator_, info.logicalPath.View());
            if (!other) {
                return FailAsset<ObjectHandle>(failure, AssetDiagnosticCode::None, other.Error());
            }

            if (other.Value().Key() == path.Value().Key()) {
                id = info.id;
                return set_.Registry().FindChild(set_.Access(), info.package, "");
            }
        }

        AssetDiagnostic missing;
        opened = set_.Open(path.Value(), warnings, &missing);
        // 색인에 없는 경로(진단 None의 NotFound — 파일이 사라진 MissingFile과 다르다)는 별칭으로
        // 푼다(7.1절 17 A2e1).
        if (!opened && opened.Error() == ErrorCode::NotFound &&
            missing.code == AssetDiagnosticCode::None) {
            auto aliased = ResolveAliasPath(path.Value(), warnings, failure);
            if (!aliased) {
                if (aliased.Error() == ErrorCode::NotFound && failure != nullptr) {
                    *failure = missing;
                }

                return Result<ObjectHandle>::Failure(aliased.Error());
            }

            opened = set_.Open(aliased.Value(), warnings, failure);
        } else if (failure != nullptr && !opened) {
            *failure = missing;
        }
    } else {
        auto parsed = AssetId::TryParse(target);
        if (!parsed || parsed.Value().IsNull()) {
            return FailAsset<ObjectHandle>(failure, AssetDiagnosticCode::InvalidPath,
                                           ErrorCode::InvalidArgument);
        }

        opened = set_.Open(parsed.Value(), warnings, failure);
    }

    if (!opened) {
        return opened;
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        if (set_.At(index).package == opened.Value()) {
            id = set_.At(index).id;
        }
    }

    // 열린 에디터 패키지에는 대표 에셋이 있다(열기가 대표 에셋을 읽고 타입을 대조한다).
    auto representative = set_.Registry().FindChild(set_.Access(), opened.Value(), "");
    if (!representative) {
        return FailAsset<ObjectHandle>(failure, AssetDiagnosticCode::None, representative.Error(),
                                       id);
    }

    return representative;
}

Result<AssetId> AssetService::New(const AssetDatabaseLock& lock, StringView type, StringView path,
                                  Span<const TextAssignment> assignments,
                                  TextDiagnostics& diagnostics, Array<EditorWarning>* warnings,
                                  AssetDiagnostic* failure) noexcept {
    if (!initialized_) {
        return FailAsset<AssetId>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(*allocator_, path);
    if (!parsed) {
        return FailAsset<AssetId>(failure,
                                  parsed.Error() == ErrorCode::InvalidArgument
                                      ? AssetDiagnosticCode::InvalidPath
                                      : AssetDiagnosticCode::None,
                                  parsed.Error());
    }

    // 별칭이 있는 경로는 점유된 것이다(7.1절 12·17 — 별칭을 지운 뒤 쓴다).
    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<AssetId>::Failure(aliases.Error());
    }

    if (const usize alias = aliases.Value()->Find(parsed.Value().Key()); alias != AssetAliasNone) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = AssetDiagnosticCode::DuplicatePath;
            failure->error = ErrorCode::AlreadyExists;
            failure->related = aliases.Value()->At(alias).asset;
        }

        return Result<AssetId>::Failure(ErrorCode::AlreadyExists);
    }

    auto lease = FindClass(type, true, failure);
    if (!lease) {
        return Result<AssetId>::Failure(lease.Error());
    }

    AssetTextResolver resolver(*allocator_, set_, index_, warnings);
    resolver.SetAliases(&aliasLookup_);
    const TextCodecOptions options = asset_database_detail::CodecOptions(set_, resolver);
    AssignInitializer initializer(*allocator_, assignments, options, diagnostics);
    auto created = set_.Create(parsed.Value(), lease.Value().Id(), {}, failure, &initializer);
    if (!created) {
        return Result<AssetId>::Failure(created.Error());
    }

    // 방금 만든 패키지다(집합이 연 정보가 있다).
    AssetId id{};
    for (usize index = 0; index < set_.Count(); ++index) {
        if (set_.At(index).package == created.Value()) {
            id = set_.At(index).id;
        }
    }

    EditorSaveOptions save;
    if (auto saved = set_.Save(id, lock, save, warnings, failure); !saved) {
        return Result<AssetId>::Failure(saved.Error());
    }

    return Result<AssetId>::Success(id);
}

Result<void> AssetService::Set(const AssetDatabaseLock& lock, StringView target,
                               Span<const TextAssignment> assignments, bool force,
                               TextDiagnostics& diagnostics, Array<EditorWarning>* warnings,
                               AssetDiagnostic* failure) noexcept {
    if (!initialized_) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    AssetId id{};
    auto asset = OpenTarget(target, id, warnings, failure);
    if (!asset) {
        return Result<void>::Failure(asset.Error());
    }

    // Reset(`--reset=` — 기본값 되돌리기 A5b2)은 텍스트 묶음과 떼어 binary로 재생한다.
    Array<TextAssignment> texts(*allocator_);
    Array<TextAssignment> resets(*allocator_);
    for (const TextAssignment& assignment : assignments) {
        Array<TextAssignment>& into =
            assignment.kind == reflection_text::AssignmentKind::Reset ? resets : texts;
        if (!into.TryPushBack(assignment)) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }
    }

    // 사전 단계: Reset 경로를 해석해 모두 계획한다(어떤 쓰기보다 먼저 — ReadOnly·범위·값 판정).
    // 지울 override는 그 경로로 시작하는 단위 전부다(경로는 필드 단계만이라 단위 접두로 쓴다).
    Array<RevertPlan> plans(*allocator_);
    OverrideSet resetUnits(*allocator_);
    OverrideSet pending(*allocator_);
    {
        auto lease = set_.Registry().AcquireType(set_.Access(), asset.Value());
        if (!lease) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, lease.Error());
        }

        if (!plans.TryReserve(resets.Size())) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }

        for (const TextAssignment& reset : resets) {
            PropertyPathBuffer path;
            if (auto parsed = ParsePropertyPath(lease.Value(), reset.path, path); !parsed) {
                return FailAsset<void>(failure, AssetDiagnosticCode::None, parsed.Error());
            }

            RevertPlan plan(*allocator_);
            if (auto planned = PlanRevert(set_.Registry(), set_.Access(), asset.Value(), path,
                                          *allocator_, plan);
                !planned) {
                return FailAsset<void>(failure, AssetDiagnosticCode::None, planned.Error());
            }

            Guid prefix[MaxPropertyPathSteps];
            for (usize index = 0; index < path.count; ++index) {
                prefix[index] = path.steps[index].fieldId;
            }

            if (auto added = resetUnits.Add(Span<const Guid>(prefix, path.count)); !added) {
                return FailAsset<void>(failure, AssetDiagnosticCode::None, added.Error());
            }

            (void)plans.TryPushBack(Move(plan));
        }
    }

    AssetTextResolver resolver(*allocator_, set_, index_, warnings);
    resolver.SetAliases(&aliasLookup_);
    resolver.SetWeakScope(id);
    const TextCodecOptions options = asset_database_detail::CodecOptions(set_, resolver);
    const AssignmentTarget live =
        AssignmentTarget::Live(set_.Registry(), set_.Access(), asset.Value());
    // 텍스트 대입을 검증한다(Reset이 있으면 검증만 하고 버린다 — 되돌리기 쓰기 뒤 Publish는 Busy라
    // 되돌리기 뒤에 다시 준비한다).
    auto prepared = reflection_text::PrepareAssignments(live, texts.View(), options, diagnostics);
    if (!prepared) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, prepared.Error());
    }

    // 경로 쓰기는 override 집합에 기록한다(A5b — 기준과 같은 값을 넣어도 저장되어 기본값 변경
    // 뒤에도 남는다). 단위 계산과 자리 예약은 게시 전에 하고 게시 뒤에는 실패 없이 합친다. 최종
    // 집합 = 지금 집합 − Reset 단위 ∪ 대입 단위다.
    {
        auto lease = set_.Registry().AcquireType(set_.Access(), asset.Value());
        if (!lease) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, lease.Error());
        }

        if (auto planned = asset_database_detail::PlanAssignmentOverrides(lease.Value(),
                                                                          texts.View(), pending);
            !planned) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, planned.Error());
        }
    }

    // 같은 단위(한쪽이 다른 쪽 접두 — 펼친 잎 포함)에 Reset과 텍스트 대입을 섞으면 순서가 결과를
    // 바꾸므로 사용법 오류다.
    if (asset_database_detail::UnitsOverlap(resetUnits, pending)) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    if (auto reserved = set_.ReserveOverrides(asset.Value(), pending); !reserved) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    // 되돌리기를 적용한 뒤의 실패는 일부만 바뀐 객체를 남기므로 패키지를 dirty로 표시하고 저장하지
    // 않는다(다음 저장·닫기가 정리한다 — RuntimeClassMetadata 'A5b2 decode 명세' 원자성).
    // MarkDirty는 열린 패키지에 실패하지 않는다.
    const auto failApplied = [&](ErrorCode error) noexcept {
        (void)set_.MarkDirty(id);
        return FailAsset<void>(failure, AssetDiagnosticCode::None, error);
    };
    if (!plans.IsEmpty()) {
        // 되돌리기는 묶음 맨 앞이다. 남는 실패는 자원·상태 오류뿐이다.
        for (const RevertPlan& plan : plans) {
            if (auto applied = ApplyRevert(set_.Registry(), set_.Access(), plan); !applied) {
                return failApplied(applied.Error());
            }
        }

        prepared = reflection_text::PrepareAssignments(live, texts.View(), options, diagnostics);
        if (!prepared) {
            return failApplied(prepared.Error());
        }
    }

    if (auto published = prepared.Value().Publish(); !published) {
        return plans.IsEmpty()
                   ? FailAsset<void>(failure, AssetDiagnosticCode::None, published.Error())
                   : failApplied(published.Error());
    }

    set_.MergeOverrides(asset.Value(), pending);
    set_.RemoveOverridesUnder(asset.Value(), resetUnits);

    // 설정 정규화(12.12절 — UE ValidateSettingsAfterImportOrEdit): 대입·되돌린 최상위 필드를
    // 넘긴다. 실패는 바뀐 객체를 남기므로 dirty로 두고 저장하지 않는다.
    {
        Array<StringView> assigned(*allocator_);
        Result<void> normalized =
            asset_database_detail::CollectAssignedFields(texts.View(), assigned);
        if (normalized) {
            normalized = asset_database_detail::CollectAssignedFields(resets.View(), assigned);
        }

        if (normalized) {
            normalized = set_.NormalizeSettings(asset.Value(), id, assigned.View());
        }

        if (!normalized) {
            return failApplied(normalized.Error());
        }
    }

    // 열린 패키지다(MarkDirty는 실패하지 않는다).
    (void)set_.MarkDirty(id);
    EditorSaveOptions save;
    save.dropUnresolved = force;
    return set_.Save(id, lock, save, warnings, failure);
}

Result<void> AssetService::Info(StringView target, const AssetInfoOptions& options, String& out,
                                Array<EditorWarning>* warnings, AssetDiagnostic* failure) noexcept {
    if (!initialized_) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    AssetId id{};
    auto asset = OpenTarget(target, id, warnings, failure);
    if (!asset) {
        return Result<void>::Failure(asset.Error());
    }

    AssetTextResolver resolver(*allocator_, set_, index_, warnings);
    resolver.SetAliases(&aliasLookup_);
    TextCodecOptions codec = asset_database_detail::CodecOptions(set_, resolver);
    codec.use = options.all ? TextUse::Report : TextUse::Edit;
    const AssignmentTarget live =
        AssignmentTarget::Live(set_.Registry(), set_.Access(), asset.Value());
    if (!options.json) {
        if (auto written = reflection_text::ExportLines(live, out, codec); !written) {
            return FailAsset<void>(failure, AssetDiagnosticCode::None, written.Error());
        }

        return Result<void>::Success();
    }

    StringBuilder<InfoJsonBytes> text(*allocator_);
    JsonWriter writer(text);
    if (auto written = reflection_text::ExportJson(live, writer, codec); !written) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, written.Error());
    }

    if (auto appended = out.TryAppend(text.View()); !appended) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, appended.Error());
    }

    return Result<void>::Success();
}

Result<void> AssetService::List(StringView type, StringView under,
                                Array<const AssetIndexEntry*>& out,
                                AssetDiagnostic* failure) noexcept {
    if (!initialized_) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    TypeId filter{};
    if (!type.IsEmpty()) {
        auto lease = FindClass(type, false, failure);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        filter = lease.Value().Id();
    }

    Array<const AssetIndexEntry*> listed(*allocator_);
    if (auto found = index_.ListUnder(under.IsEmpty() ? GameMount : under, true, listed); !found) {
        return FailAsset<void>(failure,
                               found.Error() == ErrorCode::InvalidArgument
                                   ? AssetDiagnosticCode::InvalidPath
                                   : AssetDiagnosticCode::None,
                               found.Error());
    }

    const usize before = out.Size();
    if (auto reserved = out.TryReserve(out.Size() + listed.Size()); !reserved) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (const AssetIndexEntry* entry : listed) {
        if (filter != TypeId{} && !(entry->type == filter)) {
            // 파생 판정은 AssetIndex::ListByType과 같은 규칙이다(7.1절 11·14): 등록되지 않은
            // 타입(NotFound)·파생 아님(TypeMismatch)은 맞지 않고 그 밖의 오류는 돌려준다.
            auto derived = types_->IsA(entry->type, filter);
            if (!derived) {
                if (derived.Error() == ErrorCode::NotFound ||
                    derived.Error() == ErrorCode::TypeMismatch) {
                    continue;
                }

                // 실패하면 out을 호출 전 길이로 되돌린다(줄이기는 실패하지 않는다).
                (void)out.TryResize(before);
                return FailAsset<void>(failure, AssetDiagnosticCode::None, derived.Error());
            }

            if (!derived.Value()) {
                continue;
            }
        }

        (void)out.TryPushBack(entry);
    }

    return Result<void>::Success();
}

} // namespace rocks
