#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/FileError.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <Serialization/Record.h>

// 새 임포트다(Docs/Design/AssetSystem.ko.md 7.1절 18 A2f1b).
namespace rocks {

namespace {

using reflection_text::AssignmentTarget;
using reflection_text::TextAssignment;
using reflection_text::TextCodecOptions;
using reflection_text::TextDiagnostics;

// 옵션 Record(schema = 옵션 struct id, version = 그 버전)의 필드다: 옵션 값의 정규 JSON 텍스트
// (리플렉션 JSON 표기 — 재임포트는 기본값 struct에 다시 대입한다, 7.1절 18).
constexpr Guid ImportOptionsTextField{0, 1};
// 옵션 JSON 텍스트의 처음 용량이다(StringBuilder가 늘린다).
constexpr usize OptionsJsonBytes = 512;

template <class T>
Result<T> FailImport(AssetDiagnostic* failure, AssetDiagnosticCode code, ErrorCode error,
                     AssetId asset = {}, uint64 detail = 0) noexcept {
    if (failure != nullptr) {
        *failure = AssetDiagnostic{};
        failure->code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        failure->error = error;
        failure->asset = asset;
        failure->detail = detail;
    }

    return Result<T>::Failure(error);
}

// 옵션 struct 값 하나다(타입 lease의 함수표로 만들고 소멸한다 — lease는 임포터 표가 쥔다).
class OptionsValue {
public:
    explicit OptionsValue(Allocator& allocator) noexcept : allocator_(&allocator) {
    }

    OptionsValue(const OptionsValue&) = delete;
    OptionsValue& operator=(const OptionsValue&) = delete;

    ~OptionsValue() {
        if (memory_ != nullptr) {
            lease_->StructOps().destroy(memory_);
            (void)allocator_->Deallocate(memory_);
        }
    }

    Result<void> Make(const TypeLease& lease) noexcept {
        const RuntimeStructOps& ops = lease.StructOps();
        auto memory = allocator_->Allocate(ops.size, ops.alignment);
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        ops.construct(memory.Value(), *allocator_);
        lease_ = &lease;
        memory_ = memory.Value();
        return Result<void>::Success();
    }

    void* Get() const noexcept {
        return memory_;
    }

