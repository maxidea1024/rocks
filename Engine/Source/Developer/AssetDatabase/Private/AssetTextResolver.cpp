#include <AssetDatabase/AssetTextResolver.h>
#include <AssetDatabase/LogicalPath.h>
#include <AssetRuntime/Package.generated.h>
#include <AssetRuntime/Package.h>
#include <Core/Text/NameComponent.h>

// 에셋 참조 텍스트 resolver다(Docs/Design/AssetSystem 7.1절 15 A2c3).
namespace rocks {

namespace {

using reflection_text::TextReferenceKind;

// 패키지 부분과 객체 경로를 나누는 글자다(구성 요소에는 없다 — 4.1·4.2절).
constexpr char ObjectPathSeparator = '.';
constexpr StringView ObjectPathPrefix = ".";

// text를 첫 `.`에서 패키지 부분과 객체 경로로 나눈다. `.`가 있으면 objectPath는 비어 있지 않아야
// 한다(false).
bool SplitReference(StringView text, StringView& head, StringView& objectPath) noexcept {
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] == ObjectPathSeparator) {
            head = StringView(text.Data(), index);
            objectPath = text.SliceBytes(index + 1, text.ByteCount() - index - 1);
            return !objectPath.IsEmpty();
        }
    }

    head = text;
    objectPath = StringView();
    return true;
}

// 객체 경로의 구성 요소마다 이름 규칙을 본다(Soft는 객체를 찾지 않고, Hard·Weak는 열기 전에 본다).
Result<void> ValidateObjectPath(StringView path, Allocator& scratch) noexcept {
    usize start = 0;
    for (usize index = 0; index <= path.ByteCount(); ++index) {
        if (index != path.ByteCount() && path.Data()[index] != ObjectPathSeparator) {
            continue;
        }

        if (auto valid = ValidateNameComponent(path.SliceBytes(start, index - start), scratch);
            !valid) {
            return valid;
        }

        start = index + 1;
    }

    return Result<void>::Success();
}

} // namespace

AssetTextResolver::AssetTextResolver(Allocator& allocator, EditorPackageSet& set,
                                     const AssetIndex& index,
                                     Array<EditorWarning>* warnings) noexcept
    : allocator_(&allocator), set_(&set), index_(&index), warnings_(warnings) {
}

Result<AssetId> AssetTextResolver::ResolveId(StringView head) noexcept {
    if (!head.StartsWith("/", StringComparison::CaseSensitive)) {
        auto parsed = AssetId::TryParse(head);
        if (!parsed || parsed.Value().IsNull()) {
            return Result<AssetId>::Failure(ErrorCode::InvalidArgument);
        }

        return parsed;
    }

    auto path = ParseLogicalPath(*allocator_, head);
    if (!path) {
        return Result<AssetId>::Failure(path.Error());
    }

    // 저장하지 않은 새 패키지는 색인에 없다(Create가 색인과 겹치지 않게 만든다).
    for (usize index = 0; index < set_->Count(); ++index) {
        const EditorPackageInfo& info = set_->At(index);
        if (!info.unsaved) {
            continue;
        }

        auto other = ParseLogicalPath(*allocator_, info.logicalPath.View());
        if (!other) {
            return Result<AssetId>::Failure(other.Error());
        }

        if (other.Value() == path.Value()) {
            return Result<AssetId>::Success(info.id);
        }
    }

    auto entry = index_->FindByPath(path.Value());
    if (!entry) {
        // 색인에 없는 경로는 별칭으로 푼다(옛 경로 — 같은 경로 키 둘은 그대로 오류).
        if (entry.Error() == ErrorCode::NotFound && aliases_ != nullptr) {
            return aliases_->ResolveAlias(path.Value(), warnings_);
        }

        return Result<AssetId>::Failure(entry.Error());
    }

    return Result<AssetId>::Success(entry.Value()->id);
}

Result<ObjectHandle> AssetTextResolver::OpenPackage(AssetId id) noexcept {
    if (const EditorPackageInfo* info = set_->Find(id); info != nullptr) {
        return Result<ObjectHandle>::Success(info->package);
    }

    return set_->Open(id, warnings_);
}

