#include "AssetDatabaseInternal.h"

#include <AssetDatabase/AssetService.h>
#include <AssetDatabase/AssetSourceRoots.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/FilePackageBulkStore.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Algorithms.h>
#include <Core/FileError.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Optional.h>
#include <Core/Text/Json.h>
#include <Core/Text/Path.h>
#include <Core/Text/StringBuilder.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextAssignments.h>
#include <ReflectionText/TextJson.h>
#include <Serialization/Record.h>

// 새 임포트·재임포트·묶음 임포트다(Docs/Design/AssetSystem 7.1절 18 A2f1b·A2f2a, 7.1절 19 A2f3d).
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
// order에 둔다: 고정 역할(roles — 비면 DefaultImportSourceRole, 가변만 받는 임포터는 없음)은
// 정확히 한 번씩 roles 순, 그 밖은 접두(rolePrefixes) 하나로 시작하고 접두 뒤가 비지 않으며 서로
// 다른 역할을 byte 순으로. 가변만 받는 임포터는 원본이 하나 이상이다(0개면 bad = 0). 위반이면
// bad에 그 번호(빠진 고정 역할이면 count + 그 고정 역할 번호)를 두고 거짓이다. 오류: 할당 실패.
template <class RoleAt>
Result<bool> OrderImportRoles(Allocator& allocator, const AssetImporterInfo& info, usize count,
                              RoleAt roleAt, Array<usize>& order, usize& bad) noexcept {
    order.Clear();
    const StringView defaultRole = DefaultImportSourceRole;
    const Span<const StringView> fixed = info.variableSourcesOnly ? Span<const StringView>()
                                         : info.roles.IsEmpty()
                                             ? Span<const StringView>(&defaultRole, 1)
                                             : info.roles;
    if (count > MaxImportSources) {
        bad = MaxImportSources;
        return Result<bool>::Success(false);
    }

    if (info.variableSourcesOnly && count == 0) {
        bad = 0;
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

using asset_database_detail::ImportGroupMember;

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

    // 기존 출력 하나를 등록한다(아직 청하지 않음 — 재임포트의 기록·고아 출력, 또는 묶음이 미리
    // 만든 새 주 출력(created — 실패하면 버리고 저장은 새 파일 무리다)). 오류: 할당 실패.
    Result<void> AddExisting(StringView key, StringView path, AssetId id, TypeId type,
                             ObjectHandle asset, bool created) noexcept {
        MadeOutput made(allocator_);
        Result<void> step = made.key.TryAssign(key);
        if (step) {
            step = made.path.TryAssign(path);
        }

        if (step) {
            made.id = id;
            made.type = type;
            made.asset = asset;
            made.created = created;
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

    usize GroupSize() const noexcept override {
        return members_.Size();
    }

    usize GroupIndex() const noexcept override {
        return groupIndex_;
    }

    AssetId MemberAsset(usize index) const noexcept override {
        return index < members_.Size() ? members_[index].id : AssetId{};
    }

    // 묶음 구성원 주 출력 표와 이 구성원 번호다(A2f3d — 단일 임포트는 자기 하나).
    void SetGroup(Span<const ImportGroupMember> members, usize index) noexcept {
        members_ = members;
        groupIndex_ = index;
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
    usize groupIndex_ = 0;
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

// 묶음 구성원 하나의 실행 상태다(7.1절 19 A2f3d — 단일 Import·Reimport는 구성원 하나짜리 묶음):
// 판정(PrepareImport·PrepareReimport)이 경로·임포터·원본·기존 출력을 채우고, 주 출력 단계가 새
// 주 출력을 만들고, StageImport가 원본을 읽어 임포터를 부르고 결과·기록 입력을 만들며, 저장
// 단계가 무리별로 저장한다. 문맥이 이 상태의 칸을 가리키므로 옮기지 않는다(UniquePtr로 둔다).
struct AssetService::ImportRun {
    // 기존 출력 하나(주 출력은 빈 key)다: 재임포트의 기록·고아 출력, 또는 주 출력 단계가 미리
    // 만든 새 주 출력(created).
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
        bool created = false;
    };

    ImportRun(Allocator& allocator, const ProjectLayout& layout) noexcept
        : roots(allocator, layout), ordered(allocator), resolved(allocator), recorded(allocator),
          existing(allocator), read(allocator), contextSources(allocator), options(allocator),
          optionsRecord(allocator), pending(allocator), recordSources(allocator),
          recordOutputs(allocator), recordTags(allocator), ownedRecord(allocator) {
    }

    ImportRun(const ImportRun&) = delete;
    ImportRun& operator=(const ImportRun&) = delete;

    // 입력(호출자 소유).
    const AssetImportGroupMember* member = nullptr;
    AssetImportResult* result = nullptr;
    // 판정이 채운다.
    Optional<LogicalPath> path;
    AssetId mainId{};
    AssetImporter* importer = nullptr;
    AssetSourceRoots roots;
    const AssetAliasTable* aliases = nullptr;
    // 원본(판정 순서 — 고정 역할 → 접두 역할)과 그 경로 문자열(재임포트가 기록에서 푼 것).
    Array<AssetImportSourceInput> ordered;
    Array<String> resolved;
    // 재임포트의 옛 소유 기록(옵션·태그를 임포터 실행이 끝날 때까지 가리킨다).
    ImportRecord recorded;
    Span<const uint8> recordedOptions{};
    Span<const ImportTag> previousTags{};
    Array<Existing> existing;
    // StageImport가 채운다.
    Array<ReadSource> read;
    Array<AssetImportSource> contextSources;
    OptionsValue options;
    Array<uint8> optionsRecord;
    // 이번 구성원의 경고(절대 경로·임포터 경고·고아 출력 — 성공할 때만 넘긴다).
    Array<EditorWarning> pending;
    UniquePtr<ImportContext> context;
    Array<ImportSourceInput> recordSources;
    Array<ImportOutputInput> recordOutputs;
    Array<ImportTagInput> recordTags;
    // 주 출력의 소유 기록 byte다(저장 전에 만든다 — 저장 사이에 할당이 실패하지 않게).
    Array<uint8> ownedRecord;
    // 주 출력까지 저장했는지다(기존 파일 교체 뒤의 실패에서 결과를 남긴다).
    bool mainSaved = false;
};

Result<void> AssetService::Import(const AssetDatabaseLock& lock, StringView path,
                                  Span<const AssetImportSourceInput> sources, StringView importer,
                                  Span<const TextAssignment> assignments,
                                  TextDiagnostics& diagnostics, AssetImportResult& result,
                                  Array<EditorWarning>* warnings,
                                  AssetDiagnostic* failure) noexcept {
    AssetImportGroupMember member;
    member.path = path;
    member.sources = sources;
    member.importer = importer;
    member.assignments = assignments;
    return RunImportGroup(lock, Span<const AssetImportGroupMember>(&member, 1), diagnostics,
                          Span<AssetImportResult>(&result, 1), warnings, failure, false);
}

Result<void> AssetService::Reimport(const AssetDatabaseLock& lock, StringView path,
                                    Span<const AssetImportSourceInput> overrides,
                                    Span<const TextAssignment> assignments,
                                    TextDiagnostics& diagnostics, AssetImportResult& result,
                                    Array<EditorWarning>* warnings, AssetDiagnostic* failure,
                                    bool replaceSources) noexcept {
    AssetImportGroupMember member;
    member.path = path;
    member.sources = overrides;
    member.assignments = assignments;
    member.reimport = true;
    member.replaceSources = replaceSources;
    return RunImportGroup(lock, Span<const AssetImportGroupMember>(&member, 1), diagnostics,
                          Span<AssetImportResult>(&result, 1), warnings, failure, false);
}

Result<void>
AssetService::ImportGroup(const AssetDatabaseLock& lock, Span<const AssetImportGroupMember> members,
                          TextDiagnostics& diagnostics, Span<AssetImportResult> results,
                          Array<EditorWarning>* warnings, AssetDiagnostic* failure) noexcept {
    return RunImportGroup(lock, members, diagnostics, results, warnings, failure, true);
}

Result<void> AssetService::RunImportGroup(const AssetDatabaseLock& lock,
                                          Span<const AssetImportGroupMember> members,
                                          TextDiagnostics& diagnostics,
                                          Span<AssetImportResult> results,
                                          Array<EditorWarning>* warnings, AssetDiagnostic* failure,
                                          bool group) noexcept {
    Allocator& allocator = *allocator_;
    auto clearResults = [&]() noexcept {
        for (AssetImportResult& result : results) {
            result.id = AssetId{};
            result.path.Clear();
            result.outputs.Clear();
        }
    };
    clearResults();
    if (!initialized_ || !lock.IsHeldFor(*layout_) || members.IsEmpty() ||
        members.Size() > MaxImportGroupMembers || results.Size() != members.Size()) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    // 실패한 구성원 번호다. 묶음(공개 ImportGroup)의 실패 진단은 detail = 그 번호, asset이 비면 그
    // 구성원의 주 출력이다(단일 Import·Reimport는 진단을 그대로 둔다 — detail은 원본 번호 등).
    usize current = 0;
    Array<UniquePtr<ImportRun>> runs(allocator);
    auto mark = [&](ErrorCode error) noexcept {
        if (group && failure != nullptr) {
            failure->detail = current;
            if (failure->asset.IsNull() && current < runs.Size()) {
                failure->asset = runs[current]->mainId;
            }
        }

        return Result<void>::Failure(error);
    };
    if (auto reserved = runs.TryReserve(members.Size()); !reserved) {
        return mark(FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error()).Error());
    }

    // 지금 열린 id(id 순)다: 실패하면 이번에 연 것만 닫고, 재임포트의 Busy 판정도 이것으로 한다
    // (앞 구성원이 의존으로 연 다른 구성원은 Busy가 아니다 — Hard 순환).
    Array<AssetId> before(allocator);
    if (auto reserved = before.TryReserve(set_.Count()); !reserved) {
        return mark(FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error()).Error());
    }

    for (usize index = 0; index < set_.Count(); ++index) {
        (void)before.TryPushBack(set_.At(index).id);
    }

    auto abandon = [&](const Result<void>& failed) noexcept {
        (void)set_.CloseOpened(before.View());
        clearResults();
        return mark(failed.Error());
    };

    // 판정(하나라도 실패하면 아무것도 쓰지 않는다).
    for (current = 0; current < members.Size(); ++current) {
        auto made = UniquePtr<ImportRun>::TryMake(allocator, allocator, *layout_);
        if (!made) {
            return abandon(FailImport<void>(failure, AssetDiagnosticCode::None, made.Error()));
        }

        ImportRun& run = *made.Value();
        run.member = &members[current];
        run.result = &results[current];
        (void)runs.TryPushBack(rocks::Move(made.Value()));
        auto prepared = members[current].reimport
                            ? PrepareReimport(run, before.View(), warnings, failure)
                            : PrepareImport(run, failure);
        if (!prepared) {
            return abandon(prepared);
        }
    }

    // 같은 경로 둘(경로 키 — 새 구성원끼리는 색인이 못 본다).
    for (current = 1; current < runs.Size(); ++current) {
        for (usize other = 0; other < current; ++other) {
            if (runs[other]->path->Key() == runs[current]->path->Key()) {
                return abandon(FailImport<void>(failure, AssetDiagnosticCode::None,
                                                ErrorCode::InvalidArgument, runs[current]->mainId));
            }
        }
    }

    // 새 구성원의 주 출력을 먼저 만든다(임포터가 MemberAsset·OpenAsset으로 다른 구성원을 Hard로
    // 건다 — 그 뒤 다른 구성원의 추가 출력이 이 경로를 청하면 DuplicatePath다).
    Array<ImportGroupMember> table(allocator);
    if (auto reserved = table.TryReserve(runs.Size()); !reserved) {
        current = 0;
        return abandon(FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error()));
    }

    for (current = 0; current < runs.Size(); ++current) {
        ImportRun& run = *runs[current];
        if (!run.member->reimport) {
            const AssetImporterInfo& info = run.importer->Info();
            auto lease = types_->Acquire(info.outputType);
            if (!lease || !lease.Value().IsA(GeneratedType<Asset>::Id()) ||
                HasFlag(lease.Value().Schema().flags, TypeFlags::Abstract)) {
                return abandon(FailImport<void>(failure, AssetDiagnosticCode::TypeMismatch,
                                                !lease && lease.Error() != ErrorCode::TypeMismatch
                                                    ? lease.Error()
                                                    : ErrorCode::InvalidArgument,
                                                run.mainId));
            }

            ImportRun::Existing main(allocator);
            Result<void> step = main.path.TryAssign(run.path->Text());
            if (step) {
                step = run.existing.TryReserve(run.existing.Size() + 1);
            }

            if (!step) {
                return abandon(
                    FailImport<void>(failure, AssetDiagnosticCode::None, step.Error(), run.mainId));
            }

            AssetDiagnostic diagnostic;
            auto created = set_.Create(*run.path, info.outputType, run.mainId, &diagnostic);
            if (!created) {
                if (failure != nullptr) {
                    *failure = diagnostic;
                }

                return abandon(Result<void>::Failure(created.Error()));
            }

            // Create는 패키지를 준다 — 대표 에셋(이름 빈 자식)은 방금 만든 패키지에 있다.
            main.id = run.mainId;
            main.type = info.outputType;
            main.asset = set_.Registry().FindChild(set_.Access(), created.Value(), "").Value();
            main.created = true;
            (void)run.existing.TryPushBack(rocks::Move(main));
        }

        ObjectHandle asset{};
        for (const ImportRun::Existing& existing : run.existing) {
            asset = existing.key.IsEmpty() ? existing.asset : asset;
        }

        (void)table.TryPushBack(ImportGroupMember{run.mainId, asset});
    }

    // 구성원 순서대로 임포터를 부른다(구성원마다 자기 문맥).
    for (current = 0; current < runs.Size(); ++current) {
        if (auto staged =
                StageImport(*runs[current], table.View(), current, diagnostics, warnings, failure);
            !staged) {
            return abandon(staged);
        }
    }

    // 원소 재임포트 전파(Docs/Design/AssetSystem 12.14절 A5j2 — UE UReimportTextureFactory):
    // 재임포트한 주 출력을 EditorOnly로 참조하는 에셋을 열어 원본 재구성 hook을 Propagate로 부른다.
    // hook이 원본을 실제로 다시 쓴 에셋만 정규화하고 원소 뒤에 저장한다(묶음과 무관한 참조자 —
    // compositeTexture 등 — 는 바꾸지도 저장하지도 않는다). 저장 전에 하므로 hook 실패·저장할 수
    // 없는 대상(읽기 전용· 미해결 import)·저장하지 않은 편집이 있는 열린 대상(Busy)이면 아무것도
    // 저장하지 않는다.
    Array<AssetId> propagated(allocator);
    // 일 전부터 열려 있던 전파 대상이다(실패하면 메모리 값이 디스크와 달라지므로 dirty로 둔다).
    Array<AssetId> propagatedOpen(allocator);
    auto markOpenDirty = [&]() noexcept {
        for (AssetId id : propagatedOpen) {
            (void)set_.MarkDirty(id);
        }
    };
    for (current = 0; current < runs.Size(); ++current) {
        ImportRun& run = *runs[current];
        if (!run.member->reimport) {
            continue;
        }

        ObjectHandle changed{};
        for (const ImportRun::Existing& existing : run.existing) {
            changed = existing.key.IsEmpty() ? existing.asset : changed;
        }

        Array<const AssetIndexEntry*> referencers(allocator);
        if (auto listed = index_.Referencers(
                run.mainId, DependencyKindBit(PackageDependencyKind::EditorOnly), referencers);
            !listed) {
            markOpenDirty();
            return abandon(FailImport<void>(failure, AssetDiagnosticCode::None, listed.Error()));
        }

        for (const AssetIndexEntry* referencer : referencers) {
            bool member = false;
            for (const UniquePtr<ImportRun>& other : runs) {
                member = member || other->mainId == referencer->id;
            }

            bool seen = false;
            for (AssetId id : propagated) {
                seen = seen || id == referencer->id;
            }

            if (member || seen) {
                continue;
            }

            // 일 전부터 열려 있고 저장하지 않은 편집이 있으면 그 편집까지 저장하게 되므로 Busy다
            // (재임포트 구성원의 Busy 규칙과 같다).
            const usize at =
                algo::LowerBound(before, referencer->id, [](AssetId left, AssetId key) noexcept {
                    return GuidLess(left.value, key.value);
                });
            const bool wasOpen = at < before.Size() && before[at] == referencer->id;
            const EditorPackageInfo* openInfo = set_.Find(referencer->id);
            if (wasOpen && openInfo != nullptr && openInfo->dirty) {
                markOpenDirty();
                return abandon(FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::Busy,
                                                referencer->id));
            }

            AssetDiagnostic diagnostic;
            auto opened = set_.Open(referencer->id, warnings, &diagnostic);
            if (!opened) {
                if (failure != nullptr) {
                    *failure = diagnostic;
                }

                markOpenDirty();
                return abandon(Result<void>::Failure(opened.Error()));
            }

            auto representative = set_.Registry().FindChild(set_.Access(), opened.Value(), "");
            Result<void> step = representative ? Result<void>::Success()
                                               : Result<void>::Failure(representative.Error());
            bool rebuilt = false;
            if (step) {
                step = set_.RebuildSource(representative.Value(), referencer->id, {},
                                          AssetSourceRebuildCause::Propagate, changed, warnings,
                                          &diagnostic, &rebuilt);
            }

            if (step && rebuilt) {
                if (wasOpen) {
                    step = propagatedOpen.TryPushBack(referencer->id);
                }

                // 저장할 수 없는 대상은 지금 거부한다(Save와 같은 진단 — 원소를 저장하기 전에).
                const EditorPackageInfo* info = set_.Find(referencer->id);
                if (step && info != nullptr && info->readOnly) {
                    diagnostic = AssetDiagnostic{};
                    diagnostic.code = AssetDiagnosticCode::NewerVersion;
                    diagnostic.error = ErrorCode::Unsupported;
                    diagnostic.asset = referencer->id;
                    step = Result<void>::Failure(ErrorCode::Unsupported);
                } else if (step && info != nullptr && !info->unresolved.IsEmpty()) {
                    diagnostic = AssetDiagnostic{};
                    diagnostic.code = AssetDiagnosticCode::MissingDependency;
                    diagnostic.error = ErrorCode::InvalidArgument;
                    diagnostic.asset = referencer->id;
                    diagnostic.related = info->unresolved[0].target;
                    step = Result<void>::Failure(ErrorCode::InvalidArgument);
                }

                if (step) {
                    step = set_.NormalizeSettings(representative.Value(), referencer->id, {});
                }

                if (step) {
                    step = propagated.TryPushBack(referencer->id);
                }
            }

            if (!step) {
                if (failure != nullptr) {
                    *failure = diagnostic.code != AssetDiagnosticCode::None ? diagnostic
                                                                            : AssetDiagnostic{};
                    if (diagnostic.code == AssetDiagnosticCode::None) {
                        failure->error = step.Error();
                        failure->asset = referencer->id;
                    }
                }

                markOpenDirty();
                return abandon(Result<void>::Failure(step.Error()));
            }
        }
    }

    // 저장 준비(저장 사이에 할당이 실패하지 않게): 되돌리기의 OS 경로 버퍼·새 파일 이름 표·색인
    // 갱신 이름 표.
    current = 0;
    usize outputCount = 0;
    usize pendingCount = 0;
    for (const UniquePtr<ImportRun>& run : runs) {
        outputCount += run->context->Outputs().Size();
        pendingCount += run->pending.Size();
    }

    Array<String> savedFiles(allocator);
    Array<AssetId> savedIds(allocator);
    Array<StringView> savedNames(allocator);
    String target(allocator);
    Result<void> step = savedFiles.TryReserve(outputCount);
    if (step) {
        step = savedIds.TryReserve(outputCount);
    }

    if (step) {
        step = savedNames.TryReserve(outputCount);
    }

    if (step) {
        step =
            target.TryReserve(layout_->ContentDirectory().ByteCount() + 1 + MaxNativePathBytes());
    }

    if (!step) {
        return abandon(FailImport<void>(failure, AssetDiagnosticCode::None, step.Error()));
    }

    // 기존 파일(재임포트의 기존 출력·주 출력)을 하나라도 교체했는지다. 교체한 기존 파일은 새 파일을
    // Hard로 가리킬 수 있으므로 그 뒤의 실패는 새 파일을 지우지 않는다(디스크에는 참조가 닫힌
    // 상태만 남는다 — 교차 검증 필수 2, 남은 새 출력은 다시 실행하면 고아 입양으로 수렴한다).
    bool existingSaved = false;
    // 저장 실패의 되돌리기: 연 패키지를 닫고, 기존 파일을 교체하기 전이면 이번에 쓴 새 파일을
    // 지운다(못 지우면 경고 OrphanNotDeleted).
    auto rollback = [&](const AssetDiagnostic& diagnostic) noexcept {
        markOpenDirty();
        (void)set_.CloseOpened(before.View());
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
        if (!savedNames.IsEmpty()) {
            AssetIndexRefreshPaths paths;
            paths.files = savedNames.View();
            (void)index_.RefreshPaths(*files_, paths, read_);
        }

        // 기존 파일을 교체한 뒤의 실패는 주 출력까지 저장한 구성원의 결과(id·경로·출력)를
        // 남긴다 — 새 구성원이었으면 그 파일이 남으므로 호출자는 다음 실행에서 그 구성원을
        // 재임포트로 넘긴다(그래야 수렴한다 — 새 임포트면 DuplicatePath). 나머지는 비운다.
        for (const UniquePtr<ImportRun>& run : runs) {
            AssetImportResult& result = *run->result;
            if (existingSaved && run->mainSaved) {
                result.id = run->mainId;
                continue;
            }

            result.id = AssetId{};
            result.path.Clear();
            result.outputs.Clear();
        }

        if (failure != nullptr) {
            *failure = diagnostic;
        }

        return mark(diagnostic.error);
    };
    // 저장 직전마다 저장 경고(NotDurable·고아 임시 파일 — 새 패키지는 미해결 import가 없다)와
    // 되돌리기 경고(새 파일마다 하나)·구성원 경고 자리를 잡는다.
    constexpr usize SaveWarnings = 2;
    auto reserveWarnings = [&]() noexcept {
        return warnings == nullptr ? Result<void>::Success()
                                   : warnings->TryReserve(warnings->Size() + SaveWarnings +
                                                          savedFiles.Capacity() + pendingCount);
    };
    const EditorSaveOptions save{};
    // 패키지 하나에 기록(record — owner는 ImportOwner 의존, 주 출력은 null)을 두고 저장한다.
    auto saveOne = [&](AssetId id, Span<const uint8> record, AssetId owner,
                       bool created) noexcept -> Result<void> {
        AssetDiagnostic diagnostic;
        Result<void> saved = set_.SetImportRecord(id, record, owner);
        // 저장 뒤 파일 이름을 담을 자리를 먼저 잡는다(저장 뒤 복사가 실패하면 그 파일을 되돌리기
        // 목록에 넣지 못해 고아로 남는다). 기존 파일은 실패해도 지우지 않으므로 이름을 담지 않는다.
        String file(allocator);
        if (saved && created) {
            saved = file.TryReserve(MaxNativePathBytes());
        }

        if (saved) {
            saved = reserveWarnings();
        }

        if (!saved) {
            diagnostic = AssetDiagnostic{};
            diagnostic.error = saved.Error();
            diagnostic.asset = id;
            return rollback(diagnostic);
        }

        if (saved = set_.Save(id, lock, save, warnings, &diagnostic); !saved) {
            return rollback(diagnostic);
        }

        if (created) {
            (void)file.TryAssign(set_.Find(id)->fileName.View());
            (void)savedFiles.TryPushBack(rocks::Move(file));
            (void)savedIds.TryPushBack(id);
        } else {
            existingSaved = true;
        }

        return Result<void>::Success();
    };
    // 저장 순서(7.1절 19 A2f3d): 새 비주 출력 → 새 주 출력 → 기존 비주 출력 → 기존 주 출력(무리
    // 안에서는 구성원 순서·Output 순서). 새 파일을 먼저 써서 교체한 기존 파일이 없는 새 파일을
    // 가리키는 일이 없게 하고, 주 출력(소유 기록 — 출력 목록)은 그 무리의 출력 뒤에 쓴다.
    constexpr int SavePasses = 4;
    for (int pass = 0; pass < SavePasses; ++pass) {
        const bool fresh = pass < 2;
        const bool mains = pass == 1 || pass == 3;
        for (current = 0; current < runs.Size(); ++current) {
            ImportRun& run = *runs[current];
            for (const MadeOutput& made : run.context->Outputs()) {
                if (!made.used || made.created != fresh || made.key.IsEmpty() != mains) {
                    continue;
                }

                if (mains) {
                    if (auto saved =
                            saveOne(made.id, run.ownedRecord.View(), AssetId{}, made.created);
                        !saved) {
                        return saved;
                    }

                    run.mainSaved = true;
                    continue;
                }

                ImportRecordInput reference;
                reference.owner = run.mainId;
                auto record = WriteImportRecord(allocator, made.id, reference);
                if (!record) {
                    AssetDiagnostic diagnostic;
                    diagnostic.error = record.Error();
                    diagnostic.asset = made.id;
                    return rollback(diagnostic);
                }

                if (auto saved = saveOne(made.id, record.Value().View(), run.mainId, made.created);
                    !saved) {
                    return saved;
                }
            }
        }
    }

    // 전파한 묶음 에셋을 원소 뒤에 저장한다(기존 파일 — 실패는 다른 기존 출력과 같은 되돌리기).
    for (AssetId id : propagated) {
        AssetDiagnostic diagnostic;
        (void)set_.MarkDirty(id);
        Result<void> saved = reserveWarnings();
        if (saved) {
            saved = set_.Save(id, lock, save, warnings, &diagnostic);
        } else {
            diagnostic.error = saved.Error();
            diagnostic.asset = id;
        }

        if (!saved) {
            return rollback(diagnostic);
        }

        existingSaved = true;
    }

    // 구성원 경고를 넘긴다(자리는 저장 전에 잡았다).
    if (warnings != nullptr) {
        for (const UniquePtr<ImportRun>& run : runs) {
            for (const EditorWarning& warning : run->pending) {
                (void)warnings->TryPushBack(warning);
            }
        }
    }

    // 이번에 연 패키지(출력·그 의존·OpenAsset 대상)를 닫는다 — 저장한 깨끗한 패키지이고 원래 열려
    // 있던 패키지는 그것들을 참조할 수 없다(열기는 기존 패키지의 미해결 import를 다시 묶지 않는다).
    // 열린 채 두면 같은 서비스의 다음 재임포트가 Busy다(7.1절 18 A2f2a). 닫기 실패(할당·Discard)는
    // 성공을 바꾸지 않는다 — 저장은 끝났고, 남은 패키지는 열린 채다(호출자가 Close한다).
    (void)set_.CloseOpened(before.View());
    for (const UniquePtr<ImportRun>& run : runs) {
        run->result->id = run->mainId;
    }

    return Result<void>::Success();
}

