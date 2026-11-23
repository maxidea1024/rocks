#include "TextJsonInternal.h"

#include <Object/ConstructionContext.h>
#include <Object/FieldValueWire.h>
#include <ReflectionText/TextVisit.h>

// 값 순회다(Docs/Design/ReflectionExtensions.ko.md 10.6절 — ReflectionText/TextVisit.h). ExportJson
// (TextJsonExport.cpp)과 같은 스키마·경로 연산 순회다: 단일 값은 PathRead, 배열은 PathCount와 원소
// 경로, Map·Set은 PathReadKeys와 key 경로로 읽는다.
namespace rocks::reflection_text {

namespace {

using detail::FieldSlot;
using detail::JsonPathBuilder;
using detail::SchemaScope;

// 대상 하나의 순회다.
struct Visitor {
    TextCodecContext& context;
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    PathTarget root;
    ValueVisitor& visitor;
    Allocator& allocator;
    JsonPathBuilder path;

    Result<void> VisitStruct(const SchemaScope& scope, uint32 flags) noexcept;
    Result<void> VisitInstanced(uint32 flags) noexcept;
    Result<void> VisitField(const SchemaScope& scope, usize index, uint32 flags) noexcept;
    Result<void> VisitValue(const TextSlot& slot) noexcept;
    Result<void> VisitElement(const TextSlot& slot) noexcept;
    Result<void> VisitArray(const TextSlot& slot) noexcept;
    Result<void> VisitKeyed(const TextSlot& slot) noexcept;
    Result<void> VisitRead() noexcept;
};

Result<void> Visitor::VisitStruct(const SchemaScope& scope, uint32 flags) noexcept {
    if (auto begun = visitor.BeginStruct(); !begun) {
        return begun;
    }

    const auto& fields = scope.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (auto visited = VisitField(scope, index, flags); !visited) {
            return visited;
        }
    }

