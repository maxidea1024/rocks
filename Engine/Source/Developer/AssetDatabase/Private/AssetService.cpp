#include <AssetDatabase/AssetIndexCache.h>
#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Assert.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Object/AssetId.h>
#include <ReflectionText/TextJson.h>
#include <ReflectionText/TextLines.h>

// 에셋 서비스 API다(Docs/Design/AssetSystem.ko.md 13절·7.1절 15 A2c3).
namespace rocks {

namespace {

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

// 대입 codec 옵션이다(Edit 용도, 참조는 resolver, Name은 집합 Registry의 pool).
TextCodecOptions CodecOptions(EditorPackageSet& set, AssetTextResolver& resolver) noexcept {
    TextCodecOptions options;
    options.resolver = &resolver;
    options.names = set.Registry().Names();
    return options;
}

// 공개 전 대표 에셋에 대입을 모두 검증·게시하는 초기화다(asset-new).
class AssignInitializer final : public EditorAssetInitializer {
public:
    AssignInitializer(Span<const TextAssignment> assignments, const TextCodecOptions& options,
                      TextDiagnostics& diagnostics) noexcept
        : assignments_(assignments), options_(options), diagnostics_(diagnostics) {
    }

    Result<void> Initialize(ConstructionContext& transaction,
                            ObjectHandle asset) noexcept override {
        auto prepared = reflection_text::PrepareAssignments(
            AssignmentTarget::Staged(transaction, asset), assignments_, options_, diagnostics_);
        if (!prepared) {
            return Result<void>::Failure(prepared.Error());
        }

        auto published = prepared.Value().Publish();
        return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
    }

private:
    Span<const TextAssignment> assignments_;
    const TextCodecOptions& options_;
    TextDiagnostics& diagnostics_;
};

} // namespace

AssetService::AssetService(Allocator& allocator, const ProjectLayout& layout,
                           const AssetServiceConfig& config) noexcept
    : allocator_(&allocator), layout_(&layout), index_(allocator, layout, config.scheduler),
      set_(allocator, MakeSetConfig(config, layout, index_)), read_(config.packages.read),
      types_(config.packages.types), files_(config.packages.files),
      cacheFiles_(config.cacheFiles != nullptr ? config.cacheFiles : config.packages.files),
      useCache_(config.useCache), aliases_(allocator, layout), aliasLookup_(*this),
      clock_(config.clock), aliasSearch_(config.aliasSearch) {
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

StringView AssetService::AliasPath(uint64 detail) const noexcept {
    ROCKS_ASSERT(detail < aliases_.Count());
    return aliases_.At(static_cast<usize>(detail)).path.View();
}

Result<AssetId> AssetService::AliasLookup::ResolveAlias(const LogicalPath& path,
                                                        Array<EditorWarning>* warnings) noexcept {
    return service_->ResolveAliasPath(path, warnings, nullptr);
}

Result<AssetId> AssetService::ResolveAliasPath(const LogicalPath& path,
                                               Array<EditorWarning>* warnings,
                                               AssetDiagnostic* failure) noexcept {
    auto aliases = Aliases(failure);
    if (!aliases) {
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
    const TextCodecOptions options = CodecOptions(set_, resolver);
    AssignInitializer initializer(assignments, options, diagnostics);
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

    AssetTextResolver resolver(*allocator_, set_, index_, warnings);
    resolver.SetAliases(&aliasLookup_);
    resolver.SetWeakScope(id);
    const TextCodecOptions options = CodecOptions(set_, resolver);
    auto prepared = reflection_text::PrepareAssignments(
        AssignmentTarget::Live(set_.Registry(), set_.Access(), asset.Value()), assignments, options,
        diagnostics);
    if (!prepared) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, prepared.Error());
    }

    if (auto published = prepared.Value().Publish(); !published) {
        return FailAsset<void>(failure, AssetDiagnosticCode::None, published.Error());
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
    TextCodecOptions codec = CodecOptions(set_, resolver);
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
