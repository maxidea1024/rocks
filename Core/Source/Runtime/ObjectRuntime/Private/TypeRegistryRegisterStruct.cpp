#include "TypeRegistryInternal.h"
#include "TypeRegistryRegisterInternal.h"

#include <Object/TypeRegistry.h>
#include <Core/Algorithms.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <new>

// TypeRegistry struct 등록(RegisterStruct)과 클래스·struct 필드의 값 타입 레코드 해석
// (ResolveValueTypes — Enum·Struct 대상의 native 대조·WorldOnly 소유자·Optional 내부의 값 struct·
// 의존 계수 한도)이다
// (Docs/Design/ReflectionFoundations.ko.md 4.6절). 필드 서술 검사는 TypeRegistryRegister.cpp,
// 등록 파일이 함께 쓰는 도우미 선언은 TypeRegistryRegisterInternal.h다.
namespace rocks {

namespace {

using detail::AddBytes;
using detail::CarriesValueNativeType;
using detail::CopyBytes;
using detail::RecordTag;
using detail::ValidateFieldList;

// 클래스·struct가 받는 TypeFlags 비트다. 그 밖의 비트(클래스의 Atomic, struct의 Abstract·
// WorldOnly 포함)는 InvalidArgument다.
constexpr uint32 KnownStructFlags = static_cast<uint32>(TypeFlags::Atomic);

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

        // Optional의 내부 struct는 값 struct다(참조 struct는 null이 이미 "없음"이고, Optional 안의
        // 간선은 GC·관찰자 계수가 따라가지 않는다 —
        // Docs/Design/ReflectionExtensions.ko.md 1.3·6절).
        if (field.value.kind == ValueKind::Optional && target->containsStrongReference) {
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
        (schema.flags & ~KnownStructFlags) != 0 || ops.size == 0 || ops.alignment == 0 ||
        (ops.alignment & (ops.alignment - 1)) != 0 || !ops.nativeType || !ops.construct ||
        !ops.destroy || ops.fields.Size() != schema.fields.Size() ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8() ||
        (schema.toolName.Size() != 0 && !IsTypeToolName(schema.toolName))) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize typedFields = 0;
    if (auto validated = ValidateFieldList(schema.fields, true, typedFields); !validated) {
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
    bool nameMember = false;
    bool weakMember = false;
    for (const FieldSchema& field : schema.fields) {
        // 강한 참조 멤버: 단일 참조와 ObjectRefArray(대상 형식이 ObjectReference).
        const bool referenceTarget = FieldTargetType(field).kind == ValueKind::ObjectReference;
        // Name 멤버: 단일 Name과 Array<Name>(Transient 포함 — 관찰 struct 제한이 쓴다).
        nameMember = nameMember || FieldTargetType(field).kind == ValueKind::Name;
        // weak 참조 멤버: 단일 WeakRef와 Array<WeakRef>(Transient 포함 — 관찰 struct 제한이 쓴다).
        weakMember = weakMember || FieldTargetType(field).kind == ValueKind::WeakObjectReference;
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
    if (!AddBytes(bytes, schema.name.Size()) || !AddBytes(bytes, schema.toolName.Size())) {
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
            FindNativeLocked(ops.nativeType, TypeKind::Struct) != nullptr ||
            FindToolNameLocked(schema.toolName) != nullptr) {
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
                nameMember = nameMember || record->containsName;
                weakMember = weakMember || record->containsWeakReference;
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
    if (auto prepared = PrepareIndexes(growth, true, schema.toolName.Size() != 0); !prepared) {
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
    record->schema.flags = schema.flags;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->schema.toolName = CopyBytes(schema.toolName, cursor);
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
    record->containsName = nameMember;
    record->containsWeakReference = weakMember;
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