Result<void> AssetService::PrepareImport(ImportRun& run, AssetDiagnostic* failure) noexcept {
    Allocator& allocator = *allocator_;
    const AssetImportGroupMember& member = *run.member;
    const Span<const AssetImportSourceInput> sources = member.sources;
    if (sources.IsEmpty() || member.replaceSources) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(allocator, member.path);
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
    // 읽지 않게 — 디스크에만 있는 파일은 주 출력 단계의 Create가 본다).
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

    if (!member.importer.IsEmpty()) {
        chosen = importers_->FindByName(member.importer);
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

    auto mainId = AssetId::TryNew();
    if (!mainId) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, mainId.Error());
    }

    if (auto loaded = run.roots.Load(*files_, failure); !loaded) {
        return loaded;
    }

    // 원본을 판정 순서(고정 → 접두)로 둔다.
    if (auto reserved = run.ordered.TryReserve(order.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index : order) {
        (void)run.ordered.TryPushBack(sources[index]);
    }

    run.path.Emplace(rocks::Move(parsed.Value()));
    run.mainId = mainId.Value();
    run.importer = chosen;
    run.aliases = aliases.Value();
    return Result<void>::Success();
}

Result<void> AssetService::StageImport(ImportRun& run, Span<const ImportGroupMember> members,
                                       usize memberIndex, TextDiagnostics& diagnostics,
                                       Array<EditorWarning>* warnings,
                                       AssetDiagnostic* failure) noexcept {
    Allocator& allocator = *allocator_;
    AssetImportResult& result = *run.result;
    const Span<const TextAssignment> assignments = run.member->assignments;
    AssetImporter* chosen = run.importer;
    const AssetImporterInfo& info = chosen->Info();
    if (auto reserved = run.read.TryReserve(run.ordered.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    if (auto reserved = run.contextSources.TryReserve(run.ordered.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error());
    }

    uint64 remaining = MaxImportTotalSourceBytes;
    for (usize index = 0; index < run.ordered.Size(); ++index) {
        const AssetImportSourceInput& input = run.ordered[index];
        ReadSource source(allocator);
        Result<void> step = source.role.TryAssign(input.role);
        if (step) {
            step = source.path.TryAssign(input.path);
        }

        if (!step) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error());
        }

        if (auto located = run.roots.Locate(input.path, source.location); !located) {
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
            if (auto pushed = run.pending.TryPushBack(warning); !pushed) {
                return FailImport<void>(failure, AssetDiagnosticCode::None, pushed.Error());
            }
        }

        (void)run.read.TryPushBack(rocks::Move(source));
    }

    for (const ReadSource& source : run.read) {
        (void)run.contextSources.TryPushBack(
            AssetImportSource{source.role.View(), source.path.View(), source.bytes.View()});
    }

    // 옵션과 설정을 첫 이름으로 나눈다(등록이 이름 충돌을 막았다 — 타입 lease는 표가 쥔다).
    const TypeLease& output = *importers_->OutputType(*chosen);
    OptionsValue& options = run.options;
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

        run.optionsRecord = rocks::Move(built.Value());
    } else if (!run.recordedOptions.IsEmpty()) {
        // 옵션 없는 임포터에 옵션 기록이 있다(임포터 버전이 옵션을 뺐다 — 다시 임포트한다).
        return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                ErrorCode::InvalidArgument);
    }

    // 임포터 호출(실패하면 묶음이 이번에 연 것을 닫는다).
    auto createdContext = UniquePtr<ImportContext>::TryMake(
        allocator, allocator, set_, *types_, index_, *run.aliases, run.path->Text(), info,
        run.contextSources.View(), static_cast<const void*>(options.Get()), run.mainId,
        warnings != nullptr ? &run.pending : nullptr, run.member->reimport, run.previousTags);
    if (!createdContext) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, createdContext.Error(),
                                run.mainId);
    }

    run.context = rocks::Move(createdContext.Value());
    ImportContext& context = *run.context;
    context.SetGroup(members, memberIndex);
    for (const ImportRun::Existing& existing : run.existing) {
        if (auto added = context.AddExisting(existing.key.View(), existing.path.View(), existing.id,
                                             existing.type, existing.asset, existing.created);
            !added) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, added.Error(), run.mainId);
        }
    }

    if (auto imported = chosen->Import(context); !imported) {
        AssetDiagnostic diagnostic = context.LastFailure();
        if (diagnostic.error != imported.Error()) {
            diagnostic = AssetDiagnostic{};
            diagnostic.error = imported.Error();
            diagnostic.asset = run.mainId;
        }

        if (failure != nullptr) {
            *failure = diagnostic;
        }

        return Result<void>::Failure(diagnostic.error);
    }

    const MadeOutput* main = nullptr;
    for (const MadeOutput& made : context.Outputs()) {
        main = made.key.IsEmpty() && made.used ? &made : main;
    }

    if (main == nullptr) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument,
                                run.mainId);
    }

    // 재임포트: 기록에 있는데 이번에 청하지 않은 기존 출력은 고아다(그 패키지는 그대로 — 16.1절).
    if (warnings != nullptr) {
        for (const MadeOutput& made : context.Outputs()) {
            if (made.used || made.key.IsEmpty()) {
                continue;
            }

            EditorWarning warning;
            warning.kind = EditorWarningKind::OrphanedOutput;
            warning.diagnostic.code = AssetDiagnosticCode::OrphanedOutput;
            warning.diagnostic.asset = made.id;
            warning.diagnostic.related = run.mainId;
            if (auto pushed = run.pending.TryPushBack(warning); !pushed) {
                return FailImport<void>(failure, AssetDiagnosticCode::None, pushed.Error(),
                                        run.mainId);
            }
        }
    }

    // 설정(주 출력의 저장 속성).
    if (!settingAssignments.IsEmpty()) {
        auto prepared = reflection_text::PrepareAssignments(
            AssignmentTarget::Live(set_.Registry(), set_.Access(), main->asset),
            settingAssignments.View(), codec, diagnostics);
        if (!prepared) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, prepared.Error(),
                                    run.mainId);
        }

        if (auto published = prepared.Value().Publish(); !published) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, published.Error(),
                                    run.mainId);
        }

        // 사용자 설정 대입은 override 집합에 기록한다(A5b — 임포터가 쓴 출력은 기록하지 않는다). 이
        // 단계의 실패는 임포트 실패라 아무것도 저장하지 않는다.
        OverrideSet pending(allocator);
        auto lease = set_.Registry().AcquireType(set_.Access(), main->asset);
        Result<void> recorded = lease ? asset_database_detail::PlanAssignmentOverrides(
                                            lease.Value(), settingAssignments.View(), pending)
                                      : Result<void>::Failure(lease.Error());
        if (recorded) {
            recorded = set_.ReserveOverrides(main->asset, pending);
        }

        if (!recorded) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, recorded.Error(),
                                    run.mainId);
        }

        set_.MergeOverrides(main->asset, pending);
    }

    // 설정 정규화(12.12절 — UE ValidateSettingsAfterImportOrEdit): 임포터의 기본값·재임포트 규칙과
    // 사용자 설정 대입 뒤, 검증·저장 전이다(대입이 없어도 부른다).
    {
        Array<StringView> assigned(allocator);
        Result<void> normalized =
            asset_database_detail::CollectAssignedFields(settingAssignments.View(), assigned);
        if (normalized) {
            normalized = set_.NormalizeSettings(main->asset, run.mainId, assigned.View());
        }

        if (!normalized) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, normalized.Error(),
                                    run.mainId);
        }
    }

    // 결과·기록 입력·소유 기록 byte를 만든다(저장 사이에 할당이 실패하지 않게).
    Result<void> step = run.recordSources.TryReserve(run.read.Size());
    if (step) {
        step = run.recordOutputs.TryReserve(context.Outputs().Size());
    }

    if (step) {
        step = run.recordTags.TryReserve(context.Tags().Size());
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
        return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error(), run.mainId);
    }

    for (const ReadSource& source : run.read) {
        (void)run.recordSources.TryPushBack(
            ImportSourceInput{source.role.View(), source.location.root.View(),
                              source.location.relative.View(), source.digest, source.bytes.Size()});
    }

    for (const MadeOutput& made : context.Outputs()) {
        if (made.used) {
            (void)run.recordOutputs.TryPushBack(
                ImportOutputInput{made.key.View(), made.id, made.type});
        }
    }

    // 임포터 태그(A2f3a)를 기록 입력으로 옮긴다.
    for (const ImportTag& tag : context.Tags()) {
        (void)run.recordTags.TryPushBack(ImportTagInput{tag.key.View(), tag.value.View()});
    }

    ImportRecordInput owned;
    owned.owner = run.mainId;
    owned.tags = run.recordTags.View();
    owned.importerId = info.id;
    owned.importerName = info.name;
    owned.importerVersion = info.version;
    owned.sources = run.recordSources.View();
    owned.options = run.optionsRecord.View();
    owned.outputs = run.recordOutputs.View();
    auto record = WriteImportRecord(allocator, run.mainId, owned);
    if (!record) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, record.Error(), run.mainId);
    }

    run.ownedRecord = rocks::Move(record.Value());
    return Result<void>::Success();
}

