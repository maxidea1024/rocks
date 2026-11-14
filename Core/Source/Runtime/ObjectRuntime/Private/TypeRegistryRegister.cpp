#include "TypeRegistryInternal.h"

#include <Object/TypeRegistry.h>
#include <Core/Algorithms.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <new>

// TypeRegistry 등록이다. 클래스·enum·struct 서술 검사 도우미와 Register·RegisterEnum·
// RegisterStruct, 필드 값 타입 레코드 해석(ResolveValueTypes)을 둔다.
namespace rocks {

namespace {

bool AddBytes(usize& total, usize count) noexcept {
    if (count > MaxSize - total) {
        return false;
    }

    total += count;
    return true;
}

template <class T>
Span<const T> CopyBytes(Span<const T> source, uint8*& cursor) noexcept {
    T* destination = reinterpret_cast<T*>(cursor);
    for (usize index = 0; index < source.Size(); ++index) {
        destination[index] = source[index];
    }

    cursor += source.Size() * sizeof(T);
    return {destination, source.Size()};
}

constexpr uint32 KnownTypeFlags =
    static_cast<uint32>(TypeFlags::Abstract) | static_cast<uint32>(TypeFlags::WorldOnly);

constexpr uint32 KnownFieldFlags =
    static_cast<uint32>(FieldFlags::Transient) | static_cast<uint32>(FieldFlags::ReadOnly) |
    static_cast<uint32>(FieldFlags::EditorOnly) | static_cast<uint32>(FieldFlags::Required);

bool IsAsciiIdentifier(Span<const char> text) noexcept {
    if (text.Size() == 0) {
        return false;
    }

    for (usize index = 0; index < text.Size(); ++index) {
        const char c = text[index];
        const bool letter = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
        if (!letter && (index == 0 || c < '0' || c > '9')) {
            return false;
        }
    }

    return true;
}

bool SameBytes(Span<const char> left, Span<const char> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }

    return true;
}

// 등록할 수 있는 값 종류다(Opaque부터 Digest256까지 — Array는 B5에서 열었다).
bool SupportedValueKind(ValueKind kind) noexcept {
    return static_cast<uint8>(kind) <= static_cast<uint8>(ValueKind::Digest256);
}

// 배열 원소가 될 수 있는 값 종류다(Opaque·Array 거부 — 중첩 배열은 MetaCompiler RMC2304).
bool SupportedElementKind(ValueKind kind) noexcept {
    return kind != ValueKind::Opaque && kind != ValueKind::Array && SupportedValueKind(kind);
}

// enum 도구 이름·접두의 문자 집합이다: [A-Za-z0-9_]+ (숫자로 시작할 수 있다).
bool IsEnumToolName(Span<const char> text) noexcept {
    if (text.Size() == 0) {
        return false;
    }

    for (usize index = 0; index < text.Size(); ++index) {
        const char c = text[index];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '_')) {
            return false;
        }
    }

    return true;
}

// 접두 유무를 합친 두 도구 이름 집합이 대소문자 무시로 겹치는지다(a, prefix+a)와(b, prefix+b).
bool EnumToolNamesCollide(Span<const char> left, Span<const char> right,
                          Span<const char> prefix) noexcept {
    if (EqualsIgnoringAsciiCase(left, right)) {
        return true;
    }

    // prefix+left == right 또는 left == prefix+right
    const auto prefixed = [&prefix](Span<const char> shorter, Span<const char> longer) noexcept {
        if (prefix.Size() == 0 || longer.Size() != prefix.Size() + shorter.Size()) {
            return false;
        }

        for (usize index = 0; index < prefix.Size(); ++index) {
            if (detail::AsciiLower(prefix[index]) != detail::AsciiLower(longer[index])) {
                return false;
            }
        }

        return detail::EnumNameEquals(shorter, longer, prefix.Size());
    };
    return prefixed(left, right) || prefixed(right, left);
}

constexpr usize MaxEnumerators = 4096;

