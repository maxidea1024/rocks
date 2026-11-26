#include <AssetDatabase/AssetImporter.h>
#include <AssetDatabase/ImportRecord.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <AssetRuntime/PackageTags.h>
#include <Object/PropertyPath.h>
#include <Object/TypeRegistry.h>

namespace rocks {

namespace {

bool IsWordChar(char c, bool upper, bool dot) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (upper && c >= 'A' && c <= 'Z') ||
           (upper && c == '_') || (dot && c == '.');
}

// 1–maxBytes byte이고 모든 글자가 허용 글자인지다.
bool IsToken(StringView text, usize maxBytes, bool upper, bool dot) noexcept {
    if (text.IsEmpty() || text.ByteCount() > maxBytes) {
        return false;
    }

    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (!IsWordChar(text.Data()[index], upper, dot)) {
            return false;
        }
    }

    return true;
}

// 원본 역할이다(1–MaxImportRoleBytes byte의 `[A-Za-z0-9_.:]` — `:`는 가변 원본 역할
// `blob:<sha256>`, Docs/Design/AssetSystem 7.1절 19 A2f3b).
bool IsRole(StringView role) noexcept {
    if (role.IsEmpty() || role.ByteCount() > MaxImportRoleBytes) {
        return false;
    }

    for (usize index = 0; index < role.ByteCount(); ++index) {
        const char c = role.Data()[index];
        if (!IsWordChar(c, true, true) && c != ':') {
            return false;
        }
    }

    return true;
}

// 엔진이 쓰는 태그 이름공간과 같은 임포터 이름인지다(대소문자 무시 — 임포터 태그 키
// `<이름>.<…>`가 bundle 구성원 태그 `Bundle.<이름>`과 겹치지 않게, A2f3a).
bool IsReservedImporterName(StringView name) noexcept {
    const StringView bundle(AssetBundleTagPrefix.Data(), AssetBundleTagPrefix.ByteCount() - 1);
    return EqualsIgnoringAsciiCase(name, bundle);
}

// 목록 안에 (규칙대로 비교해) 같은 것이 둘 있는지다.
bool HasDuplicate(Span<const StringView> items) noexcept {
    for (usize index = 0; index < items.Size(); ++index) {
        for (usize other = 0; other < index; ++other) {
            if (items[index] == items[other]) {
                return true;
            }
        }
    }

    return false;
}

} // namespace

AssetImporterRegistration::AssetImporterRegistration(AssetImporterRegistration&& other) noexcept
    : table_(other.table_), importer_(other.importer_) {
    other.table_ = nullptr;
    other.importer_ = nullptr;
}

AssetImporterRegistration&
AssetImporterRegistration::operator=(AssetImporterRegistration&& other) noexcept {
    // 자기 이동 대입도 같은 결과다(해제한 뒤 비운 값을 받는다).
    Release();
    table_ = other.table_;
    importer_ = other.importer_;
    other.table_ = nullptr;
    other.importer_ = nullptr;
    return *this;
}

AssetImporterRegistration::~AssetImporterRegistration() {
    Release();
}

void AssetImporterRegistration::Release() noexcept {
    if (table_ != nullptr) {
        table_->Remove(importer_);
        table_ = nullptr;
        importer_ = nullptr;
    }
}

AssetImporterTable::AssetImporterTable(Allocator& allocator, TypeRegistry& types) noexcept
    : types_(&types), importers_(allocator) {
}