    const TypeLease& Lease() const noexcept {
        return *lease_;
    }

private:
    Allocator* allocator_;
    const TypeLease* lease_ = nullptr;
    void* memory_ = nullptr;
};

// 대입 경로의 첫 이름이다(`.`·`[` 앞).
StringView FirstName(StringView path) noexcept {
    for (usize index = 0; index < path.ByteCount(); ++index) {
        const char c = path.Data()[index];
        if (c == '.' || c == '[') {
            return path.SliceBytes(0, index);
        }
    }

    return path;
}

// 출력 key 규칙이다(ImportRecord outputKey — 1–MaxImportOutputKeyBytes byte의 `[A-Za-z0-9_]`).
bool IsOutputKey(StringView key) noexcept {
    if (key.IsEmpty() || key.ByteCount() > MaxImportOutputKeyBytes) {
        return false;
    }

    for (usize index = 0; index < key.ByteCount(); ++index) {
        const char c = key.Data()[index];
        const bool word =
            (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        if (!word) {
            return false;
        }
    }

    return true;
}

// 읽은 원본 하나다.
struct ReadSource {
    explicit ReadSource(Allocator& allocator) noexcept
        : role(allocator), path(allocator), location(allocator), bytes(allocator) {
    }

    ReadSource(ReadSource&&) noexcept = default;
    ReadSource& operator=(ReadSource&&) noexcept = default;

    String role;
    String path;
    AssetSourceLocation location;
    Array<uint8> bytes;
    Sha256Digest digest{};
};

// 만든 출력 하나다.
struct MadeOutput {
    explicit MadeOutput(Allocator& allocator) noexcept : key(allocator), path(allocator) {
    }

    MadeOutput(MadeOutput&&) noexcept = default;
    MadeOutput& operator=(MadeOutput&&) noexcept = default;

    String key;
    String path;
    AssetId id{};
    TypeId type{};
    ObjectHandle asset{};
};

// 서비스가 임포터에 주는 문맥이다(새 임포트 — 출력은 저장하지 않은 새 패키지).
class NewImportContext final : public AssetImportContext {
public:
    NewImportContext(Allocator& allocator, EditorPackageSet& set, TypeRegistry& types,
                     const AssetAliasTable& aliases, StringView path, const AssetImporterInfo& info,
                     Span<const AssetImportSource> sources, const void* options, AssetId mainId,
                     Array<EditorWarning>* warnings) noexcept
        : allocator_(allocator), set_(set), types_(types), aliases_(aliases), path_(path),
          info_(info), sources_(sources), options_(options), mainId_(mainId), warnings_(warnings),
          access_(set.Access()), outputs_(allocator) {
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return sources_;
    }

    const void* Options() const noexcept override {
        return options_;
    }

    bool IsReimport() const noexcept override {
        return false;
    }

    ObjectRegistry& Registry() noexcept override {
        return set_.Registry();
    }

    const ObjectAccessContext& Access() const noexcept override {
        return access_;
    }

    Result<ObjectHandle> Output(StringView key, TypeId type) noexcept override {
        using Out = Result<ObjectHandle>;
        auto invalid = [&](AssetDiagnosticCode code, ErrorCode error,
                           AssetId related = {}) noexcept {
            failure_ = AssetDiagnostic{};
            failure_.code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
            failure_.error = error;
            failure_.asset = mainId_;
            failure_.related = related;
            return Out::Failure(error);
        };
        const bool main = key.IsEmpty();
        if ((main && type != info_.outputType) || (!main && !IsOutputKey(key)) ||
            outputs_.Size() >= MaxImportOutputs) {
            return invalid(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        }

        for (const MadeOutput& made : outputs_) {
            if (made.key.View() == key) {
                return invalid(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
            }
        }

        auto lease = types_.Acquire(type);
        if (!lease || !lease.Value().IsA(GeneratedType<Asset>::Id()) ||
            HasFlag(lease.Value().Schema().flags, TypeFlags::Abstract)) {
            return invalid(AssetDiagnosticCode::TypeMismatch,
                           !lease && lease.Error() != ErrorCode::TypeMismatch
                               ? lease.Error()
                               : ErrorCode::InvalidArgument);
        }

        MadeOutput made(allocator_);
        Result<void> step = made.key.TryAssign(key);
        if (step) {
            step = made.path.TryAssign(path_);
        }

        if (step && !main) {
            step = made.path.TryAppend("_");
            if (step) {
                step = made.path.TryAppend(key);
            }
        }

        if (!step) {
            return invalid(AssetDiagnosticCode::None, step.Error());
        }

        auto parsed = ParseLogicalPath(allocator_, made.path.View());
        if (!parsed) {
            return invalid(parsed.Error() == ErrorCode::InvalidArgument
                               ? AssetDiagnosticCode::InvalidPath
                               : AssetDiagnosticCode::None,
                           parsed.Error());
        }

        // 별칭이 있는 경로는 점유된 것이다(New와 같다).
        if (const usize alias = aliases_.Find(parsed.Value().Key()); alias != AssetAliasNone) {
            return invalid(AssetDiagnosticCode::DuplicatePath, ErrorCode::AlreadyExists,
                           aliases_.At(alias).asset);
        }

        made.id = mainId_;
        if (!main) {
            auto fresh = AssetId::TryNew();
            if (!fresh) {
                return invalid(AssetDiagnosticCode::None, fresh.Error());
            }

            made.id = fresh.Value();
        }

        if (auto reserved = outputs_.TryReserve(outputs_.Size() + 1); !reserved) {
            return invalid(AssetDiagnosticCode::None, reserved.Error());
        }

        AssetDiagnostic diagnostic;
        auto created = set_.Create(parsed.Value(), type, made.id, &diagnostic);
        if (!created) {
            failure_ = diagnostic;
            return Out::Failure(created.Error());
        }

        // Create는 패키지를 준다 — 임포터에는 그 대표 에셋(이름 빈 자식)을 준다. 방금 만든 패키지라
        // 대표 에셋이 있다.
        auto representative = set_.Registry().FindChild(access_, created.Value(), "");
        made.type = type;
        made.asset = representative.Value();
        (void)outputs_.TryPushBack(rocks::Move(made));
        return Out::Success(representative.Value());
    }

    Result<BulkDataRef> AddBulk(ObjectHandle output, Array<uint8>&& raw) noexcept override {
        for (const MadeOutput& made : outputs_) {
            if (made.asset == output) {
                return set_.Find(made.id)->store->AddUnsaved(rocks::Move(raw));
            }
        }

        return Result<BulkDataRef>::Failure(ErrorCode::InvalidArgument);
    }

    Result<void> Warn(AssetDiagnosticCode code, uint64 detail) noexcept override {
        if (!AssetDiagnosticIsWarning(code)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (warnings_ == nullptr) {
            return Result<void>::Success();
        }

        EditorWarning warning;
        warning.kind = EditorWarningKind::ImportWarning;
        warning.diagnostic.code = code;
        warning.diagnostic.asset = mainId_;
        warning.diagnostic.detail = detail;
        return warnings_->TryPushBack(warning);
    }

    Result<void> Fail(AssetDiagnosticCode code, ErrorCode error, uint64 detail) noexcept override {
        failure_ = AssetDiagnostic{};
        failure_.code = code;
        failure_.error = error;
        failure_.asset = mainId_;
        failure_.detail = detail;
        return Result<void>::Failure(error);
    }

    Span<const MadeOutput> Outputs() const noexcept {
        return outputs_.View();
    }

    // 마지막 실패 진단이다(Output·Fail이 남긴 것 — 없으면 code None).
    const AssetDiagnostic& LastFailure() const noexcept {
        return failure_;
    }

private:
    Allocator& allocator_;
    EditorPackageSet& set_;
    TypeRegistry& types_;
    const AssetAliasTable& aliases_;
    StringView path_;
    const AssetImporterInfo& info_;
    Span<const AssetImportSource> sources_;
    const void* options_;
    AssetId mainId_;
    Array<EditorWarning>* warnings_;
    ObjectAccessContext access_;
    Array<MadeOutput> outputs_;
    AssetDiagnostic failure_;
};

} // namespace

Result<void> AssetService::Import(const AssetDatabaseLock& lock, StringView path,
                                  Span<const AssetImportSourceInput> sources, StringView importer,
                                  Span<const TextAssignment> assignments,
                                  TextDiagnostics& diagnostics, AssetImportResult& result,
                                  Array<EditorWarning>* warnings,
                                  AssetDiagnostic* failure) noexcept {
    Allocator& allocator = *allocator_;
    result.id = AssetId{};
    result.path.Clear();
    result.outputs.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_) || sources.IsEmpty()) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(allocator, path);
    if (!parsed) {
        return FailImport<void>(failure,
                                parsed.Error() == ErrorCode::InvalidArgument
                                    ? AssetDiagnosticCode::InvalidPath
                                    : AssetDiagnosticCode::None,
                                parsed.Error());
    }

    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    // 주 경로의 별칭·색인·저장하지 않은 새 패키지 점유를 원본을 읽기 전에 본다(큰 원본을 헛되이
    // 읽지 않게 — 디스크에만 있는 파일은 Output의 Create가 본다).
    if (const usize alias = aliases.Value()->Find(parsed.Value().Key()); alias != AssetAliasNone) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = AssetDiagnosticCode::DuplicatePath;
            failure->error = ErrorCode::AlreadyExists;
            failure->related = aliases.Value()->At(alias).asset;
        }

        return Result<void>::Failure(ErrorCode::AlreadyExists);
    }