    return visitor.EndStruct();
}

// InstancedStruct는 비어 있으면 Absent, 아니면 담긴 타입과 그 struct다(멤버는 지금 담긴 struct의
// 경로로 읽는다).
Result<void> Visitor::VisitInstanced(uint32 flags) noexcept {
    auto type = PathReadInstancedType(registry, access, root, path.View());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (!type.Value().IsValid()) {
        return visitor.Absent();
    }

    const TypeSchema& schema = type.Value().Schema();
    if (auto named = visitor.Instanced(schema.id,
                                       StringView(schema.toolName.Data(), schema.toolName.Size()));
        !named) {
        return named;
    }

    return VisitStruct(SchemaScope{nullptr, type.Value().Struct()}, flags);
}

Result<void> Visitor::VisitField(const SchemaScope& scope, usize index, uint32 flags) noexcept {
    const TextSlot slot = FieldSlot(scope, index, flags);
    // 용도의 출력 제외 플래그가 경로의 어느 단계에 있으면 그 필드를 통째로 뺀다(10.1절).
    if (IsTextOutputExcluded(context.Options().use, slot.flags)) {
        return Result<void>::Success();
    }

    if (detail::HasNoJsonForm(*slot.field)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (!path.Push(slot.field->id)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    Result<void> visited =
        visitor.Field(detail::FieldToolName(*slot.field), *slot.field, slot.flags);
    if (visited) {
        visited = VisitValue(slot);
    }

    path.Pop();
    return visited;
}

Result<void> Visitor::VisitValue(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    switch (field.value.kind) {
    case ValueKind::Struct:
        return VisitStruct(SchemaScope{nullptr, slot.structView}, slot.flags);
    case ValueKind::InstancedStruct:
        return VisitInstanced(slot.flags);
    case ValueKind::Optional:
        if (field.element.kind == ValueKind::Struct) {
            auto set = PathIsOptionalSet(registry, access, root, path.View());
            if (!set) {
                return Result<void>::Failure(set.Error());
            }

            return set.Value() ? VisitStruct(SchemaScope{nullptr, slot.structView}, slot.flags)
                               : visitor.Absent();
        }

        return VisitRead();
    case ValueKind::Array:
    case ValueKind::StaticArray:
        return VisitArray(slot);
    case ValueKind::Map:
    case ValueKind::Set:
        return VisitKeyed(slot);
    default:
        return VisitRead();
    }
}

// 컨테이너 원소·Map 값 하나다(지금 경로의 선택자가 그 원소를 가리킨다).
Result<void> Visitor::VisitElement(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    if (field.element.kind == ValueKind::Struct) {
        return VisitStruct(SchemaScope{nullptr, slot.structView}, slot.flags);
    }

    if (field.element.kind == ValueKind::InstancedStruct) {
        return VisitInstanced(slot.flags);
    }

    return VisitRead();
}

Result<void> Visitor::VisitArray(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    usize count = field.maxElements;
    if (field.value.kind == ValueKind::Array) {
        auto counted = PathCount(registry, access, root, path.View());
        if (!counted) {
            return Result<void>::Failure(counted.Error());
        }

        count = counted.Value();
    }

    // byte 배열은 Bytes 하나다(ExportJson의 Base64 문자열과 같은 단위).
    if (::rocks::detail::IsByteArrayField(field)) {
        Array<uint8> bytes(allocator);
        if (auto reserved = bytes.TryReserve(count); !reserved) {
            return reserved;
        }

        FieldValue value(allocator);
        for (usize index = 0; index < count; ++index) {
            path.SetIndex(static_cast<uint32>(index));
            auto read = PathRead(registry, access, root, path.View(), value);
            path.ClearSelector();
            if (!read) {
                return read;
            }

            // 자리는 위에서 잡았다.
            (void)bytes.TryPushBack(static_cast<uint8>(value.unsignedInteger));
        }

        return visitor.Bytes(bytes.View());
    }

    if (auto begun = visitor.BeginArray(count); !begun) {
        return begun;
    }

    for (usize index = 0; index < count; ++index) {
        path.SetIndex(static_cast<uint32>(index));
        auto visited = VisitElement(slot);
        if (!visited) {
            path.ClearSelector();
            return visited;
        }
    }

    path.ClearSelector();
    return visitor.EndArray();
}

Result<void> Visitor::VisitKeyed(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    Array<FieldValue> keys(allocator);
    if (auto read = PathReadKeys(registry, access, root, path.View(), keys); !read) {
        return read;
    }

    if (field.value.kind == ValueKind::Set) {
        if (auto begun = visitor.BeginSet(keys.Size()); !begun) {
            return begun;
        }

        for (const FieldValue& key : keys) {
            if (auto visited = visitor.Value(key); !visited) {
                return visited;
            }
        }

        return visitor.EndSet();
    }

    if (auto begun = visitor.BeginMap(keys.Size()); !begun) {
        return begun;
    }

    for (const FieldValue& key : keys) {
        if (auto keyed = visitor.MapKey(key); !keyed) {
            return keyed;
        }

        if (auto selected = path.SetKey(context.Options().names, allocator, key); !selected) {
            return selected;
        }

        auto visited = VisitElement(slot);
        path.ClearSelector();
        if (!visited) {
            return visited;
        }
    }

    return visitor.EndMap();
}

// 단일 값이다(unset Optional은 Absent).
Result<void> Visitor::VisitRead() noexcept {
    FieldValue value(allocator);
    if (auto read = PathRead(registry, access, root, path.View(), value); !read) {
        return read;
    }

    return value.present ? visitor.Value(value) : visitor.Absent();
}

} // namespace

Result<void> VisitValues(const AssignmentTarget& target, ValueVisitor& visitor,
                         const TextCodecOptions& options) noexcept {
    ObjectRegistry& registry = target.Registry();
    TextCodecOptions effective = options;
    if (!effective.names) {
        effective.names = registry.Names();
    } else if (effective.names != registry.Names()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = target.ValueAllocator();
    // 순회는 진단을 남기지 않는다 — 문맥이 요구하는 빈 모음이다.
    TextDiagnostics diagnostics(allocator);
    TextCodecContext context(registry.Types(), effective, diagnostics, allocator);
    if (auto started = context.Start(); !started) {
        return started;
    }

    const ObjectAccessContext& access = target.Access();
    PathTarget root = PathTarget(target.Handle());
    auto lease = detail::ReadTargetRoot(target, root);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    Visitor walker{context, registry, access, root, visitor, allocator, JsonPathBuilder(allocator)};
    const TypeLease& type = lease.Value();
    const SchemaScope scope = type.Kind() == TypeKind::Struct ? SchemaScope{nullptr, type.Struct()}
                                                              : SchemaScope{&type, StructView{}};
    return walker.VisitStruct(scope, 0);
}

} // namespace rocks::reflection_text
