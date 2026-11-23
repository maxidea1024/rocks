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

// 새 임포트다(Docs/Design/AssetSystem 7.1절 18 A2f1b).
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

// 역할 목록(roleAt(i), i < count)을 임포터 역할 규칙(7.1절 18·19 A2f3b)으로 판정하고 원본 순서를
// order에 둔다: 고정 역할(roles — 비면 DefaultImportSourceRole)은 정확히 한 번씩 roles 순, 그 밖은
// 접두(rolePrefixes) 하나로 시작하고 접두 뒤가 비지 않으며 서로 다른 역할을 byte 순으로. 위반이면
// bad에 그 번호(빠진 고정 역할이면 count + 그 고정 역할 번호)를 두고 거짓이다. 오류: 할당 실패.
template <class RoleAt>
Result<bool> OrderImportRoles(Allocator& allocator, const AssetImporterInfo& info, usize count,
                              RoleAt roleAt, Array<usize>& order, usize& bad) noexcept {
    order.Clear();
    const StringView defaultRole = DefaultImportSourceRole;
    const Span<const StringView> fixed =
        info.roles.IsEmpty() ? Span<const StringView>(&defaultRole, 1) : info.roles;
    if (count > MaxImportSources) {
        bad = MaxImportSources;
        return Result<bool>::Success(false);
    }

    if (auto reserved = order.TryReserve(count); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < fixed.Size(); ++index) {
        usize found = IndexNone;
        for (usize at = 0; at < count; ++at) {
            if (roleAt(at) == fixed[index]) {
                if (found != IndexNone) {
                    bad = at;
                    return Result<bool>::Success(false);
                }

                found = at;
            }
        }

        if (found == IndexNone) {
            bad = count + index;
            return Result<bool>::Success(false);
        }

        (void)order.TryPushBack(found);
    }

    // 고정 역할 밖은 접두 역할이다 — 접두 판정 뒤 byte 순으로 넣는다(같은 역할 둘은 위반).
    const usize fixedCount = order.Size();
    for (usize at = 0; at < count; ++at) {
        const StringView role = roleAt(at);
        bool isFixed = false;
        for (StringView one : fixed) {
            isFixed = isFixed || role == one;
        }

        if (isFixed) {
            continue;
        }

        bool prefixed = false;
        for (StringView prefix : info.rolePrefixes) {
            prefixed = prefixed || (role.ByteCount() > prefix.ByteCount() &&
                                    role.StartsWith(prefix, StringComparison::CaseSensitive));
        }

        usize position = fixedCount;
        while (position < order.Size() &&
               StringView::CompareBytes(roleAt(order[position]), role) < 0) {
            ++position;
        }

        if (!prefixed || (position < order.Size() && roleAt(order[position]) == role)) {
            bad = at;
            return Result<bool>::Success(false);
        }

        (void)order.TryInsert(at, position);
    }

    return Result<bool>::Success(true);
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
    // 이번에 만든 패키지인지(거짓이면 재임포트의 기존 출력 — 실패해도 지우지 않는다)와 임포터가
    // 이번에 청했는지다(청하지 않은 기존 출력은 고아 경고).
    bool created = true;
    bool used = true;
};

// 묶음 구성원의 주 출력 하나다(A2f3d — OpenAsset이 색인보다 먼저 본다).
struct ImportGroupMember {
    AssetId id{};
    ObjectHandle asset{};
};

// 서비스가 임포터에 주는 문맥이다: 새 출력은 저장하지 않은 새 패키지, 재임포트는 기존 출력을
// 미리 등록해 같은 key 요청에 그 대표 에셋을 준다(타입 정확 일치 — 7.1절 18 A2f2).
class ImportContext final : public AssetImportContext {
public:
    ImportContext(Allocator& allocator, EditorPackageSet& set, TypeRegistry& types,
                  const AssetIndex& index, const AssetAliasTable& aliases, StringView path,
                  const AssetImporterInfo& info, Span<const AssetImportSource> sources,
                  const void* options, AssetId mainId, Array<EditorWarning>* warnings,
                  bool reimport, Span<const ImportTag> previousTags) noexcept
        : allocator_(allocator), set_(set), types_(types), index_(index), aliases_(aliases),
          path_(path), info_(info), sources_(sources), options_(options), mainId_(mainId),
          warnings_(warnings), reimport_(reimport), previousTags_(previousTags),
          access_(set.Access()), outputs_(allocator), tags_(allocator) {
    }

    // 재임포트의 기존 출력 하나를 등록한다(아직 청하지 않음). 오류: 할당 실패.
    Result<void> AddExisting(StringView key, StringView path, AssetId id, TypeId type,
                             ObjectHandle asset) noexcept {
        MadeOutput made(allocator_);
        Result<void> step = made.key.TryAssign(key);
        if (step) {
            step = made.path.TryAssign(path);
        }

        if (step) {
            made.id = id;
            made.type = type;
            made.asset = asset;
            made.created = false;
            made.used = false;
            step = outputs_.TryPushBack(rocks::Move(made));
        }

        return step;
    }