// 클래스·struct가 함께 쓰는 필드 서술 검사다(Docs/Design/ReflectionFoundations.ko.md 4.6절). 필드
// 하나의 규칙(플래그·값 종류·Required·도구 이름)과 필드 사이 규칙(같은 ID·실효 도구 이름은
// AlreadyExists)을 본다. typedFields에 Opaque가 아닌 필드 수를 쓴다.
Result<void> ValidateFieldList(Span<const FieldSchema> fields, usize& typedFields) noexcept {
    typedFields = 0;
    if (fields.Size() > 4096) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < fields.Size(); ++index) {
        const auto& field = fields[index];
        // 대상 형식은 배열이면 원소, 아니면 값이다(Docs/Design/ReflectionFoundations.ko.md 5.6절).
        // Array 필드는 value.type 0, 원소 종류는 단일 값 종류(Opaque·Array 거부), maxElements는
        // [1, MaxArrayElements]이고, 배열이 아닌 필드는 element = {Opaque, 0}·maxElements 0이다.
        const bool array = field.value.kind == ValueKind::Array;
        const ValueType& target = FieldTargetType(field);
        const bool arrayShape =
            array ? field.value.type == TypeId{} && SupportedElementKind(field.element.kind) &&
                        field.maxElements >= 1 && field.maxElements <= MaxArrayElements
                  : field.element.kind == ValueKind::Opaque && field.element.type == TypeId{} &&
                        field.maxElements == 0;
        // Required는 typed 참조 필드(배열이면 원소)에만 뜻이 있다(저장이 null을 검사하려면
        // 접근기·codec이 있어야 한다). 도구 이름은 비어 있거나 ASCII 식별자다.
        if (field.id == Guid{} || (field.flags & ~KnownFieldFlags) != 0 ||
            (HasFlag(field.flags, FieldFlags::Required) &&
             target.kind != ValueKind::ObjectReference &&
             target.kind != ValueKind::SoftObjectReference) ||
            (field.kind != FieldKind::Value && field.kind != FieldKind::ObjectReference) ||
            !SupportedValueKind(field.value.kind) || !arrayShape ||
            !StringView(field.name.Data(), field.name.Size()).ValidateUtf8() ||
            !StringView(field.category.Data(), field.category.Size()).ValidateUtf8() ||
            !StringView(field.displayName.Data(), field.displayName.Size()).ValidateUtf8() ||
            (field.toolName.Size() != 0 && !IsAsciiIdentifier(field.toolName))) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // FieldKind::ObjectReference(강한 GC 간선) ⇔ 대상 형식이 ObjectReference(단일 참조 또는
        // ObjectRefArray). 대상 type은 참조·소프트 참조의 기대 대상과 Enum·Struct의 대상 ID에만
        // 쓴다. Enum·Struct는 ID가 필요하다.
        if (field.value.kind != ValueKind::Opaque) {
            ++typedFields;
            const bool reference = target.kind == ValueKind::ObjectReference;
            const bool valueType =
                target.kind == ValueKind::Enum || target.kind == ValueKind::Struct;
            const bool typed =
                reference || valueType || target.kind == ValueKind::SoftObjectReference;
            if (field.name.Size() == 0 || (field.kind == FieldKind::ObjectReference) != reference ||
                (!typed && target.type != TypeId{}) || (valueType && target.type == TypeId{})) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        } else if (field.value.type != TypeId{}) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize previous = 0; previous < index; ++previous) {
            if (field.id == fields[previous].id) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }

            // 실효 도구 이름(상속 필드 포함)은 대소문자 무시로 유일하다. 이름 없는 Opaque 필드는
            // 제외한다.
            const Span<const char> name = EffectiveToolName(field);
            if (name.Size() != 0 &&
                EqualsIgnoringAsciiCase(name, EffectiveToolName(fields[previous]))) {
                return Result<void>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }

    return Result<void>::Success();
}

// 대상 형식(배열이면 원소)이 Enum·Struct인 필드만 값의 native 키를 싣는다.
bool CarriesValueNativeType(const FieldSchema& field) noexcept {
    const ValueKind kind = FieldTargetType(field).kind;
    return kind == ValueKind::Enum || kind == ValueKind::Struct;
}

// FieldValue로 통째 읽고 쓰지 않는 필드다(struct 값·배열 — read·write·writeStaged가 없어야 하고
// Transient가 아니면 codec이 필수다).
bool IsAggregateField(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Struct || field.value.kind == ValueKind::Array;
}

// 클래스 함수표에서 필드 ID의 valueNativeType을 찾는다(ResolveValueTypes의 thunkType).
const void* ClassThunkType(const void* context, Guid fieldId) noexcept {
    const auto& fields = *static_cast<const Span<const RuntimeFieldOps>*>(context);
    for (const RuntimeFieldOps& field : fields) {
        if (field.fieldId == fieldId) {
            return field.valueNativeType;
        }
    }

    return nullptr;
}

// struct 함수표에서 필드 ID의 valueNativeType을 찾는다.
const void* StructThunkType(const void* context, Guid fieldId) noexcept {
    const auto& fields = *static_cast<const Span<const RuntimeStructFieldOps>*>(context);
    for (const RuntimeStructFieldOps& field : fields) {
        if (field.fieldId == fieldId) {
            return field.valueNativeType;
        }
    }

    return nullptr;
}

} // namespace

namespace {

// 레코드 블록 할당 태그다('TYPE').
constexpr uint32 RecordTag = 0x54595045u;

} // namespace