Result<void> AssetService::PrepareReimport(ImportRun& run, Span<const AssetId> before,
                                           Array<EditorWarning>* warnings,
                                           AssetDiagnostic* failure) noexcept {
    Allocator& allocator = *allocator_;
    const AssetImportGroupMember& member = *run.member;
    const Span<const AssetImportSourceInput> overrides = member.sources;
    // 묶음 전에 열려 있던 패키지다(편집 중 — Busy, before는 id 순). 묶음이 연 다른 구성원은 아니다.
    auto wasOpen = [&](AssetId id) noexcept {
        const usize at = algo::LowerBound(before, id, [](AssetId left, AssetId key) noexcept {
            return GuidLess(left.value, key.value);
        });
        return at < before.Size() && before[at] == id;
    };
    if (!member.importer.IsEmpty()) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto parsed = ParseLogicalPath(allocator, member.path);
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
    run.mainId = mainId;
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

    if (wasOpen(mainId)) {
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

        if (wasOpen(output)) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::Busy, output);
        }
    }

    auto aliases = Aliases(failure);
    if (!aliases) {
        return Result<void>::Failure(aliases.Error());
    }

    if (auto loaded = run.roots.Load(*files_, failure); !loaded) {
        return loaded;
    }

    // 주 출력을 열어 온전한 기록을 읽는다(실패하면 묶음이 이번에 연 것을 닫는다).
    if (auto opened = set_.Open(mainId, warnings, failure); !opened) {
        return Result<void>::Failure(opened.Error());
    }

    const EditorPackageInfo* mainInfo = set_.Find(mainId);
    // 미해결 import가 있으면 저장할 수 없다(asset-set과 같다 — 복제·ID 수정과 같은 규칙).
    if (!mainInfo->unresolved.IsEmpty()) {
        if (failure != nullptr) {
            *failure = AssetDiagnostic{};
            failure->code = AssetDiagnosticCode::MissingDependency;
            failure->error = ErrorCode::InvalidArgument;
            failure->asset = mainId;
            failure->related = mainInfo->unresolved[0].target;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto record = ReadImportRecord(allocator, mainId, mainInfo->importRecord.View());
    if (!record) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, record.Error(), mainId);
    }

    run.recorded = rocks::Move(record.Value());
    const ImportRecord& recorded = run.recorded;
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
        return FailImport<void>(failure, AssetDiagnosticCode::ExternalModification,
                                ErrorCode::StaleHandle, mainId);
    }

    // 원본(7.1절 18·19 A2f3b): 기록 역할은 임포터 역할 규칙에 맞아야 한다(임포터 버전이 역할을
    // 바꿨으면 InvalidImportOption — 다시 임포트한다). replaceSources면 overrides가 새 원본 전체다
    // (새 임포트와 같은 역할 규칙 — 기록 원본은 버린다), 아니면 기록 원본을 루트로 풀고 overrides가
    // 같은 역할만 바꾼다. 문맥 순서는 고정 역할 → 접두 역할(byte 순)이다.
    const AssetImporterInfo& info = importer->Info();
    Array<usize> order(allocator);
    usize badRole = 0;
    const bool single = overrides.Size() == 1 && overrides[0].role.IsEmpty();
    if (member.replaceSources) {
        // 역할 없는 원본 하나(`--source=`)는 유일한 고정 역할이다(고정 역할이 하나일 때만 —
        // 가변만 받는 임포터는 고정 역할이 없다).
        const StringView defaultRole = DefaultImportSourceRole;
        const StringView onlyRole = info.variableSourcesOnly ? StringView()
                                    : info.roles.IsEmpty()   ? defaultRole
                                    : info.roles.Size() == 1 ? info.roles[0]
                                                             : StringView();
        if (overrides.IsEmpty() || (single && onlyRole.IsEmpty())) {
            return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                    ErrorCode::InvalidArgument, mainId);
        }

        const auto roleAt = [&](usize at) noexcept {
            return single ? onlyRole : overrides[at].role;
        };
        auto checked = OrderImportRoles(allocator, info, overrides.Size(), roleAt, order, badRole);
        if (!checked) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, checked.Error(), mainId);
        }

        if (!checked.Value()) {
            return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                    ErrorCode::InvalidArgument, mainId, badRole);
        }

        if (auto reserved = run.ordered.TryReserve(order.Size()); !reserved) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error(), mainId);
        }

        for (usize index : order) {
            (void)run.ordered.TryPushBack(
                AssetImportSourceInput{roleAt(index), overrides[index].path});
        }
    } else {
        auto checked = OrderImportRoles(
            allocator, info, recorded.sources.Size(),
            [&](usize at) noexcept { return recorded.sources[at].role.View(); }, order, badRole);
        if (!checked) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, checked.Error(), mainId);
        }

        if (!checked.Value() ||
            (single && (recorded.sources.Size() != 1 || info.variableSourcesOnly))) {
            return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                    ErrorCode::InvalidArgument, mainId);
        }

        for (usize index = 0; index < overrides.Size() && !single; ++index) {
            bool known = false;
            for (const ImportSource& source : recorded.sources) {
                known = known || source.role.View() == overrides[index].role;
            }

            if (!known) {
                return FailImport<void>(failure, AssetDiagnosticCode::InvalidImportOption,
                                        ErrorCode::InvalidArgument, mainId, index);
            }
        }

        // 기록에서 푼 경로는 resolved에 둔다(ordered가 그 문자열을 가리킨다 — 미리 잡아 옮기지
        // 않는다).
        if (auto reserved = run.resolved.TryReserve(order.Size()); !reserved) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error(), mainId);
        }

        if (auto reserved = run.ordered.TryReserve(order.Size()); !reserved) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error(), mainId);
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
                        run.roots.Resolve(source.root.View(), source.relativePath.View(), path);
                    !unpacked) {
                    return FailImport<void>(failure,
                                            unpacked.Error() == ErrorCode::NotFound
                                                ? AssetDiagnosticCode::MissingSource
                                                : AssetDiagnosticCode::InvalidSource,
                                            unpacked.Error(), mainId, index);
                }

                (void)run.resolved.TryPushBack(rocks::Move(path));
                chosen = run.resolved[run.resolved.Size() - 1].View();
            }

            (void)run.ordered.TryPushBack(AssetImportSourceInput{source.role.View(), chosen});
        }
    }

    // 기존 출력을 연다(주 출력은 빈 key — 기록 key·타입, 경로는 지금 색인 경로).
    // 옛 소유 기록의 태그를 임포터에 보인다(recorded는 실행이 끝날 때까지 산다 — A2f3a).
    run.previousTags = recorded.tags.View();
    run.recordedOptions = recorded.options.View();
    if (auto reserved = run.existing.TryReserve(recorded.outputs.Size()); !reserved) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, reserved.Error(), mainId);
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
            return FailImport<void>(failure, AssetDiagnosticCode::TypeMismatch,
                                    ErrorCode::TypeMismatch, output.asset);
        }

        auto opened = set_.Open(output.asset, warnings, failure);
        if (!opened) {
            return Result<void>::Failure(opened.Error());
        }

        ImportRun::Existing existing(allocator);
        Result<void> step = existing.key.TryAssign(output.outputKey.View());
        if (step) {
            step = existing.path.TryAssign(found.Value()->path.View());
        }

        if (!step) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error(), mainId);
        }

        // Open은 패키지를 준다 — 임포터에는 대표 에셋(이름 빈 자식 — 패키지에 늘 있다)을 준다.
        existing.id = output.asset;
        existing.type = output.type;
        existing.asset = set_.Registry().FindChild(set_.Access(), opened.Value(), "").Value();
        (void)run.existing.TryPushBack(rocks::Move(existing));
    }

    // 이 owner를 가리키는 참조 기록의 고아 출력(`<주 경로>_<key>` — 앞선 임포트가 기존 파일을
    // 교체한 뒤 실패해 남긴 새 출력, 7.1절 19 A2f3d)도 기존 출력으로 더한다: 임포터가 같은 key를
    // 청하면 그 패키지를 다시 쓰고(아니면 OrphanedOutput 경고), 같은 경로 점유(DuplicatePath)로
    // 다시 실행이 막히지 않는다(수렴한다). 후보는 색인의 owner 표로 찾는다(O(log N + 후보) —
    // 재임포트마다 색인 전체를 훑지 않는다). 경로 검사를 먼저 하고 기록 출력 대조는 후보에만 한다.
    Array<const AssetIndexEntry*> referencing(allocator);
    if (auto found = index_.FindByOwner(mainId, referencing); !found) {
        return FailImport<void>(failure, AssetDiagnosticCode::None, found.Error(), mainId);
    }

    const StringView mainPath = main.path.View();
    for (const AssetIndexEntry* entry : referencing) {
        const AssetIndexEntry& candidate = *entry;
        const StringView path = candidate.path.View();
        if (candidate.id == mainId || candidate.duplicateId ||
            path.ByteCount() <= mainPath.ByteCount() + 1 ||
            !path.StartsWith(mainPath, StringComparison::CaseSensitive) ||
            path.Data()[mainPath.ByteCount()] != '_') {
            continue;
        }

        const StringView key =
            path.SliceBytes(mainPath.ByteCount() + 1, path.ByteCount() - mainPath.ByteCount() - 1);
        bool listed = !IsOutputKey(key);
        for (const ImportOutput& output : recorded.outputs) {
            listed = listed || output.asset == candidate.id;
        }

        if (listed) {
            continue;
        }

        // 기록 출력과 입양 출력의 합은 출력 상한 안이다(넘으면 기록 쓰기가 실패한다 — 그 전에
        // 거부한다, 한계: 고아를 asset-orphans-delete로 지운 뒤 다시 실행).
        if (run.existing.Size() >= MaxImportOutputs) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::CapacityExceeded,
                                    candidate.id);
        }

        // 묶음 전에 열려 있으면 Busy(기록 출력과 같다), 읽기 전용이면 NewerVersion.
        if (wasOpen(candidate.id)) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, ErrorCode::Busy,
                                    candidate.id);
        }

        if (candidate.readOnly) {
            return FailImport<void>(failure, AssetDiagnosticCode::NewerVersion,
                                    ErrorCode::Unsupported, candidate.id);
        }

        auto opened = set_.Open(candidate.id, warnings, failure);
        if (!opened) {
            return Result<void>::Failure(opened.Error());
        }

        ImportRun::Existing adopted(allocator);
        Result<void> step = adopted.key.TryAssign(key);
        if (step) {
            step = adopted.path.TryAssign(path);
        }

        if (step) {
            adopted.id = candidate.id;
            adopted.type = candidate.type;
            adopted.asset = set_.Registry().FindChild(set_.Access(), opened.Value(), "").Value();
            step = run.existing.TryPushBack(rocks::Move(adopted));
        }

        if (!step) {
            return FailImport<void>(failure, AssetDiagnosticCode::None, step.Error(), mainId);
        }
    }

    run.path.Emplace(rocks::Move(parsed.Value()));
    run.importer = importer;
    run.aliases = aliases.Value();
    return Result<void>::Success();
}

} // namespace rocks