    Span<const AssetImportSource> Sources() const noexcept override {
        return sources_;
    }

    const void* Options() const noexcept override {
        return options_;
    }

    bool IsReimport() const noexcept override {
        return reimport_;
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
        if ((main && type != info_.outputType) || (!main && !IsOutputKey(key))) {
            return invalid(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        }

        // 같은 key: 청하지 않은 기존 출력이면 그것(타입 정확 일치), 이미 청했으면 두 번째 요청.
        for (MadeOutput& made : outputs_) {
            if (made.key.View() != key) {
                continue;
            }

            if (made.used) {
                return invalid(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
            }

            if (made.type != type) {
                return invalid(AssetDiagnosticCode::TypeMismatch, ErrorCode::InvalidArgument,
                               made.id);
            }

            made.used = true;
            return Out::Success(made.asset);
        }

        if (outputs_.Size() >= MaxImportOutputs) {
            return invalid(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
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

    Result<void> AddTag(StringView key, StringView value) noexcept override {
        if (!IsValidImportTag(info_.name, key, value)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (const ImportTag& tag : tags_) {
            if (tag.key.View() == key && tag.value.View() == value) {
                return Result<void>::Success();
            }
        }

        if (tags_.Size() >= MaxImportTags) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        ImportTag tag(allocator_);
        Result<void> step = tag.key.TryAssign(key);
        if (step) {
            step = tag.value.TryAssign(value);
        }

        return step ? tags_.TryPushBack(rocks::Move(tag)) : step;
    }

    Span<const ImportTag> PreviousTags() const noexcept override {
        return previousTags_;
    }

    Result<ObjectHandle> OpenAsset(AssetId asset) noexcept override {
        using Out = Result<ObjectHandle>;
        auto refuse = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
            failure_ = AssetDiagnostic{};
            failure_.code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
            failure_.error = error;
            failure_.asset = mainId_;
            failure_.related = asset;
            return Out::Failure(error);
        };
        // 자기 출력은 Output으로 받는다.
        bool own = asset.IsNull() || asset == mainId_;
        for (const MadeOutput& made : outputs_) {
            own = own || made.id == asset;
        }

        if (own) {
            return refuse(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
        }

        // 묶음 구성원 주 출력(A2f3d)을 먼저 본다 — 새 구성원은 색인에 없다.
        for (const ImportGroupMember& member : members_) {
            if (member.id == asset) {
                return Out::Success(member.asset);
            }
        }

        auto found = index_.FindById(asset);
        if (!found) {
            return refuse(found.Error() == ErrorCode::NotFound
                              ? AssetDiagnosticCode::MissingDependency
                              : AssetDiagnosticCode::DuplicateAssetId,
                          found.Error());
        }

        AssetDiagnostic diagnostic;
        auto opened = set_.Open(asset, warnings_, &diagnostic);
        if (!opened) {
            failure_ = diagnostic;
            return Out::Failure(opened.Error());
        }

        // Open은 패키지를 준다 — 대표 에셋(이름 빈 자식 — 패키지에 늘 있다)을 준다.
        auto representative = set_.Registry().FindChild(access_, opened.Value(), "");
        return representative ? Out::Success(representative.Value())
                              : refuse(AssetDiagnosticCode::None, representative.Error());
    }

    // 묶음 구성원 주 출력 표다(A2f3d — 단일 임포트는 비어 있다).
    void SetGroupMembers(Span<const ImportGroupMember> members) noexcept {
        members_ = members;
    }

    Span<const MadeOutput> Outputs() const noexcept {
        return outputs_.View();
    }

    // 이번에 더한 임포터 태그다(더한 순서 — 기록 쓰기가 정렬한다).
    Span<const ImportTag> Tags() const noexcept {
        return tags_.View();
    }

    // 마지막 실패 진단이다(Output·Fail이 남긴 것 — 없으면 code None).
    const AssetDiagnostic& LastFailure() const noexcept {
        return failure_;
    }

private:
    Allocator& allocator_;
    EditorPackageSet& set_;
    TypeRegistry& types_;
    const AssetIndex& index_;
    const AssetAliasTable& aliases_;
    StringView path_;
    const AssetImporterInfo& info_;
    Span<const AssetImportSource> sources_;
    const void* options_;
    AssetId mainId_;
    Array<EditorWarning>* warnings_;
    bool reimport_;
    Span<const ImportTag> previousTags_;
    Span<const ImportGroupMember> members_{};
    ObjectAccessContext access_;
    Array<MadeOutput> outputs_;
    Array<ImportTag> tags_;
    AssetDiagnostic failure_;
};

// 기록 옵션 Record(schema = 옵션 struct, 버전 ≤ 지금 버전, 필드 1 = JSON)를 target에 대입한다.
// 오류: schema·버전·형식 위반(Record 판정의 오류 — 호출자는 InvalidImportOption), JSON 문법
// InvalidArgument, 대입 진단 InvalidArgument(diagnostics), 할당 실패.
Result<void> ApplyRecordedOptions(Allocator& allocator, const AssignmentTarget& target,
                                  const TypeLease& lease, Span<const uint8> record,
                                  const TextCodecOptions& codec,
                                  TextDiagnostics& diagnostics) noexcept {
    auto reader = RecordReader::Open(allocator, record, lease.Id(), 1, lease.Schema().version);
    if (!reader) {
        return Result<void>::Failure(reader.Error());
    }

    // 필드 1이 없거나(NotFound) wire가 String이 아니면 형식 위반이다(InvalidArgument).
    auto field = reader.Value().Find(ImportOptionsTextField);
    if (!field && field.Error() != ErrorCode::NotFound) {
        return Result<void>::Failure(field.Error());
    }

    if (!field || field.Value().wireType != static_cast<uint32>(WireType::String)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    JsonDocument document(allocator);
    const Span<const uint8> payload = field.Value().payload;
    const JsonError parsed =
        document.Parse(StringView(reinterpret_cast<const char*>(payload.Data()), payload.Size()));
    if (!parsed.Succeeded()) {
        return Result<void>::Failure(parsed.code == JsonErrorCode::OutOfMemory
                                         ? ErrorCode::OutOfMemory
                                         : ErrorCode::InvalidArgument);
    }

    auto prepared = reflection_text::ImportJson(document.Root(), target, codec, diagnostics);
    if (!prepared) {
        return Result<void>::Failure(prepared.Error());
    }

    auto published = prepared.Value().Publish();
    return published ? Result<void>::Success() : Result<void>::Failure(published.Error());
}

} // namespace

// 새 임포트·재임포트가 같이 쓰는 실행의 입력이다(원본은 임포터 roles 순 절대 경로).
struct AssetService::ImportRun {
    // 재임포트의 기존 출력 하나(주 출력은 빈 key)다.
    struct Existing {
        explicit Existing(Allocator& allocator) noexcept : key(allocator), path(allocator) {
        }

        Existing(Existing&&) noexcept = default;
        Existing& operator=(Existing&&) noexcept = default;

        String key;
        String path;
        AssetId id{};
        TypeId type{};
        ObjectHandle asset{};
    };

    explicit ImportRun(Allocator& allocator) noexcept : existing(allocator) {
    }

    const AssetDatabaseLock* lock = nullptr;
    const LogicalPath* path = nullptr;
    AssetId mainId{};
    AssetImporter* importer = nullptr;
    Span<const AssetImportSourceInput> sources{};
    const AssetSourceRoots* roots = nullptr;
    const AssetAliasTable* aliases = nullptr;
    Span<const AssetId> before{};
    Span<const TextAssignment> assignments{};
    TextDiagnostics* diagnostics = nullptr;
    AssetImportResult* result = nullptr;
    Array<EditorWarning>* warnings = nullptr;
    AssetDiagnostic* failure = nullptr;
    bool reimport = false;
    Span<const uint8> recordedOptions{};
    // 재임포트의 옛 소유 기록 태그다(문맥 PreviousTags — A2f3a).
    Span<const ImportTag> previousTags{};
    Array<Existing> existing;
};

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
    // 역할: 고정 역할 정확히 한 번씩 + 접두 역할(A2f3b), 문맥은 고정 → 접두(byte 순).
    Array<usize> order(allocator);
    usize badRole = 0;
    auto roleCheck = OrderImportRoles(
        allocator, info, sources.Size(), [&](usize at) noexcept { return sources[at].role; }, order,
        badRole);
    if (!roleCheck) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, roleCheck.Error());
    }

    if (!roleCheck.Value()) {
        return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                ErrorCode::InvalidArgument, {}, badRole);
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

    // 원본을 판정 순서(고정 → 접두)로 둔다.
    Array<AssetImportSourceInput> ordered(allocator);
    if (auto reserved = ordered.TryReserve(order.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index : order) {
        (void)ordered.TryPushBack(sources[index]);
    }

    // 지금 열린 id다(실패하면 이번에 연 것만 닫는다).
    Array<AssetId> before(allocator);
    if (auto reserved = before.TryReserve(set_.Count()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        (void)before.TryPushBack(set_.At(index).id);
    }

    ImportRun run(allocator);
    run.lock = &lock;
    run.path = &parsed.Value();
    run.mainId = mainId.Value();
    run.importer = chosen;
    run.sources = ordered.View();
    run.roots = &roots;
    run.aliases = aliases.Value();
    run.before = before.View();
    run.assignments = assignments;
    run.diagnostics = &diagnostics;
    run.result = &result;
    run.warnings = warnings;
    run.failure = failure;
    return RunImport(run);
}

Result<void> AssetService::RunImport(ImportRun& run) noexcept {
    Allocator& allocator = *allocator_;
    const AssetDatabaseLock& lock = *run.lock;
    TextDiagnostics& diagnostics = *run.diagnostics;
    AssetImportResult& result = *run.result;
    Array<EditorWarning>* warnings = run.warnings;
    AssetDiagnostic* failure = run.failure;
    const Span<const TextAssignment> assignments = run.assignments;
    AssetImporter* chosen = run.importer;
    const AssetImporterInfo& info = chosen->Info();
    Array<ReadSource> read(allocator);
    Array<AssetImportSource> contextSources(allocator);
    if (auto reserved = read.TryReserve(run.sources.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    if (auto reserved = contextSources.TryReserve(run.sources.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    uint64 remaining = MaxImportTotalSourceBytes;
    // 이번 임포트의 경고(절대 경로·임포터 경고)는 따로 모아 성공할 때만 넘긴다.
    Array<EditorWarning> pending(allocator);
    for (usize index = 0; index < run.sources.Size(); ++index) {
        const AssetImportSourceInput& input = run.sources[index];
        ReadSource source(allocator);
        Result<void> step = source.role.TryAssign(run.sources[index].role);
        if (step) {
            step = source.path.TryAssign(input.path);
        }

        if (!step) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error());
        }

        if (auto located = run.roots->Locate(input.path, source.location); !located) {
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
            warning.diagnostic.asset = run.mainId;
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
        // 재임포트: 기록 옵션(JSON)을 먼저 대입한다(명시 대입만 바꾼다 — 16.1절).
        if (!run.recordedOptions.IsEmpty()) {
            if (auto applied = ApplyRecordedOptions(allocator, target, options.Lease(),
                                                    run.recordedOptions, codec, diagnostics);
                !applied) {
                return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                        applied.Error());
            }
        }

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
    } else if (!run.recordedOptions.IsEmpty()) {
        // 옵션 없는 임포터에 옵션 기록이 있다(임포터 버전이 옵션을 뺐다 — 다시 임포트한다).
        return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                ErrorCode::InvalidArgument);
    }

    // 임포터 호출(실패하면 이번에 연 것을 닫는다 — 열린 id는 호출자가 적었다).
    ImportContext context(allocator, set_, *types_, index_, *run.aliases, run.path->Text(), info,
                          contextSources.View(), options.Get(), run.mainId,
                          warnings != nullptr ? &pending : nullptr, run.reimport, run.previousTags);
    auto abandon = [&](const AssetDiagnostic& diagnostic) noexcept {
        (void)set_.CloseOpened(run.before);
        if (failure != nullptr) {
            *failure = diagnostic;
        }

        return Result<void>::Failure(diagnostic.error);
    };
    auto abandonWith = [&](AssetDiagnosticCode code, ErrorCode error) noexcept {
        AssetDiagnostic diagnostic;
        diagnostic.code = error == ErrorCode::OutOfMemory ? AssetDiagnosticCode::None : code;
        diagnostic.error = error;
        diagnostic.asset = run.mainId;
        return abandon(diagnostic);
    };
    for (const ImportRun::Existing& existing : run.existing) {
        if (auto added = context.AddExisting(existing.key.View(), existing.path.View(), existing.id,
                                             existing.type, existing.asset);
            !added) {
            return abandonWith(AssetDiagnosticCode::None, added.Error());
        }
    }

    if (auto imported = chosen->Import(context); !imported) {
        AssetDiagnostic diagnostic = context.LastFailure();
        if (diagnostic.error != imported.Error()) {
            diagnostic = AssetDiagnostic{};
            diagnostic.error = imported.Error();
            diagnostic.asset = run.mainId;
        }

        return abandon(diagnostic);
    }

    const MadeOutput* main = nullptr;
    for (const MadeOutput& made : context.Outputs()) {
        main = made.key.IsEmpty() && made.used ? &made : main;
    }

    if (main == nullptr) {
        return abandonWith(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 재임포트: 기록에 있는데 이번에 청하지 않은 기존 출력은 고아다(그 패키지는 그대로 — 16.1절).
    if (warnings != nullptr) {
        for (const MadeOutput& made : context.Outputs()) {
            if (made.used) {
                continue;
            }

            EditorWarning warning;
            warning.kind = EditorWarningKind::OrphanedOutput;
            warning.diagnostic.code = AssetDiagnosticCode::OrphanedOutput;
            warning.diagnostic.asset = made.id;
            warning.diagnostic.related = run.mainId;
            if (auto pushed = pending.TryPushBack(warning); !pushed) {
                return abandonWith(AssetDiagnosticCode::None, pushed.Error());
            }
        }
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
    Array<AssetId> savedIds(allocator);
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
        step = savedIds.TryReserve(context.Outputs().Size());
    }

    if (step) {
        step = result.path.TryAssign(main->path.View());
    }

    if (step) {
        step = result.outputs.TryReserve(context.Outputs().Size());
    }

    for (const MadeOutput& made : context.Outputs()) {
        if (!step || made.key.IsEmpty() || !made.used) {
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
        if (made.used) {
            (void)recordOutputs.TryPushBack(ImportOutputInput{made.key.View(), made.id, made.type});
        }
    }

    // 기존 파일(재임포트의 기존 출력·주 출력)을 하나라도 교체했는지다. 교체한 기존 파일은 새 출력을
    // Hard로 가리킬 수 있으므로 그 뒤의 실패는 새 파일을 지우지 않는다(7.1절 19 A2f3d — 디스크에는
    // 참조가 닫힌 상태만 남는다, 교차 검증 필수 2).
    bool existingSaved = false;
    // 저장 실패의 되돌리기: 만든 패키지를 닫고, 기존 파일을 교체하기 전이면 이번에 쓴 새 출력
    // 파일을 지운다(못 지우면 경고).
    auto rollback = [&](const AssetDiagnostic& diagnostic) noexcept {
        (void)set_.CloseOpened(run.before);
        for (usize index = 0; index < savedFiles.Size() && !existingSaved; ++index) {
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
                warning.diagnostic.asset = savedIds[index];
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
    // 저장 순서(7.1절 19 A2f3d): 새 출력 → 기존 출력(재임포트) → 주 출력(소유 기록 — 출력 목록이
    // 디스크에 없는 출력을 적지 않는다). 새 파일을 먼저 써서 교체한 기존 파일이 없는 새 파일을
    // 가리키는 일이 없게 한다.
    const EditorSaveOptions save{};
    constexpr int SavePasses = 2;
    for (int pass = 0; pass < SavePasses; ++pass) {
        for (const MadeOutput& made : context.Outputs()) {
            if (made.key.IsEmpty() || !made.used || made.created != (pass == 0)) {
                continue;
            }

            ImportRecordInput reference;
            reference.owner = run.mainId;
            AssetDiagnostic diagnostic;
            auto record = WriteImportRecord(allocator, made.id, reference);
            Result<void> saved =
                record ? set_.SetImportRecord(made.id, record.Value().View(), run.mainId)
                       : Result<void>::Failure(record.Error());
            // 저장 뒤 파일 이름을 담을 자리를 먼저 잡는다(저장 뒤 복사가 실패하면 그 파일을
            // 되돌리기 목록에 넣지 못해 고아로 남는다). 기존 출력(재임포트)은 실패해도 지우지
            // 않으므로 이름을 담지 않는다.
            String file(allocator);
            if (saved && made.created) {
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

            if (made.created) {
                (void)file.TryAssign(set_.Find(made.id)->fileName.View());
                (void)savedFiles.TryPushBack(rocks::Move(file));
                (void)savedIds.TryPushBack(made.id);
            } else {
                existingSaved = true;
            }
        }
    }

    // 임포터 태그(A2f3a)를 기록 입력으로 옮긴다(자리는 여기서 잡는다 — 실패하면 되돌린다).
    Array<ImportTagInput> recordTags(allocator);
    if (auto reserved = recordTags.TryReserve(context.Tags().Size()); !reserved) {
        AssetDiagnostic diagnostic;
        diagnostic.error = reserved.Error();
        diagnostic.asset = run.mainId;
        return rollback(diagnostic);
    }

    for (const ImportTag& tag : context.Tags()) {
        (void)recordTags.TryPushBack(ImportTagInput{tag.key.View(), tag.value.View()});
    }

    ImportRecordInput owned;
    owned.owner = run.mainId;
    owned.tags = recordTags.View();
    owned.importerId = info.id;
    owned.importerName = info.name;
    owned.importerVersion = info.version;
    owned.sources = recordSources.View();
    owned.options = optionsRecord.View();
    owned.outputs = recordOutputs.View();
    AssetDiagnostic diagnostic;
    auto record = WriteImportRecord(allocator, run.mainId, owned);
    Result<void> saved = record ? set_.SetImportRecord(run.mainId, record.Value().View(), AssetId{})
                                : Result<void>::Failure(record.Error());
    if (saved) {
        saved = reserveWarnings();
    }

    if (!saved) {
        diagnostic.error = saved.Error();
        diagnostic.asset = run.mainId;
        return rollback(diagnostic);
    }

    if (saved = set_.Save(run.mainId, lock, save, warnings, &diagnostic); !saved) {
        return rollback(diagnostic);
    }

    // 이번 임포트의 경고를 넘긴다(자리는 저장 전에 잡았다).
    if (warnings != nullptr) {
        for (const EditorWarning& warning : pending) {
            (void)warnings->TryPushBack(warning);
        }
    }

    // 이번에 연 패키지(출력·그 의존)를 닫는다 — 저장한 깨끗한 패키지이고 원래 열려 있던 패키지는
    // 그것들을 참조할 수 없다(열기는 기존 패키지의 미해결 import를 다시 묶지 않는다). 열린 채 두면
    // 같은 서비스의 다음 재임포트가 Busy다(7.1절 18 A2f2a). 닫기 실패(할당·Discard)는 성공을 바꾸지
    // 않는다 — 저장은 끝났고, 남은 패키지는 열린 채다(호출자가 Close한다).
    (void)set_.CloseOpened(run.before);
    result.id = run.mainId;
    return Result<void>::Success();
}

Result<void> AssetService::Reimport(const AssetDatabaseLock& lock, StringView path,
                                    Span<const AssetImportSourceInput> overrides,
                                    Span<const TextAssignment> assignments,
                                    TextDiagnostics& diagnostics, AssetImportResult& result,
                                    Array<EditorWarning>* warnings, AssetDiagnostic* failure,
                                    bool replaceSources) noexcept {
    Allocator& allocator = *allocator_;
    result.id = AssetId{};
    result.path.Clear();
    result.outputs.Clear();
    if (!initialized_ || !lock.IsHeldFor(*layout_)) {
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

    auto entry = index_.FindByPath(parsed.Value());
    if (!entry) {
        return FailImport<void>(failure,
                                entry.Error() == ErrorCode::AlreadyExists
                                    ? AssetDiagnosticCode::DuplicatePath
                                    : AssetDiagnosticCode::None,
                                entry.Error());
    }

    // 주 출력: 같은 id 하나·읽기 전용 아님·소유 기록·열려 있지 않음.
    const AssetIndexEntry& main = *entry.Value();
    const AssetId mainId = main.id;
    if (main.duplicateId) {
        return FailImport<void>(failure, AssetDiagnosticCode::DuplicateAssetId,
                                ErrorCode::AlreadyExists, mainId);
    }

    if (main.readOnly) {
        return FailImport<void>(failure, AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported,
                                mainId);
    }

    if (!main.importRecord.present || main.importRecord.owner != mainId) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->error = ErrorCode::InvalidArgument;
            failure->asset = mainId;
            failure->related = main.importRecord.present ? main.importRecord.owner : AssetId{};
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (set_.Find(mainId) != nullptr) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::Busy, mainId);
    }

    AssetImporter* importer =
        importers_ != nullptr ? importers_->FindById(main.importRecord.importerId) : nullptr;
    if (importer == nullptr) {
        return FailImport<void>(failure, AssetDiagnosticCode::UnsupportedSourceFormat,
                                ErrorCode::Unsupported, mainId);
    }

    if (main.importRecord.importerVersion > importer->Info().version) {
        return FailImport<void>(failure, AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported,
                                mainId);
    }

    // 기록 출력(주 출력 말고): 색인에 없으면(지워짐) 새 출력으로, 있으면 같은 id 하나·읽기 전용
    // 아님·이 owner의 참조 기록·열려 있지 않음.
    for (AssetId output : main.importRecord.outputs) {
        if (output == mainId) {
            continue;
        }

        auto found = index_.FindById(output);
        if (!found && found.Error() == ErrorCode::NotFound) {
            continue;
        }

        if (!found) {
            return FailImport<void>(failure, AssetDiagnosticCode::DuplicateAssetId, found.Error(),
                                    output);
        }

        const AssetIndexEntry& other = *found.Value();
        if (other.readOnly) {
            return FailImport<void>(failure, AssetDiagnosticCode::NewerVersion,
                                    ErrorCode::Unsupported, output);
        }

        if (!other.importRecord.present || other.importRecord.owner != mainId) {
            if (failure != nullptr) {
                *failure = AssetDiagnostic{};
                failure->error = ErrorCode::InvalidArgument;
                failure->asset = output;
                failure->related = mainId;
            }

            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (set_.Find(output) != nullptr) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::Busy, output);
        }
    }

    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    AssetSourceRoots roots(allocator, *layout_);
    if (auto loaded = roots.Load(*files_, failure); !loaded) {
        return loaded;
    }

    // 지금 열린 id(실패하면 이번에 연 것 — 주 출력·출력과 그 의존 — 을 닫는다).
    Array<AssetId> before(allocator);
    if (auto reserved = before.TryReserve(set_.Count()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        (void)before.TryPushBack(set_.At(index).id);
    }

    auto close = [&](AssetDiagnosticCode code, ErrorCode error, AssetId asset,
                     uint64 detail = 0) noexcept {
        (void)set_.CloseOpened(before.View());
        return FailImport<void>(failure, code, error, asset, detail);
    };
    // 주 출력을 열어 온전한 기록을 읽는다.
    if (auto opened = set_.Open(mainId, warnings, failure); !opened) {
        (void)set_.CloseOpened(before.View());
        return Result<void>::Failure(opened.Error());
    }

    const EditorPackageInfo* mainInfo = set_.Find(mainId);
    // 미해결 import가 있으면 저장할 수 없다(asset-set과 같다 — 복제·ID 수정과 같은 규칙).
    if (!mainInfo->unresolved.IsEmpty()) {
        // 닫기 전에 대상을 꺼낸다(닫으면 정보가 사라진다).
        const AssetId target = mainInfo->unresolved[0].target;
        (void)set_.CloseOpened(before.View());
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = AssetDiagnosticCode::MissingDependency;
            failure->error = ErrorCode::InvalidArgument;
            failure->asset = mainId;
            failure->related = target;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto record = ReadImportRecord(allocator, mainId, mainInfo->importRecord.View());
    if (!record) {
        return close(AssetDiagnosticCode::None, record.Error(), mainId);
    }

    const ImportRecord& recorded = record.Value();
    // 판정은 색인 요약으로 했다 — 파일 기록이 그 요약과 다르면(색인 뒤 밖에서 바뀜) 판정하지 않은
    // 출력을 열 수 있으므로 거부한다(Open의 다른 id와 같은 ExternalModification / StaleHandle).
    // owner는 따로 보지 않는다 — 소유 기록이 아니게 바뀌면 임포터 id가 비어 다르다.
    bool same = recorded.importerId == main.importRecord.importerId &&
                recorded.importerVersion == main.importRecord.importerVersion &&
                recorded.outputs.Size() == main.importRecord.outputs.Size();
    for (usize index = 0; same && index < recorded.outputs.Size(); ++index) {
        same = recorded.outputs[index].asset == main.importRecord.outputs[index];
    }

    if (!same) {
        return close(AssetDiagnosticCode::ExternalModification, ErrorCode::StaleHandle, mainId);
    }

    // 원본(7.1절 18·19 A2f3b): 기록 역할은 임포터 역할 규칙에 맞아야 한다(임포터 버전이 역할을
    // 바꿨으면 InvalidImportOption — 다시 임포트한다). replaceSources면 overrides가 새 원본 전체다
    // (새 임포트와 같은 역할 규칙 — 기록 원본은 버린다), 아니면 기록 원본을 루트로 풀고 overrides가
    // 같은 역할만 바꾼다. 문맥 순서는 고정 역할 → 접두 역할(byte 순)이다.
    const AssetImporterInfo& info = importer->Info();
    Array<usize> order(allocator);
    usize badRole = 0;
    Array<String> resolved(allocator);
    Array<AssetImportSourceInput> ordered(allocator);
    const bool single = overrides.Size() == 1 && overrides[0].role.IsEmpty();
    if (replaceSources) {
        // 역할 없는 원본 하나(`--source=`)는 유일한 고정 역할이다(고정 역할이 하나일 때만).
        const StringView defaultRole = DefaultImportSourceRole;
        const StringView onlyRole = info.roles.IsEmpty()     ? defaultRole
                                    : info.roles.Size() == 1 ? info.roles[0]
                                                             : StringView();
        if (overrides.IsEmpty() || (single && onlyRole.IsEmpty())) {
            return close(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                         mainId);
        }

        const auto roleAt = [&](usize at) noexcept {
            return single ? onlyRole : overrides[at].role;
        };
        auto checked = OrderImportRoles(allocator, info, overrides.Size(), roleAt, order, badRole);
        if (!checked) {
            return close(AssetDiagnosticCode::None, checked.Error(), mainId);
        }

        if (!checked.Value()) {
            return close(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                         mainId, badRole);
        }

        if (auto reserved = ordered.TryReserve(order.Size()); !reserved) {
            return close(AssetDiagnosticCode::None, reserved.Error(), mainId);
        }

        for (usize index : order) {
            (void)ordered.TryPushBack(AssetImportSourceInput{roleAt(index), overrides[index].path});
        }
    } else {
        auto checked = OrderImportRoles(
            allocator, info, recorded.sources.Size(),
            [&](usize at) noexcept { return recorded.sources[at].role.View(); }, order, badRole);
        if (!checked) {
            return close(AssetDiagnosticCode::None, checked.Error(), mainId);
        }

        if (!checked.Value() || (single && recorded.sources.Size() != 1)) {
            return close(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                         mainId);
        }

        for (usize index = 0; index < overrides.Size() && !single; ++index) {
            bool known = false;
            for (const ImportSource& source : recorded.sources) {
                known = known || source.role.View() == overrides[index].role;
            }

            if (!known) {
                return close(AssetDiagnosticCode::InvalidImportOption, ErrorCode::InvalidArgument,
                             mainId, index);
            }
        }

        if (auto reserved = resolved.TryReserve(order.Size()); !reserved) {
            return close(AssetDiagnosticCode::None, reserved.Error(), mainId);
        }

        if (auto reserved = ordered.TryReserve(order.Size()); !reserved) {
            return close(AssetDiagnosticCode::None, reserved.Error(), mainId);
        }

        for (usize index : order) {
            const ImportSource& source = recorded.sources[index];
            StringView chosen{};
            for (const AssetImportSourceInput& override : overrides) {
                chosen = single || override.role == source.role.View() ? override.path : chosen;
            }

            if (chosen.IsEmpty()) {
                String path(allocator);
                if (auto unpacked =
                        roots.Resolve(source.root.View(), source.relativePath.View(), path);
                    !unpacked) {
                    return close(unpacked.Error() == ErrorCode::NotFound
                                     ? AssetDiagnosticCode::MissingSource
                                     : AssetDiagnosticCode::InvalidSource,
                                 unpacked.Error(), mainId, index);
                }

                (void)resolved.TryPushBack(rocks::Move(path));
                chosen = resolved[resolved.Size() - 1].View();
            }

            (void)ordered.TryPushBack(AssetImportSourceInput{source.role.View(), chosen});
        }
    }

    // 기존 출력을 연다(주 출력은 빈 key — 기록 key·타입, 경로는 지금 색인 경로).
    ImportRun run(allocator);
    // 옛 소유 기록의 태그를 임포터에 보인다(record는 RunImport가 끝날 때까지 산다 — A2f3a).
    run.previousTags = recorded.tags.View();
    if (auto reserved = run.existing.TryReserve(recorded.outputs.Size()); !reserved) {
        return close(AssetDiagnosticCode::None, reserved.Error(), mainId);
    }

    for (const ImportOutput& output : recorded.outputs) {
        auto found = output.asset == mainId ? Result<const AssetIndexEntry*>::Success(&main)
                                            : index_.FindById(output.asset);
        if (!found) {
            continue;
        }

        // 기록 타입은 그 패키지의 대표 타입이어야 한다(손 편집·손상 기록 — 임포터가 Output(key,
        // T)로 받은 객체가 T가 아니면 안 된다).
        if (found.Value()->type != output.type) {
            return close(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch, output.asset);
        }

        AssetDiagnostic diagnostic;
        auto opened = set_.Open(output.asset, warnings, &diagnostic);
        if (!opened) {
            (void)set_.CloseOpened(before.View());
            if (failure != nullptr) {
                *failure = diagnostic;
            }

            return Result<void>::Failure(opened.Error());
        }

        ImportRun::Existing existing(allocator);
        Result<void> step = existing.key.TryAssign(output.outputKey.View());
        if (step) {
            step = existing.path.TryAssign(found.Value()->path.View());
        }

        if (!step) {
            return close(AssetDiagnosticCode::None, step.Error(), mainId);
        }

        // Open은 패키지를 준다 — 임포터에는 대표 에셋(이름 빈 자식 — 패키지에 늘 있다)을 준다.
        auto representative = set_.Registry().FindChild(set_.Access(), opened.Value(), "");

        existing.id = output.asset;
        existing.type = output.type;
        existing.asset = representative.Value();
        (void)run.existing.TryPushBack(rocks::Move(existing));
    }

    // 이 owner를 가리키는 참조 기록의 고아 출력(`<주 경로>_<key>` — 앞선 임포트가 기존 파일을
    // 교체한 뒤 실패해 남긴 새 출력, 7.1절 19 A2f3d)도 기존 출력으로 더한다: 임포터가 같은 key를
    // 청하면 그 패키지를 다시 쓰고(아니면 OrphanedOutput 경고), 같은 경로 점유(DuplicatePath)로
    // 다시 실행이 막히지 않는다(수렴한다).
    const StringView mainPath = main.path.View();
    for (usize at = 0; at < index_.Count(); ++at) {
        const AssetIndexEntry& entry = index_.At(at);
        bool listed = !entry.importRecord.present || !(entry.importRecord.owner == mainId) ||
                      entry.id == mainId || entry.duplicateId;
        for (const ImportOutput& output : recorded.outputs) {
            listed = listed || output.asset == entry.id;
        }

        const StringView path = entry.path.View();
        if (listed || path.ByteCount() <= mainPath.ByteCount() + 1 ||
            !path.StartsWith(mainPath, StringComparison::CaseSensitive) ||
            path.Data()[mainPath.ByteCount()] != '_') {
            continue;
        }

        const StringView key =
            path.SliceBytes(mainPath.ByteCount() + 1, path.ByteCount() - mainPath.ByteCount() - 1);
        if (!IsOutputKey(key)) {
            continue;
        }

        // 열려 있으면 Busy(기록 출력과 같다), 읽기 전용이면 NewerVersion.
        if (set_.Find(entry.id) != nullptr) {
            return close(AssetDiagnosticCode::None, ErrorCode::Busy, entry.id);
        }

        if (entry.readOnly) {
            return close(AssetDiagnosticCode::NewerVersion, ErrorCode::Unsupported, entry.id);
        }

        AssetDiagnostic diagnostic;
        auto opened = set_.Open(entry.id, warnings, &diagnostic);
        if (!opened) {
            (void)set_.CloseOpened(before.View());
            if (failure != nullptr) {
                *failure = diagnostic;
            }

            return Result<void>::Failure(opened.Error());
        }

        ImportRun::Existing adopted(allocator);
        Result<void> step = adopted.key.TryAssign(key);
        if (step) {
            step = adopted.path.TryAssign(path);
        }

        if (step) {
            adopted.id = entry.id;
            adopted.type = entry.type;
            adopted.asset = set_.Registry().FindChild(set_.Access(), opened.Value(), "").Value();
            step = run.existing.TryPushBack(rocks::Move(adopted));
        }

        if (!step) {
            return close(AssetDiagnosticCode::None, step.Error(), mainId);
        }
    }

    run.lock = &lock;
    run.path = &parsed.Value();
    run.mainId = mainId;
    run.importer = importer;
    run.sources = ordered.View();
    run.roots = &roots;
    run.aliases = aliases.Value();
    run.before = before.View();
    run.assignments = assignments;
    run.diagnostics = &diagnostics;
    run.result = &result;
    run.warnings = warnings;
    run.failure = failure;
    run.reimport = true;
    run.recordedOptions = recorded.options.View();
    // 실행 앞부분(원본·옵션)의 실패도 이번에 연 주 출력·출력을 닫는다.
    auto ran = RunImport(run);
    if (!ran) {
        (void)set_.CloseOpened(before.View());
    }

    return ran;
}

} // namespace rocks