Result<TypeRegistrationToken> TypeRegistry::Register(const TypeSchema& schema,
                                                     RuntimeTypeOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }

    if (schema.id == Guid{} || schema.version == 0 || schema.fields.Size() > 4096 ||
        ops.fields.Size() > schema.fields.Size() || schema.baseType == schema.id ||
        (schema.flags & ~KnownTypeFlags) != 0 ||
        (HasFlag(schema.flags, TypeFlags::Abstract) != (ops.construct == nullptr)) ||
        !ops.destroy || !ops.trace || !ops.nativeType || ops.size < sizeof(Object) ||
        ops.alignment < alignof(Object) || (ops.alignment & (ops.alignment - 1)) != 0 ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8()) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize typedFields = 0;
    if (auto validated = ValidateFieldList(schema.fields, typedFields); !validated) {
        return Result<TypeRegistrationToken>::Failure(validated.Error());
    }

    if (typedFields != ops.fields.Size() ||
        (ops.completeFieldLayout && typedFields != schema.fields.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    // 완전한 필드 배치를 선언한 타입은 그래프에 저장할 수 있어야 하므로 Transient가 아닌 모든
    // 필드에 codec(encode·decode)이 있어야 한다(Docs/Design/ReflectionFoundations.ko.md 6.4절).
    if (ops.completeFieldLayout) {
        for (usize schemaIndex = 0; schemaIndex < schema.fields.Size(); ++schemaIndex) {
            const auto& schemaField = schema.fields[schemaIndex];
            if (HasFlag(schemaField.flags, FieldFlags::Transient)) {
                continue;
            }

            bool coded = false;
            for (usize opsIndex = 0; opsIndex < ops.fields.Size(); ++opsIndex) {
                if (ops.fields[opsIndex].fieldId == schemaField.id) {
                    coded = ops.fields[opsIndex].encode != nullptr;
                    break;
                }
            }

            if (!coded) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    for (usize index = 0; index < ops.fields.Size(); ++index) {
        const auto& field = ops.fields[index];
        const FieldSchema* matched = nullptr;
        for (usize schemaIndex = 0; schemaIndex < schema.fields.Size(); ++schemaIndex) {
            const auto& schemaField = schema.fields[schemaIndex];
            if (schemaField.id == field.fieldId && schemaField.value.kind != ValueKind::Opaque &&
                CarriesValueNativeType(schemaField) == (field.valueNativeType != nullptr)) {
                matched = &schemaField;
                break;
            }
        }

        // struct·배열 필드에는 read·write·writeStaged·readStaged가 없어야 하고(FieldValue로 통째
        // 읽고 쓰지 않는다 — 멤버·원소 단위는 B6 경로 접근) 그 밖의 필드는 read·write·writeStaged가
        // 다 있어야 한다(readStaged는 선택 — Required 실효 값 검사용).
        const bool aggregateField = matched && IsAggregateField(*matched);
        const bool accessors = aggregateField ? (!field.read && !field.write &&
                                                 !field.writeStaged && !field.readStaged)
                                              : (field.read && field.write && field.writeStaged);
        // struct·배열 필드는 Transient가 아니면 codec이 있어야 한다(그 밖은 completeFieldLayout
        // 규칙). 경로 접근 thunk(path)는 struct·배열 필드만 가질 수 있다 — 최상위 단일 값의 경로
        // 읽기·쓰기는 read·write 한 경로다(7.1절).
        if (!matched || !accessors || (field.encode == nullptr) != (field.decode == nullptr) ||
            (aggregateField && !field.encode && !HasFlag(matched->flags, FieldFlags::Transient)) ||
            (!aggregateField && field.path != nullptr)) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize previous = 0; previous < index; ++previous) {
            if (field.fieldId == ops.fields[previous].fieldId) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }

    // descriptor와 함수표 자체도 한 블록에 복사한다. 입력 배열/scratch 수명을 빌리지 않는다.
    constexpr usize fieldOffset =
        (sizeof(Record) + alignof(FieldSchema) - 1) & ~(alignof(FieldSchema) - 1);
    const usize fieldEnd = fieldOffset + schema.fields.Size() * sizeof(FieldSchema);
    const usize opsOffset =
        (fieldEnd + alignof(RuntimeFieldOps) - 1) & ~(alignof(RuntimeFieldOps) - 1);
    const usize opsEnd = opsOffset + ops.fields.Size() * sizeof(RuntimeFieldOps);
    const usize typesOffset = (opsEnd + alignof(Record*) - 1) & ~(alignof(Record*) - 1);
    const usize typesEnd = typesOffset + schema.fields.Size() * sizeof(Record*);
    // 필드 ID 정렬 색인과 스키마 위치 → 함수표 위치 색인(경로 접근의 FieldIndex·FieldOps).
    const usize orderOffset = (typesEnd + alignof(uint32) - 1) & ~(alignof(uint32) - 1);
    const usize opsIndexOffset = orderOffset + schema.fields.Size() * sizeof(uint32);
    usize bytes = opsIndexOffset + schema.fields.Size() * sizeof(uint32);
    if (!AddBytes(bytes, schema.name.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }

    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        if (!AddBytes(bytes, field.name.Size()) || !AddBytes(bytes, field.attributes.Size()) ||
            !AddBytes(bytes, field.category.Size()) || !AddBytes(bytes, field.displayName.Size()) ||
            !AddBytes(bytes, field.toolName.Size())) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
        }
    }

    Record* base = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (closed_ || mutating_ || nextGeneration_ == 0) {
            return Result<TypeRegistrationToken>::Failure(closed_     ? ErrorCode::ShuttingDown
                                                          : mutating_ ? ErrorCode::Busy
                                                                      : ErrorCode::Overflow);
        }

        if (FindLocked(schema.id) != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
        }

        if (schema.baseType != TypeId{}) {
            base = FindLocked(schema.baseType);
            // base는 클래스여야 한다(Acquire의 종류 규칙과 같다).
            if (base != nullptr && base->kind != TypeKind::Class) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
            }
        }

        // Enum·Struct 필드가 가리키는 레코드는 먼저 등록되어 있어야 하고, 정적 thunk의 C++ 타입과
        // 같은 native 타입이어야 한다. WorldOnly가 아닌 클래스는 WorldOnly 소유자를 요구하는
        // struct를 필드로 둘 수 없다.
        if (auto resolved = ResolveValueTypes(schema.fields, &ClassThunkType, &ops.fields,
                                              HasFlag(schema.flags, TypeFlags::WorldOnly), nullptr);
            !resolved) {
            return Result<TypeRegistrationToken>::Failure(resolved.Error());
        }

        if (schema.baseType != TypeId{}) {
            if (!base) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::NotFound);
            }

            const bool dependentsFull = base->dependents == NumericLimits<uint32>::Max();
            if (ops.nativeBaseType != base->ops.nativeType || dependentsFull) {
                return Result<TypeRegistrationToken>::Failure(
                    dependentsFull ? ErrorCode::Overflow : ErrorCode::TypeMismatch);
            }

            // nativeBaseType은 Resolve<기반>의 포인터 변환 근거다. C++ 상속 자체는 등록 코드의
            // static_assert(__is_base_of)가 보증하고, 여기서는 명백히 잘못된 수동 함수표(기반보다
            // 작거나 정렬이 약한 파생)를 거부한다.
            if (ops.size < base->ops.size || ops.alignment < base->ops.alignment) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
            }

            // WorldOnly는 상속되는 플래그다. 수동 등록이 파생에서 빠뜨려도 받아들이지 않는다.
            if (HasFlag(base->schema.flags, TypeFlags::WorldOnly) &&
                !HasFlag(schema.flags, TypeFlags::WorldOnly)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
            }

            // 파생 스키마는 base field를 동일한 안정 ID/표현으로 포함한다. 그래서 기반 타입의 필드
            // 함수(Resolve<기반> 경유)를 파생 객체에 써도 같은 필드를 가리킨다.
            for (usize baseIndex = 0; baseIndex < base->schema.fields.Size(); ++baseIndex) {
                const auto& inherited = base->schema.fields[baseIndex];
                bool matched = false;
                for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
                    const auto& field = schema.fields[fieldIndex];
                    if (field.id == inherited.id && field.kind == inherited.kind &&
                        field.value.kind == inherited.value.kind &&
                        field.value.type == inherited.value.type &&
                        field.element.kind == inherited.element.kind &&
                        field.element.type == inherited.element.type &&
                        field.maxElements == inherited.maxElements &&
                        field.flags == inherited.flags &&
                        SameBytes(EffectiveToolName(field), EffectiveToolName(inherited))) {
                        matched = true;
                        break;
                    }
                }

                if (!matched) {
                    return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
                }
            }
        } else if (ops.nativeBaseType != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        mutating_ = true;
    }

    // 색인 공간은 레코드보다 먼저 준비한다(실패하면 아무것도 바꾸지 않는다).
    IndexGrowth growth(allocator_);
    if (auto prepared = PrepareIndexes(growth, false); !prepared) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(prepared.Error());
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), RecordTag);
    if (!memory) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(memory.Error());
    }

    if (!memory.Value()) {
        platform::Fatal("Allocator returned null TypeRegistry record");
    }

    Record* record = ::new (memory.Value()) Record();
    auto* fields =
        reinterpret_cast<FieldSchema*>(static_cast<uint8*>(memory.Value()) + fieldOffset);
    auto* fieldOps =
        reinterpret_cast<RuntimeFieldOps*>(static_cast<uint8*>(memory.Value()) + opsOffset);
    auto* fieldTypes =
        reinterpret_cast<Record**>(static_cast<uint8*>(memory.Value()) + typesOffset);
    auto* fieldOrder = reinterpret_cast<uint32*>(static_cast<uint8*>(memory.Value()) + orderOffset);
    auto* opsIndex =
        reinterpret_cast<uint32*>(static_cast<uint8*>(memory.Value()) + opsIndexOffset);
    uint8* cursor = reinterpret_cast<uint8*>(opsIndex + schema.fields.Size());
    {
        // mutating_이 등록·해제를 막으므로 위에서 확인한 enum·struct 레코드가 그대로 있다.
        platform::LockGuard lock(gate_);
        Span<const RuntimeFieldOps> thunks = ops.fields;
        if (!ResolveValueTypes(schema.fields, &ClassThunkType, &thunks,
                               HasFlag(schema.flags, TypeFlags::WorldOnly), fieldTypes)) {
            platform::Fatal("TypeRegistry value type dependency vanished during registration");
        }
    }

    record->fieldTypes = fieldTypes;
    record->schema.id = schema.id;
    record->schema.version = schema.version;
    record->schema.baseType = schema.baseType;
    record->schema.flags = schema.flags;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->schema.fields = {fields, schema.fields.Size()};
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        ::new (fields + index) FieldSchema(schema.fields[index]);
        fields[index].name = CopyBytes(schema.fields[index].name, cursor);
        fields[index].attributes = CopyBytes(schema.fields[index].attributes, cursor);
        fields[index].category = CopyBytes(schema.fields[index].category, cursor);
        fields[index].displayName = CopyBytes(schema.fields[index].displayName, cursor);
        fields[index].toolName = CopyBytes(schema.fields[index].toolName, cursor);
        fieldOrder[index] = static_cast<uint32>(index);
        opsIndex[index] = Record::NoOps;
    }

    algo::Sort(Span<uint32>(fieldOrder, schema.fields.Size()),
               [fields](uint32 left, uint32 right) noexcept {
                   const Guid a = fields[left].id;
                   const Guid b = fields[right].id;
                   return a.high != b.high ? a.high < b.high : a.low < b.low;
               });
    record->fieldOrder = fieldOrder;
    record->opsIndex = opsIndex;
    for (usize index = 0; index < ops.fields.Size(); ++index) {
        ::new (fieldOps + index) RuntimeFieldOps(ops.fields[index]);
        for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
            if (fields[fieldIndex].id == fieldOps[index].fieldId) {
                opsIndex[fieldIndex] = static_cast<uint32>(index);
                // 원본 입력 함수표는 변경하지 않는다. 등록된 복사본에만 정책을 결합한다.
                fieldOps[index].write.Bind(*this, ops.nativeType, fields[fieldIndex].id,
                                           fields[fieldIndex].value.kind,
                                           fields[fieldIndex].value.type,
                                           HasFlag(fields[fieldIndex].flags, FieldFlags::ReadOnly));
                break;
            }
        }
    }

    record->ops = Move(ops);
    record->ops.fields = {fieldOps, record->ops.fields.Size()};
    record->base = base;

    platform::LockGuard lock(gate_);
    PublishLocked(record, growth);
    if (base) {
        ++base->dependents;
    }

    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        if (fieldTypes[fieldIndex]) {
            ++fieldTypes[fieldIndex]->dependents;
        }
    }

    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

