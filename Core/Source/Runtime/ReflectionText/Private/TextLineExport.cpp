#include "TextJsonInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/PropertyPath.h>
#include <ReflectionText/TextLines.h>

// `<경로>=<값>` 줄 출력이다(Docs/Design/ReflectionExtensions.ko.md 10.4절 "asset-info 출력" —
// A2c3). JSON 출력(TextJsonExport.cpp)과 같은 순회로 자리마다 줄을 만든다.
namespace rocks::reflection_text {

namespace {

using detail::FieldSlot;
using detail::JsonPathBuilder;
using detail::SchemaScope;

// struct 기본값 대입의 값 텍스트와 Optional unset 대입 줄의 앞말이다(10.4절 표).
constexpr StringView LineStructDefault = "{}";
constexpr StringView LineUnsetPrefix = "--unset=";
constexpr StringView LineInstancedNone = "none";

// 대상 하나의 줄 쓰기다.
struct LineExporter {
    TextCodecContext& context;
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    PathTarget root;
    const TypeLease& rootType;
    String& out;
    Allocator& allocator;
    JsonPathBuilder path;

    Result<void> WriteMembers(const SchemaScope& scope, uint32 flags) noexcept;
    Result<void> WriteField(const SchemaScope& scope, usize index, uint32 flags) noexcept;
    Result<void> WriteInstanced(uint32 flags) noexcept;
    Result<void> WriteArray(const TextSlot& slot) noexcept;
    Result<void> WriteKeyed(const TextSlot& slot) noexcept;
    Result<void> WriteSingle(const TextSlot& slot) noexcept;
    Result<void> Line(StringView prefix, StringView value, bool withValue) noexcept;
};

// 지금 경로의 줄 하나를 쓴다: prefix + 경로 [+ '=' + value] + '\n'. 줄바꿈이 든 줄은 줄 형식으로 쓸
// 수 없다(Unsupported — JSON 출력을 쓴다).
Result<void> LineExporter::Line(StringView prefix, StringView value, bool withValue) noexcept {
    String line(allocator);
    if (auto appended = line.TryAppend(prefix); !appended) {
        return appended;
    }

    if (auto formatted = FormatTextPath(context, rootType, path.View(), line); !formatted) {
        return Result<void>::Failure(formatted.Error());
    }

    // `=`·값·줄 끝 자리를 한 번에 잡는다(아래 붙이기는 실패하지 않는다).
    const usize valueBytes = withValue ? 1 + value.ByteCount() : 0;
    if (auto reserved = line.TryReserve(line.ByteCount() + valueBytes + 1); !reserved) {
        return reserved;
    }

    if (withValue) {
        (void)line.TryAppend("=");
        (void)line.TryAppend(value);
    }

    const StringView text = line.View();
    for (usize index = 0; index < text.ByteCount(); ++index) {
        if (text.Data()[index] == '\n' || text.Data()[index] == '\r') {
            return Result<void>::Failure(ErrorCode::Unsupported);
        }
    }

    (void)line.TryAppend("\n");
    return out.TryAppend(line.View());
}

Result<void> LineExporter::WriteMembers(const SchemaScope& scope, uint32 flags) noexcept {
    const auto& fields = scope.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (auto written = WriteField(scope, index, flags); !written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// InstancedStruct 자리(지금 경로): 비어 있으면 `none`, 아니면 타입 도구 이름 줄 뒤 멤버 줄이다.
Result<void> LineExporter::WriteInstanced(uint32 flags) noexcept {
    auto type = PathReadInstancedType(registry, access, root, path.View());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (!type.Value().IsValid()) {
        return Line(StringView(), LineInstancedNone, true);
    }

    const Span<const char> name = type.Value().Schema().toolName;
    if (name.Size() == 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (auto written = Line(StringView(), StringView(name.Data(), name.Size()), true); !written) {
        return written;
    }

    return WriteMembers(SchemaScope{nullptr, type.Value().Struct()}, flags);
}

Result<void> LineExporter::WriteField(const SchemaScope& scope, usize index,
                                      uint32 flags) noexcept {
    const TextSlot slot = FieldSlot(scope, index, flags);
    // 용도의 출력 제외 플래그가 경로의 어느 단계에 있으면 그 필드를 통째로 뺀다(10.1절).
    if (IsTextOutputExcluded(context.Options().use, slot.flags)) {
        return Result<void>::Success();
    }

    // 이벤트 필드는 값이 아니라 텍스트 표기가 없다 — 통째로 뺀다(Docs/Design/EventReflection.ko.md
    // 3.1절).
    if (IsEventField(*slot.field)) {
        return Result<void>::Success();
    }

    if (detail::HasNoJsonForm(*slot.field)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!path.Push(slot.field->id)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const FieldSchema& field = *slot.field;
    Result<void> written = Result<void>::Success();
    switch (field.value.kind) {
    case ValueKind::Struct:
        written = WriteMembers(SchemaScope{nullptr, slot.structView}, slot.flags);
        break;
    case ValueKind::InstancedStruct:
        written = WriteInstanced(slot.flags);
        break;
    case ValueKind::Optional:
        if (field.element.kind == ValueKind::Struct) {
            auto set = PathIsOptionalSet(registry, access, root, path.View());
            if (!set) {
                written = Result<void>::Failure(set.Error());
            } else if (!set.Value()) {
                written = Line(LineUnsetPrefix, StringView(), false);
            } else {
                written = Line(StringView(), LineStructDefault, true);
                if (written) {
                    written = WriteMembers(SchemaScope{nullptr, slot.structView}, slot.flags);
                }
            }
        } else {
            written = WriteSingle(slot);
        }

        break;
    case ValueKind::Array:
    case ValueKind::StaticArray:
        written = WriteArray(slot);
        break;
    case ValueKind::Map:
    case ValueKind::Set:
        written = WriteKeyed(slot);
        break;
    default:
        written = WriteSingle(slot);
        break;
    }

    path.Pop();
    return written;
}

// 단일 값 자리(필드·Optional<단일 값>): unset Optional은 `--unset=` 줄이다.
Result<void> LineExporter::WriteSingle(const TextSlot& slot) noexcept {
    TextValue value(allocator);
    if (auto read = PathRead(registry, access, root, path.View(), value.single); !read) {
        return read;
    }

    if (!value.single.present) {
        return Line(LineUnsetPrefix, StringView(), false);
    }

    value.shape = TextValueShape::Single;
    String text(allocator);
    if (auto formatted = FormatTextValue(context, slot, value, text); !formatted) {
        return formatted;
    }

    return Line(StringView(), text.View(), true);
}

Result<void> LineExporter::WriteArray(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    usize count = field.maxElements;
    if (field.value.kind == ValueKind::Array) {
        auto counted = PathCount(registry, access, root, path.View());
        if (!counted) {
            return Result<void>::Failure(counted.Error());
        }

        count = counted.Value();
    }

    const bool structElement = field.element.kind == ValueKind::Struct;
    const bool instancedElement = field.element.kind == ValueKind::InstancedStruct;
    if (field.value.kind == ValueKind::StaticArray &&
        field.element.kind == ValueKind::ObjectReference) {
        // 강한 참조 원소의 정적 배열은 원소마다 `a[i]=v`다 — 대입 묶음에서 통째는 준비 단위가 없고
        // 원소 자리가 참조 단위다(10.4절).
        TextSlot element = slot;
        element.part = TextSlotPart::Element;
        for (usize index = 0; index < count; ++index) {
            path.SetIndex(static_cast<uint32>(index));
            auto written = WriteSingle(element);
            path.ClearSelector();
            if (!written) {
                return written;
            }
        }

        return Result<void>::Success();
    }

    if (!structElement && !instancedElement) {
        // 단일 값 원소: 쉼표 목록 한 줄(바이트 배열은 Base64 — FormatTextValue).
        TextValue value(allocator);
        value.shape = TextValueShape::List;
        if (auto reserved = value.elements.TryReserve(count); !reserved) {
            return reserved;
        }

        for (usize index = 0; index < count; ++index) {
            path.SetIndex(static_cast<uint32>(index));
            FieldValue element(allocator);
            auto read = PathRead(registry, access, root, path.View(), element);
            path.ClearSelector();
            if (!read) {
                return read;
            }

            (void)value.elements.TryPushBack(Move(element));
        }

        String text(allocator);
        if (auto formatted = FormatTextValue(context, slot, value, text); !formatted) {
            return formatted;
        }

        return Line(StringView(), text.View(), true);
    }

    // struct·InstancedStruct 원소: 가변 Array는 비운 뒤 원소마다 만들고(`a[i]={}` —
    // InstancedStruct는 `a[i]=<타입>`), StaticArray는 원소 자리에 바로 멤버를 쓴다.
    const bool growable = field.value.kind == ValueKind::Array;
    if (growable) {
        if (auto written = Line(StringView(), StringView(), true); !written) {
            return written;
        }
    }

    for (usize index = 0; index < count; ++index) {
        path.SetIndex(static_cast<uint32>(index));
        Result<void> written = Result<void>::Success();
        if (instancedElement) {
            written = WriteInstanced(slot.flags);
        } else {
            if (growable) {
                written = Line(StringView(), LineStructDefault, true);
            }

            if (written) {
                written = WriteMembers(SchemaScope{nullptr, slot.structView}, slot.flags);
            }
        }

        path.ClearSelector();
        if (!written) {
            return written;
        }
    }

    return Result<void>::Success();
}

Result<void> LineExporter::WriteKeyed(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    Array<FieldValue> keys(allocator);
    if (auto read = PathReadKeys(registry, access, root, path.View(), keys); !read) {
        return read;
    }

    const bool set = field.value.kind == ValueKind::Set;
    const bool structValue = !set && field.element.kind == ValueKind::Struct;
    const bool instancedValue = !set && field.element.kind == ValueKind::InstancedStruct;
    if (!structValue && !instancedValue) {
        // Set은 원소 목록, 단일 값 Map은 `k:v` 목록 한 줄이다.
        TextValue value(allocator);
        value.shape = set ? TextValueShape::List : TextValueShape::Entries;
        if (!set) {
            if (auto reserved = value.values.TryReserve(keys.Size()); !reserved) {
                return reserved;
            }

            for (const FieldValue& key : keys) {
                if (auto selected = path.SetKey(context.Options().names, allocator, key);
                    !selected) {
                    return selected;
                }

                FieldValue entry(allocator);
                auto read = PathRead(registry, access, root, path.View(), entry);
                path.ClearSelector();
                if (!read) {
                    return read;
                }

                (void)value.values.TryPushBack(Move(entry));
            }
        }

        value.elements = Move(keys);
        String text(allocator);
        if (auto formatted = FormatTextValue(context, slot, value, text); !formatted) {
            return formatted;
        }

        return Line(StringView(), text.View(), true);
    }

    // struct·InstancedStruct 값: 비운 뒤 항목마다 만든다(`m[k]={}` — InstancedStruct는
    // `m[k]=<타입>`).
    if (auto written = Line(StringView(), StringView(), true); !written) {
        return written;
    }

    for (const FieldValue& key : keys) {
        if (auto selected = path.SetKey(context.Options().names, allocator, key); !selected) {
            return selected;
        }

        Result<void> written = Result<void>::Success();
        if (instancedValue) {
            written = WriteInstanced(slot.flags);
        } else {
            written = Line(StringView(), LineStructDefault, true);
            if (written) {
                written = WriteMembers(SchemaScope{nullptr, slot.structView}, slot.flags);
            }
        }

        path.ClearSelector();
        if (!written) {
            return written;
        }
    }

    return Result<void>::Success();
}

// 줄 경로 표기(FormatTextPath)의 InstancedStruct 담긴 타입을 대상 값에서 읽는 resolver다.
class TargetInstancedTypes final : public PathInstancedTypeResolver {
public:
    TargetInstancedTypes(ObjectRegistry& registry, const ObjectAccessContext& access,
                         const PathTarget& root) noexcept
        : registry_(registry), access_(access), root_(root) {
    }

    Result<void> InstancedType(PropertyPath prefix, TypeLease& out) noexcept override {
        auto type = PathReadInstancedType(registry_, access_, root_, prefix);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        out = Move(type.Value());
        return Result<void>::Success();
    }

private:
    ObjectRegistry& registry_;
    const ObjectAccessContext& access_;
    const PathTarget& root_;
};

} // namespace

Result<void> ExportLines(const AssignmentTarget& target, String& out,
                         const TextCodecOptions& options) noexcept {
    ObjectRegistry& registry = target.Registry();
    TextCodecOptions effective = options;
    if (!effective.names) {
        effective.names = registry.Names();
    } else if (effective.names != registry.Names()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = target.ValueAllocator();
    // 출력은 진단을 남기지 않는다(쓰기 실패는 호출 오류다) — 문맥이 요구하는 빈 모음이다.
    TextDiagnostics diagnostics(allocator);
    TextCodecContext context(registry.Types(), effective, diagnostics, allocator);
    if (auto started = context.Start(); !started) {
        return started;
    }

    const ObjectAccessContext& access = target.Access();
    Result<TypeLease> lease = Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    PathTarget root = PathTarget(target.Handle());
    switch (target.GetKind()) {
    case AssignmentTarget::Kind::Live:
        lease = registry.AcquireType(access, target.Handle());
        break;
    case AssignmentTarget::Kind::Staged:
        lease = target.Transaction()->AcquireType(target.Handle());
        root = PathTarget::Staged(*target.Transaction(), target.Handle());
        break;
    case AssignmentTarget::Kind::Value: {
        const TypeLease* type = target.StructType();
        if (!type || !type->IsValid() || type->Kind() != TypeKind::Struct || !target.Storage()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        lease = type->Clone();
        root = PathTarget::Value(*type, type->Struct(), target.Storage(), allocator);
        break;
    }
    }

    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    const TypeLease& type = lease.Value();
    TargetInstancedTypes instanced(registry, access, root);
    context.SetInstancedTypes(&instanced);
    LineExporter exporter{context, registry, access,    root,
                          type,    out,      allocator, JsonPathBuilder(allocator)};
    const SchemaScope scope = type.Kind() == TypeKind::Struct ? SchemaScope{nullptr, type.Struct()}
                                                              : SchemaScope{&type, StructView{}};
    return exporter.WriteMembers(scope, 0);
}

} // namespace rocks::reflection_text