    if (auto indexed = index_.FindByPath(parsed.Value());
        indexed || indexed.Error() == ErrorCode::AlreadyExists) {
        return FailImport<void>(failure, AssetDiagnosticCode::DuplicatePath,
                                ErrorCode::AlreadyExists);
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        const EditorPackageInfo& open = set_.At(index);
        if (!open.unsaved) {
            continue;
        }

        auto other = ParseLogicalPath(allocator, open.logicalPath.View());
        if (!other) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, other.Error());
        }

        if (other.Value().Key() == parsed.Value().Key()) {
            return FailImport<void>(failure, AssetDiagnosticCode::DuplicatePath,
                                    ErrorCode::AlreadyExists, {}, 0);
        }
    }

    // 임포터: 이름 또는 원본 확장자의 기본(원본이 여럿이면 모두 같아야 한다).
    AssetImporter* chosen = nullptr;
    if (importers_ == nullptr) {
        return FailImport<void>(failure, AssetDiagnosticCode::UnsupportedSourceFormat,
                                ErrorCode::Unsupported);
    }

    if (!importer.IsEmpty()) {
        chosen = importers_->FindByName(importer);
        if (chosen == nullptr) {
            return FailImport<void>(failure, AssetDiagnosticCode::UnsupportedSourceFormat,
                                    ErrorCode::NotFound);
        }
    } else {
        for (usize index = 0; index < sources.Size(); ++index) {
            auto extension = Path::Extension(sources[index].path);
            AssetImporter* found =
                extension ? importers_->FindByExtension(extension.Value()) : nullptr;
            if (found == nullptr || (chosen != nullptr && found != chosen)) {
                return FailImport<void>(failure, AssetDiagnosticCode::UnsupportedSourceFormat,
                                        ErrorCode::Unsupported, {}, index);
            }

            chosen = found;
        }
    }

    const AssetImporterInfo& info = chosen->Info();
    // 역할: 임포터 roles와 정확히 같은 집합(원본 하나면 DefaultImportSourceRole), 문맥은 roles 순.
    const StringView defaultRoles[] = {DefaultImportSourceRole};
    const Span<const StringView> roles =
        info.roles.IsEmpty() ? Span<const StringView>(defaultRoles) : info.roles;
    if (sources.Size() != roles.Size()) {
        return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                ErrorCode::InvalidArgument);
    }

    Array<usize> order(allocator);
    if (auto reserved = order.TryReserve(roles.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < roles.Size(); ++index) {
        usize found = IndexNone;
        for (usize at = 0; at < sources.Size(); ++at) {
            if (sources[at].role == roles[index]) {
                found = found == IndexNone ? at : found;
            }
        }

        if (found == IndexNone) {
            return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                    ErrorCode::InvalidArgument, {}, index);
        }

        (void)order.TryPushBack(found);
    }

    // 원본 위치·읽기(합계 예산).
    auto mainId = AssetId::TryNew();
    if (!mainId) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, mainId.Error());
    }

    AssetSourceRoots roots(allocator, *layout_);
    if (auto loaded = roots.Load(*files_, failure); !loaded) {
        return loaded;
    }

    Array<ReadSource> read(allocator);
    Array<AssetImportSource> contextSources(allocator);
    if (auto reserved = read.TryReserve(roles.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    if (auto reserved = contextSources.TryReserve(roles.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    uint64 remaining = MaxImportTotalSourceBytes;
    // 이번 임포트의 경고(절대 경로·임포터 경고)는 따로 모아 성공할 때만 넘긴다.
    Array<EditorWarning> pending(allocator);
    for (usize index = 0; index < roles.Size(); ++index) {
        const AssetImportSourceInput& input = sources[order[index]];
        ReadSource source(allocator);
        Result<void> step = source.role.TryAssign(roles[index]);
        if (step) {
            step = source.path.TryAssign(input.path);
        }

        if (!step) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error());
        }

        if (auto located = roots.Locate(input.path, source.location); !located) {
            return FailImport<void>(failure, AssetDiagnosticCode::InvalidSource, located.Error(),
                                    {}, index);
        }

        const uint64 limit = remaining < MaxImportSourceBytes ? remaining : MaxImportSourceBytes;
        if (auto loaded =
                ReadImportSource(*files_, input.path, source.bytes, source.digest, limit, failure);
            !loaded) {
            if (failure != nullptr) {
                failure->detail = index;
            }

            return loaded;
        }

        remaining -= source.bytes.Size();
        if (source.location.absolute && warnings != nullptr) {
            EditorWarning warning;
            warning.kind = EditorWarningKind::ImportWarning;
            warning.diagnostic.code = AssetDiagnosticCode::AbsoluteSourcePath;
            warning.diagnostic.asset = mainId.Value();
            warning.diagnostic.detail = index;
            if (auto pushed = pending.TryPushBack(warning); !pushed) {
                return FailImport<void>(failure, AssetDiagnosticCode::None, pushed.Error());
            }
        }

        (void)read.TryPushBack(rocks::Move(source));
    }

    for (const ReadSource& source : read) {
        (void)contextSources.TryPushBack(
            AssetImportSource{source.role.View(), source.path.View(), source.bytes.View()});
    }

    // 옵션과 설정을 첫 이름으로 나눈다(등록이 이름 충돌을 막았다 — 타입 lease는 표가 쥔다).
    const TypeLease& output = *importers_->OutputType(*chosen);
    OptionsValue options(allocator);
    if (const TypeLease* optionsType = importers_->OptionsType(*chosen); optionsType != nullptr) {
        if (auto made = options.Make(*optionsType); !made) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, made.Error());
        }
    }

    Array<TextAssignment> optionAssignments(allocator);
    Array<TextAssignment> settingAssignments(allocator);
    if (auto reserved = optionAssignments.TryReserve(assignments.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    if (auto reserved = settingAssignments.TryReserve(assignments.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (const TextAssignment& assignment : assignments) {
        const StringView name = FirstName(assignment.path);
        PropertyPathBuffer probe;
        if (options.Get() != nullptr && ParsePropertyPath(options.Lease(), name, probe)) {
            (void)optionAssignments.TryPushBack(assignment);
        } else if (ParsePropertyPath(output, name, probe)) {
            (void)settingAssignments.TryPushBack(assignment);
        } else {
            return FailImport<void>(failure, AssetDiagnosticCode::UnknownProperty,
                                    ErrorCode::InvalidArgument, {}, assignment.sourceIndex);
        }
    }

    AssetTextResolver resolver(allocator, set_, index_, warnings);
    resolver.SetAliases(&aliasLookup_);
    const TextCodecOptions codec = asset_database_detail::CodecOptions(set_, resolver);
    Array<uint8> optionsRecord(allocator);
    if (options.Get() != nullptr) {
        const AssignmentTarget target = AssignmentTarget::Value(
            set_.Registry(), set_.Access(), options.Lease(), options.Get(), allocator);
        if (!optionAssignments.IsEmpty()) {
            auto prepared = reflection_text::PrepareAssignments(target, optionAssignments.View(),
                                                                codec, diagnostics);
            if (!prepared) {
                return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                        prepared.Error());
            }

            if (auto published = prepared.Value().Publish(); !published) {
                return FailImport<void>(failure, AssetDiagnosticCode::None, published.Error());
            }
        }

        StringBuilder<OptionsJsonBytes> text(allocator);
        JsonWriter writer(text);
        Result<void> written = reflection_text::ExportJson(target, writer, codec);
        if (written) {
            written = writer.Finish();
        }

        if (!written) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, written.Error());
        }

        RecordBuilder record(allocator, options.Lease().Id(), options.Lease().Schema().version);
        if (auto added = record.AddField(
                ImportOptionsTextField, WireType::String,
                Span<const uint8>(reinterpret_cast<const uint8*>(text.View().Data()),
                                  text.View().ByteCount()));
            !added) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, added.Error());
        }

        auto built = record.Build();
        if (!built) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, built.Error());
        }

        optionsRecord = rocks::Move(built.Value());
    }

    // 임포터 호출(지금 열린 id를 적어 두고 실패하면 이번에 연 것을 닫는다).
    Array<AssetId> before(allocator);
    if (auto reserved = before.TryReserve(set_.Count()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        (void)before.TryPushBack(set_.At(index).id);
    }

    NewImportContext context(allocator, set_, *types_, *aliases.Value(), parsed.Value().Text(),
                             info, contextSources.View(), options.Get(), mainId.Value(),
                             warnings != nullptr ? &pending : nullptr);
    auto abandon = [&](const AssetDiagnostic& diagnostic) noexcept {
        (void)set_.CloseOpened(before.View());
        if (failure != nullptr) {
            *failure = diagnostic;
        }

        return Result<void>::Failure(diagnostic.error);
    };
    auto abandonWith = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        AssetDiagnostic diagnostic;
        diagnostic.code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        diagnostic.error = error;
        diagnostic.asset = mainId.Value();
        return abandon(diagnostic);
    };
    if (auto imported = chosen->Import(context); !imported) {
        AssetDiagnostic diagnostic = context.LastFailure();
        if (diagnostic.error != imported.Error()) {
            diagnostic = AssetDiagnostic{};
            diagnostic.error = imported.Error();
            diagnostic.asset = mainId.Value();
        }

        return abandon(diagnostic);
    }

    const MadeOutput* main = nullptr;
    for (const MadeOutput& made : context.Outputs()) {
        main = made.key.IsEmpty() ? &made : main;
    }

    if (main == nullptr) {
        return abandonWith(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 설정(주 출력의 저장 속성).
    if (!settingAssignments.IsEmpty()) {
        auto prepared = reflection_text::PrepareAssignments(
            AssignmentTarget::Live(set_.Registry(), set_.Access(), main->asset),
            settingAssignments.View(), codec, diagnostics);
        if (!prepared) {
            return abandonWith(AssetDiagnosticCode::None, prepared.Error());
        }

        if (auto published = prepared.Value().Publish(); !published) {
            return abandonWith(AssetDiagnosticCode::None, published.Error());
        }
    }

    // 결과·기록 입력을 먼저 만든다(저장 뒤에 할당이 실패하지 않게).
    Array<ImportSourceInput> recordSources(allocator);
    Array<ImportOutputInput> recordOutputs(allocator);
    Array<String> savedFiles(allocator);
    Result<void> step = recordSources.TryReserve(read.Size());
    if (step) {
        step = recordOutputs.TryReserve(context.Outputs().Size());
    }

    if (step) {
        step = savedFiles.TryReserve(context.Outputs().Size());
    }

    // 되돌리기의 OS 경로 버퍼와 색인 갱신 이름 표(되돌리기 안에서 할당하지 않는다).
    String target(allocator);
    Array<StringView> savedNames(allocator);
    if (step) {
        step =
            target.TryReserve(layout_->ContentDirectory().ByteCount() + 1 + MaxNativePathBytes());
    }

    if (step) {
        step = savedNames.TryReserve(context.Outputs().Size());
    }

    if (step) {
        step = result.path.TryAssign(main->path.View());
    }

    if (step) {
        step = result.outputs.TryReserve(context.Outputs().Size());
    }

    for (const MadeOutput& made : context.Outputs()) {
        if (!step || made.key.IsEmpty()) {
            continue;
        }

        AssetImportOutput entry(allocator);
        step = entry.key.TryAssign(made.key.View());
        if (step) {
            step = entry.path.TryAssign(made.path.View());
        }

        entry.id = made.id;
        if (step) {
            (void)result.outputs.TryPushBack(rocks::Move(entry));
        }
    }

    if (!step) {
        result.path.Clear();
        result.outputs.Clear();
        return abandonWith(AssetDiagnosticCode::None, step.Error());
    }

    for (const ReadSource& source : read) {
        (void)recordSources.TryPushBack(
            ImportSourceInput{source.role.View(), source.location.root.View(),
                              source.location.relative.View(), source.digest, source.bytes.Size()});
    }

    for (const MadeOutput& made : context.Outputs()) {
        (void)recordOutputs.TryPushBack(ImportOutputInput{made.key.View(), made.id, made.type});
    }

    // 저장 실패의 되돌리기: 만든 패키지를 닫고 이번에 쓴 출력 파일을 지운다(못 지우면 경고).
    auto rollback = [&](const AssetDiagnostic& diagnostic) noexcept {
        (void)set_.CloseOpened(before.View());
        for (usize index = 0; index < savedFiles.Size(); ++index) {
            target.Clear();
            (void)target.TryAppend(layout_->ContentDirectory());
            (void)target.TryAppend("/");
            (void)target.TryAppend(savedFiles[index].View());
            // 경고 자리는 저장마다 미리 잡았다(실패하지 않는다).
            if (const platform::FileStatus deleted =
                    files_->Delete(target.View().Data(), target.View().ByteCount());
                !deleted && warnings != nullptr) {
                EditorWarning warning;
                warning.kind = EditorWarningKind::OrphanNotDeleted;
                warning.diagnostic.error = FileErrorCode(deleted);
                warning.diagnostic.asset = result.outputs[index].id;
                (void)warnings->TryPushBack(warning);
            }

            (void)savedNames.TryPushBack(savedFiles[index].View());
        }

        // 색인에서 지운 파일을 뺀다. 갱신이 실패하면(할당·읽기) 이 서비스의 색인은 다음
        // 갱신·검색까지 지운 파일 항목을 남긴다(같은 경로 임포트는 DuplicatePath — 7.1절 18).
        AssetIndexRefreshPaths paths;
        paths.files = savedNames.View();
        (void)index_.RefreshPaths(*files_, paths, read_);

        result.path.Clear();
        result.outputs.Clear();
        if (failure != nullptr) {
            *failure = diagnostic;
        }

        return Result<void>::Failure(diagnostic.error);
    };
    // 저장 직전마다 저장 경고(NotDurable·고아 임시 파일 — 새 패키지는 미해결 import가 없다)와
    // 되돌리기 경고(출력마다 하나) 자리를 잡는다.
    constexpr usize SaveWarnings = 2;
    auto reserveWarnings = [&]() noexcept {
        return warnings == nullptr ? Result<void>::Success()
                                   : warnings->TryReserve(warnings->Size() + SaveWarnings +
                                                          savedFiles.Capacity() + pending.Size());
    };
    // 다른 출력 먼저(참조 기록 owner = 주 출력), 주 출력 마지막(소유 기록).
    const EditorSaveOptions save{};
    for (const MadeOutput& made : context.Outputs()) {
        if (made.key.IsEmpty()) {
            continue;
        }

        ImportRecordInput reference;
        reference.owner = mainId.Value();
        AssetDiagnostic diagnostic;
        auto record = WriteImportRecord(allocator, made.id, reference);
        Result<void> saved =
            record ? set_.SetImportRecord(made.id, record.Value().View(), mainId.Value())
                   : Result<void>::Failure(record.Error());
        // 저장 뒤 파일 이름을 담을 자리를 먼저 잡는다(저장 뒤 복사가 실패하면 그 파일을 되돌리기
        // 목록에 넣지 못해 고아로 남는다).
        String file(allocator);
        if (saved) {
            saved = file.TryReserve(MaxNativePathBytes());
        }

        if (saved) {
            saved = reserveWarnings();
        }

        if (!saved) {
            diagnostic = AssetDiagnostic{};
            diagnostic.error = saved.Error();
            diagnostic.asset = made.id;
            return rollback(diagnostic);
        }

        if (saved = set_.Save(made.id, lock, save, warnings, &diagnostic); !saved) {
            return rollback(diagnostic);
        }

        (void)file.TryAssign(set_.Find(made.id)->fileName.View());
        (void)savedFiles.TryPushBack(rocks::Move(file));
    }

    ImportRecordInput owned;
    owned.owner = mainId.Value();
    owned.importerId = info.id;
    owned.importerName = info.name;
    owned.importerVersion = info.version;
    owned.sources = recordSources.View();
    owned.options = optionsRecord.View();
    owned.outputs = recordOutputs.View();
    AssetDiagnostic diagnostic;
    auto record = WriteImportRecord(allocator, mainId.Value(), owned);
    Result<void> saved =
        record ? set_.SetImportRecord(mainId.Value(), record.Value().View(), AssetId{})
               : Result<void>::Failure(record.Error());
    if (saved) {
        saved = reserveWarnings();
    }

    if (!saved) {
        diagnostic.error = saved.Error();
        diagnostic.asset = mainId.Value();
        return rollback(diagnostic);
    }

    if (saved = set_.Save(mainId.Value(), lock, save, warnings, &diagnostic); !saved) {
        return rollback(diagnostic);
    }

    // 이번 임포트의 경고를 넘긴다(자리는 저장 전에 잡았다).
    if (warnings != nullptr) {
        for (const EditorWarning& warning : pending) {
            (void)warnings->TryPushBack(warning);
        }
    }

    result.id = mainId.Value();
    return Result<void>::Success();
}

} // namespace rocks