Result<void> AssetImporterTable::Validate(const AssetImporterInfo& info, Entry& entry) noexcept {
    auto invalid = []() noexcept { return Result<void>::Failure(ErrorCode::InvalidArgument); };
    // 태그 접두(A2f5)는 비면 이름이고, 있으면 이름과 같은 규칙이다.
    const bool tagPrefixValid =
        info.tagPrefix.IsEmpty() || (IsToken(info.tagPrefix, MaxImporterNameBytes, true, false) &&
                                     !IsReservedImporterName(info.tagPrefix));
    if (info.id == Guid{} || !IsToken(info.name, MaxImporterNameBytes, true, false) ||
        IsReservedImporterName(info.name) || !tagPrefixValid ||
        info.extensions.Size() > MaxImporterExtensions ||
        info.roles.Size() > MaxImportSources || HasDuplicate(info.extensions) ||
        HasDuplicate(info.roles)) {
        return invalid();
    }

    for (StringView extension : info.extensions) {
        if (!IsToken(extension, MaxImporterExtensionBytes, false, false)) {
            return invalid();
        }
    }

    for (StringView role : info.roles) {
        if (!IsRole(role)) {
            return invalid();
        }
    }

    // 가변 원본 접두(A2f3b): 역할 문자, 고정 역할이 접두로 시작하지 않고, 접두끼리 앞부분이 아니다
    // (어느 역할이 어느 규칙에 드는지 모호함이 없게).
    if (info.rolePrefixes.Size() > MaxImportSources || HasDuplicate(info.rolePrefixes)) {
        return invalid();
    }

    // 가변만 받는 임포터(A5h): 고정 역할이 없고 접두가 하나 이상이다.
    if (info.variableSourcesOnly && (!info.roles.IsEmpty() || info.rolePrefixes.IsEmpty())) {
        return invalid();
    }

    const StringView defaultRole = DefaultImportSourceRole;
    const Span<const StringView> fixedRoles = info.variableSourcesOnly ? Span<const StringView>()
                                              : info.roles.IsEmpty()
                                                  ? Span<const StringView>(&defaultRole, 1)
                                                  : info.roles;
    for (usize index = 0; index < info.rolePrefixes.Size(); ++index) {
        const StringView prefix = info.rolePrefixes[index];
        if (!IsRole(prefix)) {
            return invalid();
        }

        for (StringView role : fixedRoles) {
            if (role.StartsWith(prefix, StringComparison::CaseSensitive)) {
                return invalid();
            }
        }

        for (usize other = 0; other < info.rolePrefixes.Size(); ++other) {
            if (other != index &&
                info.rolePrefixes[other].StartsWith(prefix, StringComparison::CaseSensitive)) {
                return invalid();
            }
        }
    }

    // 주 출력: 추상 아닌 에셋 클래스.
    // 조회의 종류 불일치(TypeMismatch)는 규칙 위반이다.
    auto lookupError = [](ErrorCode error) noexcept {
        return Result<void>::Failure(error == ErrorCode::TypeMismatch ? ErrorCode::InvalidArgument
                                                                      : error);
    };
    auto output = types_->Acquire(info.outputType);
    if (!output) {
        return lookupError(output.Error());
    }

    // Acquire는 클래스만 준다(struct·enum은 TypeMismatch — 위에서 InvalidArgument).
    const TypeLease& outputType = output.Value();
    if (!outputType.IsA(GeneratedType<Asset>::Id()) ||
        HasFlag(outputType.Schema().flags, TypeFlags::Abstract)) {
        return invalid();
    }

    if (info.optionsType == TypeId{}) {
        entry.output = rocks::Move(output.Value());
        return Result<void>::Success();
    }

    // 옵션: struct이고 필드 이름이 예약 이름·주 출력의 노출 속성과 겹치지 않는다.
    auto options = types_->AcquireValueType(info.optionsType, TypeKind::Struct);
    if (!options) {
        return lookupError(options.Error());
    }

    for (const FieldSchema& field : options.Value().Schema().fields) {
        const Span<const char> raw = field.toolName.Size() != 0 ? field.toolName : field.name;
        const StringView name(raw.Data(), raw.Size());
        for (StringView reserved : ImportReservedNames) {
            if (EqualsIgnoringAsciiCase(name, reserved)) {
                return invalid();
            }
        }

        PropertyPathBuffer path;
        if (ParsePropertyPath(outputType, name, path)) {
            return invalid();
        }
    }

    entry.output = rocks::Move(output.Value());
    entry.options = rocks::Move(options.Value());
    return Result<void>::Success();
}

Result<AssetImporterRegistration> AssetImporterTable::Register(AssetImporter& importer) noexcept {
    using Out = Result<AssetImporterRegistration>;
    const AssetImporterInfo& info = importer.Info();
    Entry entry;
    entry.importer = &importer;
    if (auto valid = Validate(info, entry); !valid) {
        return Out::Failure(valid.Error());
    }

    for (const Entry& other : importers_) {
        const AssetImporterInfo& existing = other.importer->Info();
        if (existing.id == info.id || EqualsIgnoringAsciiCase(existing.name, info.name)) {
            return Out::Failure(ErrorCode::AlreadyExists);
        }

        for (StringView extension : info.extensions) {
            for (StringView taken : existing.extensions) {
                if (extension == taken) {
                    return Out::Failure(ErrorCode::AlreadyExists);
                }
            }
        }
    }

    if (auto pushed = importers_.TryPushBack(rocks::Move(entry)); !pushed) {
        return Out::Failure(pushed.Error());
    }

    AssetImporterRegistration registration;
    registration.table_ = this;
    registration.importer_ = &importer;
    return Out::Success(rocks::Move(registration));
}

AssetImporter* AssetImporterTable::FindById(Guid id) const noexcept {
    for (const Entry& entry : importers_) {
        if (entry.importer->Info().id == id) {
            return entry.importer;
        }
    }

    return nullptr;
}

AssetImporter* AssetImporterTable::FindByName(StringView name) const noexcept {
    for (const Entry& entry : importers_) {
        if (EqualsIgnoringAsciiCase(entry.importer->Info().name, name)) {
            return entry.importer;
        }
    }

    return nullptr;
}

AssetImporter* AssetImporterTable::FindByExtension(StringView extension) const noexcept {
    for (const Entry& entry : importers_) {
        for (StringView own : entry.importer->Info().extensions) {
            if (EqualsIgnoringAsciiCase(own, extension)) {
                return entry.importer;
            }
        }
    }

    return nullptr;
}

const AssetImporterTable::Entry*
AssetImporterTable::EntryOf(const AssetImporter& importer) const noexcept {
    for (const Entry& entry : importers_) {
        if (entry.importer == &importer) {
            return &entry;
        }
    }

    return nullptr;
}

const TypeLease* AssetImporterTable::OutputType(const AssetImporter& importer) const noexcept {
    const Entry* entry = EntryOf(importer);
    return entry != nullptr ? &entry->output : nullptr;
}

const TypeLease* AssetImporterTable::OptionsType(const AssetImporter& importer) const noexcept {
    const Entry* entry = EntryOf(importer);
    return entry != nullptr && entry->options.IsValid() ? &entry->options : nullptr;
}

void AssetImporterTable::Remove(AssetImporter* importer) noexcept {
    // 토큰이 가리키는 임포터는 반드시 표에 있다(등록만 토큰을 만들고 해제는 한 번이다).
    usize index = 0;
    while (importers_[index].importer != importer) {
        ++index;
    }

    (void)importers_.TryRemoveAt(index);
}

} // namespace rocks