Result<TypeRegistrationToken> TypeRegistry::RegisterEnum(const EnumSchema& schema,
                                                         RuntimeEnumOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }

    int64 minimum = 0;
    int64 maximum = 0;
    const bool flags = HasFlag(schema.flags, EnumFlags::Flags);
    if (schema.id == Guid{} || schema.name.Size() == 0 ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8() ||
        !EnumUnderlyingRange(schema.underlying, minimum, maximum) ||
        (schema.flags & ~static_cast<uint32>(EnumFlags::Flags)) != 0 || schema.values.Size() == 0 ||
        schema.values.Size() > MaxEnumerators ||
        (schema.toolPrefix.Size() != 0 && !IsEnumToolName(schema.toolPrefix)) ||
        ops.nativeType == nullptr) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize bytes =
        (sizeof(Record) + alignof(EnumeratorSchema) - 1) & ~(alignof(EnumeratorSchema) - 1);
    const usize valuesOffset = bytes;
    bytes += schema.values.Size() * sizeof(EnumeratorSchema);
    if (!AddBytes(bytes, schema.name.Size()) || !AddBytes(bytes, schema.toolPrefix.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }

    for (usize index = 0; index < schema.values.Size(); ++index) {
        const EnumeratorSchema& value = schema.values[index];
        if (!IsAsciiIdentifier(value.name) ||
            (value.toolName.Size() != 0 && !IsEnumToolName(value.toolName)) ||
            !StringView(value.displayName.Data(), value.displayName.Size()).ValidateUtf8() ||
            (value.flags & ~static_cast<uint32>(EnumeratorFlags::Hidden)) != 0 ||
            value.value < minimum || value.value > maximum || (flags && value.value < 0)) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        // 값 별칭과 (접두 유무를 합친) 도구 이름 중복은 받지 않는다.
        for (usize previous = 0; previous < index; ++previous) {
            const EnumeratorSchema& other = schema.values[previous];
            if (other.value == value.value ||
                EnumToolNamesCollide(EffectiveToolName(value), EffectiveToolName(other),
                                     schema.toolPrefix)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }

        if (!AddBytes(bytes, value.name.Size()) || !AddBytes(bytes, value.toolName.Size()) ||
            !AddBytes(bytes, value.displayName.Size())) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
        }
    }

    {
        platform::LockGuard lock(gate_);
        if (closed_ || mutating_ || nextGeneration_ == 0) {
            return Result<TypeRegistrationToken>::Failure(closed_     ? ErrorCode::ShuttingDown
                                                          : mutating_ ? ErrorCode::Busy
                                                                      : ErrorCode::Overflow);
        }

        // 같은 C++ enum을 다른 ID로 다시 등록하지 않는다. 생성 thunk는 GeneratedType<E>::Id()
        // 하나만 받으므로 별칭 ID를 가리키는 필드는 쓸 수도 저장할 수도 없게 된다.
        if (FindLocked(schema.id) != nullptr ||
            FindNativeLocked(ops.nativeType, TypeKind::Enum) != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
        }

        mutating_ = true;
    }

    // 색인 공간은 레코드보다 먼저 준비한다(실패하면 아무것도 바꾸지 않는다).
    IndexGrowth growth(allocator_);
    if (auto prepared = PrepareIndexes(growth, true); !prepared) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(prepared.Error());
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), RecordTag);
    if (!memory) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(memory.Error());
    }

    if (!memory.Value()) {
        platform::Fatal("Allocator returned null TypeRegistry record");
    }

    Record* record = ::new (memory.Value()) Record();
    auto* values =
        reinterpret_cast<EnumeratorSchema*>(static_cast<uint8*>(memory.Value()) + valuesOffset);
    uint8* cursor = reinterpret_cast<uint8*>(values + schema.values.Size());
    record->kind = TypeKind::Enum;
    record->schema.id = schema.id;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->enumSchema.id = schema.id;
    record->enumSchema.name = record->schema.name;
    record->enumSchema.underlying = schema.underlying;
    record->enumSchema.flags = schema.flags;
    record->enumSchema.toolPrefix = CopyBytes(schema.toolPrefix, cursor);
    for (usize index = 0; index < schema.values.Size(); ++index) {
        ::new (values + index) EnumeratorSchema(schema.values[index]);
        values[index].name = CopyBytes(schema.values[index].name, cursor);
        values[index].toolName = CopyBytes(schema.values[index].toolName, cursor);
        values[index].displayName = CopyBytes(schema.values[index].displayName, cursor);
    }

    record->enumSchema.values = {values, schema.values.Size()};
    record->ops.nativeType = ops.nativeType;
    record->ops.module = Move(ops.module);

    platform::LockGuard lock(gate_);
    PublishLocked(record, growth);
    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

