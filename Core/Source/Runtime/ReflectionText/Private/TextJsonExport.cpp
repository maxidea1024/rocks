#include "TextJsonInternal.h"

#include <Core/Math/MathUtility.h>
#include <Object/ConstructionContext.h>

// JSON export다(리플렉션 B9i4 — ReflectionText/TextJson.h의 ExportJson). 대상의 스키마를 따라
// 경로를 만들며 단일 값은 PathRead, 배열은 PathCount와 원소 경로, Map·Set은 PathReadKeys와 key
// 경로로 읽어 쓴다.
namespace rocks::reflection_text {

namespace detail {

Result<void> JsonPathBuilder::SetKey(NamePool* names, Allocator& allocator,
                                     const FieldValue& key) noexcept {
    ClearSelector();
    Array<uint8> wire(allocator);
    if (auto made = AppendValueKeyWire(names, allocator, key, wire); !made) {
        return made;
    }

    if (auto reserved = keys_.TryReserve(keys_.Size() + 1); !reserved) {
        return reserved;
    }

    if (auto pushed = wires_.TryPushBack(Move(wire)); !pushed) {
        return pushed;
    }

    // 용량을 먼저 잡았으므로 실패하지 않는다. wire는 옮겨도 버퍼가 그대로라 view가 유효하다.
    const Array<uint8>& stored = wires_[wires_.Size() - 1];
    [[maybe_unused]] auto pushed = keys_.TryPushBack(PathKey{key.kind, stored.View()});
    PathStep& last = steps_[count_ - 1];
    last.selector = PathSelector::Key;
    last.index = static_cast<uint32>(keys_.Size() - 1);
    return Result<void>::Success();
}

Result<void> JsonPathBuilder::Assign(PropertyPath path, Allocator& allocator) noexcept {
    count_ = 0;
    keys_.Clear();
    wires_.Clear();
    if (path.steps.Size() > MaxPropertyPathSteps) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = keys_.TryReserve(path.keys.Size()); !reserved) {
        return reserved;
    }

    for (usize index = 0; index < path.keys.Size(); ++index) {
        Array<uint8> wire(allocator);
        Result<void> copied = wire.TryAppend(path.keys[index].wire);
        if (copied) {
            copied = wires_.TryPushBack(Move(wire));
        }

        if (!copied) {
            wires_.Clear();
            return copied;
        }
    }

    // wire는 옮겨도 버퍼가 그대로라 view가 유효하다(용량은 위에서 잡았다).
    for (usize index = 0; index < path.keys.Size(); ++index) {
        (void)keys_.TryPushBack(PathKey{path.keys[index].kind, wires_[index].View()});
    }

    for (usize index = 0; index < path.steps.Size(); ++index) {
        steps_[index] = path.steps[index];
    }

    count_ = path.steps.Size();
    return Result<void>::Success();
}

} // namespace detail

namespace {

using detail::FieldSlot;
using detail::JsonPathBuilder;
using detail::PartSlot;
using detail::SchemaScope;

StringView ToolName(const FieldSchema& field) noexcept {
    const Span<const char> name = EffectiveToolName(field);
    return StringView(name.Data(), name.Size());
}

// 대상 하나의 JSON 쓰기다.
struct Exporter {
    TextCodecContext& context;
    ObjectRegistry& registry;
    const ObjectAccessContext& access;
    PathTarget root;
    JsonWriter& writer;
    Allocator& allocator;
    JsonPathBuilder path;

    Result<void> WriteStruct(const SchemaScope& scope, uint32 flags) noexcept;
    Result<void> WriteInstanced(uint32 flags) noexcept;
    Result<void> WriteField(const SchemaScope& scope, usize index, uint32 flags) noexcept;
    Result<void> WriteValue(const TextSlot& slot) noexcept;
    Result<void> WriteArray(const TextSlot& slot) noexcept;
    Result<void> WriteKeyed(const TextSlot& slot) noexcept;
    Result<void> WriteRead(const TextScalarType& type) noexcept;
    Result<void> WriteScalar(const TextScalarType& type, const FieldValue& value) noexcept;
};

Result<void> Exporter::WriteStruct(const SchemaScope& scope, uint32 flags) noexcept {
    if (auto begun = writer.BeginObject(); !begun) {
        return begun;
    }

    const auto& fields = scope.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (auto written = WriteField(scope, index, flags); !written) {
            return written;
        }
    }