Result<void> AssetTextResolver::Resolve(StringView text, TypeId expected, TextReferenceKind kind,
                                        FieldValue& out) noexcept {
    StringView head;
    StringView objectPath;
    if (!SplitReference(text, head, objectPath)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto id = ResolveId(head);
    if (!id) {
        return Result<void>::Failure(id.Error());
    }

    // 객체 경로 형식은 열기 전에 본다(형식 거부는 아무것도 열지 않는다).
    if (!objectPath.IsEmpty()) {
        if (auto valid = ValidateObjectPath(objectPath, *allocator_); !valid) {
            return valid;
        }
    }

    if (kind == TextReferenceKind::Soft) {
        if (auto assigned = out.string.TryAssign(objectPath); !assigned) {
            return assigned;
        }

        out.guid = id.Value().value;
        return Result<void>::Success();
    }

    Result<ObjectHandle> package = Result<ObjectHandle>::Failure(ErrorCode::NotFound);
    if (kind == TextReferenceKind::Weak) {
        // Weak은 대입 대상의 패키지 안만 보존된다(5.1절) — 밖이면 거부하고 열지 않는다.
        if (weakScope_.IsNull() || !(id.Value() == weakScope_)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (const EditorPackageInfo* info = set_->Find(weakScope_); info != nullptr) {
            package = Result<ObjectHandle>::Success(info->package);
        }
    } else {
        package = OpenPackage(id.Value());
    }

    if (!package) {
        return Result<void>::Failure(package.Error());
    }

    ObjectRegistry& registry = set_->Registry();
    const ObjectAccessContext access = set_->Access();
    auto target = registry.FindObject(access, package.Value(), objectPath);
    if (!target) {
        return Result<void>::Failure(target.Error());
    }

    if (expected != TypeId{}) {
        auto type = registry.AcquireType(access, target.Value());
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        if (!type.Value().IsA(expected)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    out.reference = target.Value();
    return Result<void>::Success();
}

Result<void> AssetTextResolver::AppendPackage(AssetId id, StringBuilderBase& out) noexcept {
    if (const EditorPackageInfo* info = set_->Find(id); info != nullptr && info->unsaved) {
        return out.TryAppend(info->logicalPath.View());
    }

    if (auto entry = index_->FindById(id); entry) {
        return out.TryAppend(entry.Value()->path.View());
    }

    return out.TryAppendFormat("{}", id);
}

Result<void> AssetTextResolver::Format(TextReferenceKind kind, const FieldValue& value,
                                       StringBuilderBase& out) noexcept {
    if (kind == TextReferenceKind::Soft) {
        if (auto appended = AppendPackage(AssetId{value.guid}, out); !appended) {
            return appended;
        }

        if (value.string.IsEmpty()) {
            return Result<void>::Success();
        }

        // `.<객체 경로>`를 한 번에 붙인다(자리를 먼저 잡아 아래 붙이기는 실패하지 않는다).
        String tail(*allocator_);
        if (auto reserved = tail.TryReserve(1 + value.string.ByteCount()); !reserved) {
            return reserved;
        }

        (void)tail.TryAppend(ObjectPathPrefix);
        (void)tail.TryAppend(value.string.View());
        return out.TryAppend(tail.View());
    }

    ObjectRegistry& registry = set_->Registry();
    const ObjectAccessContext access = set_->Access();
    auto outermost = registry.GetOutermost(access, value.reference);
    if (!outermost) {
        return Result<void>::Failure(outermost.Error());
    }

    // Package 객체 자신은 텍스트로 나타낼 수 없다(빈 객체 경로는 대표 에셋이다).
    if (outermost.Value() == value.reference) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 패키지 정책 Registry라 outermost는 언제나 Package다(ObjectIdentity.h RegistryPolicy).
    auto borrowed = registry.Resolve<Package>(access, outermost.Value());
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto package = borrowed.Value().Get();
    if (!package) {
        return Result<void>::Failure(package.Error());
    }

    const AssetId id = package.Value()->Id();
    // `.` 뒤에 객체 경로를 읽어 `.<객체 경로>`를 만든다(대표 에셋은 `.`만 남는다).
    String tail(*allocator_);
    if (auto started = tail.TryAppend(ObjectPathPrefix); !started) {
        return started;
    }

    if (auto described = registry.GetObjectPath(access, value.reference, tail); !described) {
        return described;
    }

    if (auto appended = AppendPackage(id, out); !appended) {
        return appended;
    }

    if (tail.ByteCount() == ObjectPathPrefix.ByteCount()) {
        return Result<void>::Success();
    }

    return out.TryAppend(tail.View());
}

} // namespace rocks