Result<void> TypeRegistry::ResolveValueTypes(Span<const FieldSchema> fields,
                                             const void* (*thunkType)(const void*, Guid),
                                             const void* context, bool ownerWorldOnly,
                                             Record** targets) noexcept {
    for (usize index = 0; index < fields.Size(); ++index) {
        const auto& field = fields[index];
        if (targets) {
            targets[index] = nullptr;
        }

        // 대상 형식(배열이면 원소)이 Enum·Struct인 필드만 레코드를 가리킨다.
        const ValueType& value = FieldTargetType(field);
        const bool enumeration = value.kind == ValueKind::Enum;
        if (!enumeration && value.kind != ValueKind::Struct) {
            continue;
        }

        Record* target = FindLocked(value.type);

        if (!target) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        // 스키마로 해석한 대상과 정적 thunk의 C++ 타입이 어긋나지 않게 한다.
        const TypeKind expected = enumeration ? TypeKind::Enum : TypeKind::Struct;
        if (target->kind != expected || thunkType(context, field.id) != target->ops.nativeType) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (!ownerWorldOnly && target->requiresWorldOnlyOwner) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 참조 struct 원소 배열(StructRefArray)의 원소는 Registry 컨테이너를 담지 않는다(원소
        // 블록이 owner 저장소 밖이라 Registry가 그 컨테이너의 변경을 거부한다 — 5.6절 결정 15).
        if (field.value.kind == ValueKind::Array && target->containsStrongReference &&
            target->containsReferenceArray) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (target->dependents >= NumericLimits<uint32>::Max() - fields.Size()) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        if (targets) {
            targets[index] = target;
        }
    }

    return Result<void>::Success();
}

Result<TypeRegistrationToken> TypeRegistry::RegisterStruct(const TypeSchema& schema,
                                                           RuntimeStructOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }

    if (schema.id == Guid{} || schema.version == 0 || schema.baseType != TypeId{} ||
        schema.flags != 0 || ops.size == 0 || ops.alignment == 0 ||
        (ops.alignment & (ops.alignment - 1)) != 0 || !ops.nativeType || !ops.construct ||
        !ops.destroy || ops.fields.Size() != schema.fields.Size() ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8()) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize typedFields = 0;
    if (auto validated = ValidateFieldList(schema.fields, typedFields); !validated) {
        return Result<TypeRegistrationToken>::Failure(validated.Error());
    }

    // struct는 부분 metadata를 허용하지 않는다: 모든 필드가 typed이고 함수표는 스키마와 1:1이며
    // Transient가 아닌 필드는 codec을 가진다.
    if (typedFields != schema.fields.Size()) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    bool strongReference = false;
    bool required = false;
    bool referenceArray = false;
    for (const FieldSchema& field : schema.fields) {
        // 강한 참조 멤버: 단일 참조와 ObjectRefArray(대상 형식이 ObjectReference).
        const bool referenceTarget = FieldTargetType(field).kind == ValueKind::ObjectReference;
        strongReference = strongReference || referenceTarget;
        referenceArray =
            referenceArray || (field.value.kind == ValueKind::Array && referenceTarget);
        // Transient 멤버는 저장·복원하지 않으므로 Required 실효 값 검사 대상이 아니다. 배열 멤버의
        // Required(원소마다 non-null)도 센다 — 검사는 누락 여부와 무관하게 실효 값을 본다
        // (Docs/Design/ObjectFoundations.ko.md 4절).
        required = required || (HasFlag(field.flags, FieldFlags::Required) &&
                                !HasFlag(field.flags, FieldFlags::Transient));
        const RuntimeStructFieldOps* match = nullptr;
        usize found = 0;
        for (const RuntimeStructFieldOps& candidate : ops.fields) {
            if (candidate.fieldId == field.id) {
                match = &candidate;
                ++found;
            }
        }

        if (found != 1 || CarriesValueNativeType(field) != (match->valueNativeType != nullptr) ||
            (match->encode == nullptr) != (match->decode == nullptr) ||
            (!HasFlag(field.flags, FieldFlags::Transient) && !match->encode)) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
    }

    constexpr usize fieldOffset =
        (sizeof(Record) + alignof(FieldSchema) - 1) & ~(alignof(FieldSchema) - 1);
    const usize fieldEnd = fieldOffset + schema.fields.Size() * sizeof(FieldSchema);
    const usize opsOffset =
        (fieldEnd + alignof(RuntimeStructFieldOps) - 1) & ~(alignof(RuntimeStructFieldOps) - 1);
    const usize opsEnd = opsOffset + schema.fields.Size() * sizeof(RuntimeStructFieldOps);
    const usize typesOffset = (opsEnd + alignof(Record*) - 1) & ~(alignof(Record*) - 1);
    const usize typesEnd = typesOffset + schema.fields.Size() * sizeof(Record*);
    const usize orderOffset = (typesEnd + alignof(uint32) - 1) & ~(alignof(uint32) - 1);
    usize bytes = orderOffset + schema.fields.Size() * sizeof(uint32);
    if (!AddBytes(bytes, schema.name.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }

    for (const FieldSchema& field : schema.fields) {
        if (!AddBytes(bytes, field.name.Size()) || !AddBytes(bytes, field.attributes.Size()) ||
            !AddBytes(bytes, field.category.Size()) || !AddBytes(bytes, field.displayName.Size()) ||
            !AddBytes(bytes, field.toolName.Size())) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
        }
    }

    uint32 depth = 1;
    bool requiresWorldOnlyOwner = ops.requiresWorldOnlyOwner;
    {
        platform::LockGuard lock(gate_);
        if (closed_ || mutating_ || nextGeneration_ == 0) {
            return Result<TypeRegistrationToken>::Failure(closed_     ? ErrorCode::ShuttingDown
                                                          : mutating_ ? ErrorCode::Busy
                                                                      : ErrorCode::Overflow);
        }

        // 같은 C++ struct를 다른 ID로 다시 등록하지 않는다(생성 thunk는 생성 ID 하나만 받는다).
        if (FindLocked(schema.id) != nullptr ||
            FindNativeLocked(ops.nativeType, TypeKind::Struct) != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
        }

        // struct는 WorldOnly 형질을 거부하지 않고 전파한다(그것을 필드로 둔 클래스가 판정한다).
        Span<const RuntimeStructFieldOps> thunks = ops.fields;
        if (auto resolved =
                ResolveValueTypes(schema.fields, &StructThunkType, &thunks, true, nullptr);
            !resolved) {
            return Result<TypeRegistrationToken>::Failure(resolved.Error());
        }

        // 멤버 struct와 struct 원소 배열 멤버의 형질을 전파한다(원소 배열 멤버의 깊이도 원소 깊이 +
        // 1). Required 후손은 struct 멤버와 원소 struct 모두에서 전파한다(Required 실효 값 검사의
        // 가지치기 — Transient 멤버 아래는 저장하지 않으므로 뺀다). 참조 struct 원소 배열
        // (StructRefArray) 멤버는 그 자체가 Registry 컨테이너다.
        for (const FieldSchema& field : schema.fields) {
            const ValueType& value = FieldTargetType(field);
            if (value.kind != ValueKind::Struct) {
                continue;
            }

            const bool array = field.value.kind == ValueKind::Array;
            if (const Record* record = FindLocked(value.type)) {
                depth = record->depth + 1 > depth ? record->depth + 1 : depth;
                strongReference = strongReference || record->containsStrongReference;
                required = required || (record->containsRequired &&
                                        !HasFlag(field.flags, FieldFlags::Transient));
                requiresWorldOnlyOwner = requiresWorldOnlyOwner || record->requiresWorldOnlyOwner;
                referenceArray = referenceArray || (array ? record->containsStrongReference
                                                          : record->containsReferenceArray);
            }
        }

        // 수동 등록의 깊이 방어(MetaCompiler RMC2303이 먼저 막는다)와 Trace 계약.
        if (depth > MaxStructDepth || (ops.trace != nullptr) != strongReference) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        mutating_ = true;
    }

    // 색인 공간은 레코드보다 먼저 준비한다(실패하면 아무것도 바꾸지 않는다).
    IndexGrowth growth(allocator_);
    if (auto prepared = PrepareIndexes(growth, true); !prepared) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(prepared.Error());
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), RecordTag);
    if (!memory) {
        platform::LockGuard lock(gate_);
        mutating_ = false;
        return Result<TypeRegistrationToken>::Failure(memory.Error());
    }

    if (!memory.Value()) {
        platform::Fatal("Allocator returned null TypeRegistry record");
    }

    Record* record = ::new (memory.Value()) Record();
    auto* block = static_cast<uint8*>(memory.Value());
    auto* fields = reinterpret_cast<FieldSchema*>(block + fieldOffset);
    auto* fieldOps = reinterpret_cast<RuntimeStructFieldOps*>(block + opsOffset);
    auto* fieldTypes = reinterpret_cast<Record**>(block + typesOffset);
    auto* fieldOrder = reinterpret_cast<uint32*>(block + orderOffset);
    uint8* cursor = reinterpret_cast<uint8*>(fieldOrder + schema.fields.Size());
    {
        // mutating_이 등록·해제를 막으므로 위에서 확인한 레코드가 그대로 있다.
        platform::LockGuard lock(gate_);
        Span<const RuntimeStructFieldOps> thunks = ops.fields;
        if (!ResolveValueTypes(schema.fields, &StructThunkType, &thunks, true, fieldTypes)) {
            platform::Fatal("TypeRegistry value type dependency vanished during registration");
        }
    }

    record->kind = TypeKind::Struct;
    record->fieldTypes = fieldTypes;
    record->schema.id = schema.id;
    record->schema.version = schema.version;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->schema.fields = {fields, schema.fields.Size()};
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        ::new (fields + index) FieldSchema(schema.fields[index]);
        fields[index].name = CopyBytes(schema.fields[index].name, cursor);
        fields[index].attributes = CopyBytes(schema.fields[index].attributes, cursor);
        fields[index].category = CopyBytes(schema.fields[index].category, cursor);
        fields[index].displayName = CopyBytes(schema.fields[index].displayName, cursor);
        fields[index].toolName = CopyBytes(schema.fields[index].toolName, cursor);
        // 멤버 함수표는 스키마 순서로 둔다(codec이 index로 바로 찾는다).
        for (const RuntimeStructFieldOps& candidate : ops.fields) {
            if (candidate.fieldId == fields[index].id) {
                ::new (fieldOps + index) RuntimeStructFieldOps(candidate);
                break;
            }
        }

        fieldOrder[index] = static_cast<uint32>(index);
    }

    algo::Sort(Span<uint32>(fieldOrder, schema.fields.Size()),
               [fields](uint32 left, uint32 right) noexcept {
                   const Guid a = fields[left].id;
                   const Guid b = fields[right].id;
                   return a.high != b.high ? a.high < b.high : a.low < b.low;
               });
    record->fieldOrder = fieldOrder;
    record->depth = depth;
    record->containsStrongReference = strongReference;
    record->containsRequired = required;
    record->requiresWorldOnlyOwner = requiresWorldOnlyOwner;
    record->containsReferenceArray = referenceArray;
    record->ops.size = ops.size;
    record->ops.alignment = ops.alignment;
    record->ops.nativeType = ops.nativeType;
    record->ops.module = Move(ops.module);
    record->structOps.size = ops.size;
    record->structOps.alignment = ops.alignment;
    record->structOps.nativeType = ops.nativeType;
    record->structOps.construct = ops.construct;
    record->structOps.destroy = ops.destroy;
    record->structOps.trace = ops.trace;
    record->structOps.migrate = ops.migrate;
    record->structOps.requiresWorldOnlyOwner = requiresWorldOnlyOwner;
    record->structOps.fields = {fieldOps, schema.fields.Size()};

    platform::LockGuard lock(gate_);
    PublishLocked(record, growth);
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        if (fieldTypes[fieldIndex]) {
            ++fieldTypes[fieldIndex]->dependents;
        }
    }

    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

} // namespace rocks