    return writer.EndObject();
}

// InstancedStruct 값(리플렉션 B9j — Docs/Design/ReflectionExtensions.ko.md 10.4절)은 비어 있으면
// null, 아니면 `{"_structType": "<타입 도구 이름>", 멤버...}`다(멤버는 지금 담긴 struct의 경로로
// 읽는다). 도구 이름이 없는 struct(수동 등록)는 Unsupported다.
Result<void> Exporter::WriteInstanced(uint32 flags) noexcept {
    auto type = PathReadInstancedType(registry, access, root, path.View());
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    if (!type.Value().IsValid()) {
        return writer.Null();
    }

    const Span<const char> name = type.Value().Schema().toolName;
    if (name.Size() == 0) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (auto begun = writer.BeginObject(); !begun) {
        return begun;
    }

    if (auto keyed = writer.Key(detail::InstancedTypeKey); !keyed) {
        return keyed;
    }

    if (auto named = writer.String(StringView(name.Data(), name.Size())); !named) {
        return named;
    }

    const SchemaScope scope{nullptr, type.Value().Struct()};
    const auto& fields = scope.Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (auto written = WriteField(scope, index, flags); !written) {
            return written;
        }
    }

    return writer.EndObject();
}

Result<void> Exporter::WriteField(const SchemaScope& scope, usize index, uint32 flags) noexcept {
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

    Result<void> written = writer.Key(ToolName(*slot.field));
    if (written) {
        written = WriteValue(slot);
    }

    path.Pop();
    return written;
}

Result<void> Exporter::WriteValue(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    switch (field.value.kind) {
    case ValueKind::Struct:
        return WriteStruct(SchemaScope{nullptr, slot.structView}, slot.flags);
    case ValueKind::InstancedStruct:
        return WriteInstanced(slot.flags);
    case ValueKind::Optional:
        if (field.element.kind == ValueKind::Struct) {
            auto set = PathIsOptionalSet(registry, access, root, path.View());
            if (!set) {
                return Result<void>::Failure(set.Error());
            }

            return set.Value() ? WriteStruct(SchemaScope{nullptr, slot.structView}, slot.flags)
                               : writer.Null();
        }

        return WriteRead(SlotValueType(slot));
    case ValueKind::Array:
    case ValueKind::StaticArray:
        return WriteArray(slot);
    case ValueKind::Map:
    case ValueKind::Set:
        return WriteKeyed(slot);
    default:
        return WriteRead(SlotValueType(slot));
    }
}

Result<void> Exporter::WriteArray(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    usize count = field.maxElements;
    if (field.value.kind == ValueKind::Array) {
        auto counted = PathCount(registry, access, root, path.View());
        if (!counted) {
            return Result<void>::Failure(counted.Error());
        }

        count = counted.Value();
    }

    if (auto begun = writer.BeginArray(); !begun) {
        return begun;
    }

    const bool structElement = field.element.kind == ValueKind::Struct;
    for (usize index = 0; index < count; ++index) {
        path.SetIndex(static_cast<uint32>(index));
        auto written =
            structElement ? WriteStruct(SchemaScope{nullptr, slot.structView}, slot.flags)
            : field.element.kind == ValueKind::InstancedStruct ? WriteInstanced(slot.flags)
                                                               : WriteRead(SlotValueType(slot));
        if (!written) {
            path.ClearSelector();
            return written;
        }
    }

    path.ClearSelector();
    return writer.EndArray();
}

Result<void> Exporter::WriteKeyed(const TextSlot& slot) noexcept {
    const FieldSchema& field = *slot.field;
    Array<FieldValue> keys(allocator);
    if (auto read = PathReadKeys(registry, access, root, path.View(), keys); !read) {
        return read;
    }

    const bool set = field.value.kind == ValueKind::Set;
    const TextScalarType keyType = SlotKeyType(slot);
    if (set) {
        if (auto begun = writer.BeginArray(); !begun) {
            return begun;
        }

        for (usize index = 0; index < keys.Size(); ++index) {
            if (auto written = WriteScalar(keyType, keys[index]); !written) {
                return written;
            }
        }

        return writer.EndArray();
    }

    if (auto begun = writer.BeginObject(); !begun) {
        return begun;
    }

    const bool structValue = field.element.kind == ValueKind::Struct;
    StringBuilder<128> text(allocator);
    for (usize index = 0; index < keys.Size(); ++index) {
        text.Clear();
        if (auto formatted = FormatTextScalar(context, keyType, keys[index], text); !formatted) {
            return formatted;
        }

        if (auto keyed = writer.Key(text.View()); !keyed) {
            return keyed;
        }

        if (auto selected = path.SetKey(context.Options().names, allocator, keys[index]);
            !selected) {
            return selected;
        }

        auto written = structValue ? WriteStruct(SchemaScope{nullptr, slot.structView}, slot.flags)
                       : field.element.kind == ValueKind::InstancedStruct
                           ? WriteInstanced(slot.flags)
                           : WriteRead(SlotValueType(slot));
        path.ClearSelector();
        if (!written) {
            return written;
        }
    }

    return writer.EndObject();
}

Result<void> Exporter::WriteRead(const TextScalarType& type) noexcept {
    FieldValue value(allocator);
    if (auto read = PathRead(registry, access, root, path.View(), value); !read) {
        return read;
    }

    // unset Optional은 null이다.
    if (!value.present) {
        return writer.Null();
    }

    return WriteScalar(type, value);
}

Result<void> Exporter::WriteScalar(const TextScalarType& type, const FieldValue& value) noexcept {
    switch (value.kind) {
    case ValueKind::Bool:
        return writer.Bool(value.boolean);
    case ValueKind::Int8:
    case ValueKind::Int16:
    case ValueKind::Int32:
    case ValueKind::Int64:
        return writer.Int64(value.signedInteger);
    case ValueKind::UInt8:
    case ValueKind::UInt16:
    case ValueKind::UInt32:
    case ValueKind::UInt64:
        return writer.UInt64(value.unsignedInteger);
    case ValueKind::Float32:
        if (math::IsFinite(value.float32)) {
            return writer.Float(value.float32);
        }

        break;
    case ValueKind::Float64:
        if (math::IsFinite(value.float64)) {
            return writer.Double(value.float64);
        }

        break;
    case ValueKind::ObjectReference:
    case ValueKind::WeakObjectReference:
        if (value.reference.IsNull()) {
            return writer.Null();
        }

        break;
    case ValueKind::SoftObjectReference:
        if (value.guid == Guid{} && value.string.View().IsEmpty()) {
            return writer.Null();
        }

        break;
    case ValueKind::ClassReference:
        if (value.guid == Guid{}) {
            return writer.Null();
        }

        break;
    default:
        break;
    }

    StringBuilder<128> text(allocator);
    if (auto formatted = FormatTextScalar(context, type, value, text); !formatted) {
        return formatted;
    }

    const bool flags = value.kind == ValueKind::Enum && type.enumSchema &&
                       HasFlag(type.enumSchema->flags, EnumFlags::Flags);
    if (!flags) {
        return writer.String(text.View());
    }

    // Flags enum은 이름 배열이다(명령행 표기 `A+B`를 나눈다 — 0은 빈 배열).
    if (auto begun = writer.BeginArray(); !begun) {
        return begun;
    }

    const StringView joined = text.View();
    usize start = 0;
    for (usize at = 0; at <= joined.ByteCount() && !joined.IsEmpty(); ++at) {
        if (at != joined.ByteCount() && joined.Data()[at] != '+') {
            continue;
        }

        if (auto written = writer.String(StringView(joined.Data() + start, at - start)); !written) {
            return written;
        }

        start = at + 1;
    }

    return writer.EndArray();
}

} // namespace

Result<void> ExportJson(const AssignmentTarget& target, JsonWriter& writer,
                        const TextCodecOptions& options) noexcept {
    ObjectRegistry& registry = target.Registry();
    TextCodecOptions effective = options;
    if (!effective.names) {
        effective.names = registry.Names();
    } else if (effective.names != registry.Names()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    Allocator& allocator = target.ValueAllocator();
    // export는 진단을 남기지 않는다(값 쓰기 실패는 호출 오류다) — 문맥이 요구하는 빈 모음이다.
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

    Exporter exporter{
        context, registry, access, root, writer, allocator, JsonPathBuilder(allocator)};
    const TypeLease& type = lease.Value();
    const SchemaScope scope = type.Kind() == TypeKind::Struct ? SchemaScope{nullptr, type.Struct()}
                                                              : SchemaScope{&type, StructView{}};
    return exporter.WriteStruct(scope, 0);
}

} // namespace rocks::reflection_text
