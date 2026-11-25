#include "TypeRegistryInternal.h"
#include "TypeRegistryRegisterInternal.h"

#include <Base/PlacementNew.h>
#include <Object/TypeRegistry.h>
#include <Core/Algorithms.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

// TypeRegistry 클래스 등록(Register)과 클래스·struct가 함께 쓰는 필드 서술 검사(ValidateFieldList·
// CarriesValueNativeType)·식별자 판정(IsAsciiIdentifier)이다. enum 등록은
// TypeRegistryRegisterEnum.cpp, struct 등록과 필드 값 타입 레코드 해석(ResolveValueTypes)은
// TypeRegistryRegisterStruct.cpp, 등록 파일이 함께 쓰는 도우미 선언은
// TypeRegistryRegisterInternal.h다.
namespace rocks {

namespace {

using detail::AddBytes;
using detail::CarriesKeyNativeType;
using detail::CarriesValueNativeType;
using detail::ContainsInheritedFields;
using detail::CopyBytes;
using detail::RecordTag;
using detail::ValidateEditorOnlyFieldIds;
using detail::ValidateFieldList;

// 클래스·struct가 받는 TypeFlags 비트다. 그 밖의 비트(클래스의 Atomic, struct의 Abstract·
// WorldOnly 포함)는 InvalidArgument다.
constexpr uint32 KnownClassFlags =
    static_cast<uint32>(TypeFlags::Abstract) | static_cast<uint32>(TypeFlags::WorldOnly) |
    static_cast<uint32>(TypeFlags::Transient) | static_cast<uint32>(TypeFlags::Deprecated) |
    static_cast<uint32>(TypeFlags::EditorOnly);

constexpr uint32 KnownFieldFlags =
    static_cast<uint32>(FieldFlags::Transient) | static_cast<uint32>(FieldFlags::ReadOnly) |
    static_cast<uint32>(FieldFlags::EditorOnly) | static_cast<uint32>(FieldFlags::Required) |
    static_cast<uint32>(FieldFlags::DuplicateTransient) |
    static_cast<uint32>(FieldFlags::NonTryoutDuplicateTransient) |
    static_cast<uint32>(FieldFlags::SaveGame) |
    static_cast<uint32>(FieldFlags::TextExportTransient) |
    static_cast<uint32>(FieldFlags::TextMapMergeByKey);

// 복제 제외 플래그다. Required와 함께 쓰면 InvalidArgument다(리플렉션 B9h1 — 복제본이 늘 null을
// 가져 패키지 저장의 rejectMissingRequired가 실패한다).
constexpr uint32 DuplicateExcludedFlags =
    static_cast<uint32>(FieldFlags::DuplicateTransient) |
    static_cast<uint32>(FieldFlags::NonTryoutDuplicateTransient);

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

// 클래스 필드(structMember면 struct 멤버)의 값 종류로 등록할 수 있는지다. 종류는 성질 표
// (Meta/TypeSchema.h ValueKindProperties)로 판정하고, Opaque는 수동 메타데이터의 비typed 필드라 표
// 밖에서 받는다(struct는 typed 필드 수 검사가 Opaque 멤버를 거부한다).
bool SupportedFieldKind(ValueKind kind, bool structMember) noexcept {
    return kind == ValueKind::Opaque ||
           (structMember ? ValueKindCanBeStructMember(kind) : ValueKindCanBeField(kind));
}

// FieldValue로 통째 읽고 쓰지 않는 필드다(필드 단위 aggregate — struct 값·배열·Optional<값 struct>:
// read·write·writeStaged가 없어야 하고 Transient가 아니면 codec이 필수다).
bool IsAggregateField(const FieldSchema& field) noexcept {
    return FieldIsAggregate(field);
}

// 클래스 함수표에서 필드 ID의 valueNativeType(key면 keyNativeType)을 찾는다(ResolveValueTypes의
// thunkType).
const void* ClassThunkType(const void* context, Guid fieldId, bool key) noexcept {
    const auto& fields = *static_cast<const Span<const RuntimeFieldOps>*>(context);
    for (const RuntimeFieldOps& field : fields) {
        if (field.fieldId == fieldId) {
            return key ? field.keyNativeType : field.valueNativeType;
        }
    }

    return nullptr;
}

} // namespace

namespace detail {

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

Result<void> ValidateEditorOnlyFieldIds(Span<const FieldSchema> fields,
                                        Span<const Guid> ids) noexcept {
    // 필드 목록 상한(ValidateFieldList)과 같다.
    constexpr usize MaxEditorOnlyFieldIds = 4096;
    if (ids.Size() > MaxEditorOnlyFieldIds) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < ids.Size(); ++index) {
        if (ids[index] == Guid{}) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize previous = 0; previous < index; ++previous) {
            if (ids[previous] == ids[index]) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        for (usize field = 0; field < fields.Size(); ++field) {
            if (fields[field].id == ids[index] &&
                !HasFlag(fields[field].flags, FieldFlags::EditorOnly)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }
    }

    return Result<void>::Success();
}

Result<void> ValidateFieldList(Span<const FieldSchema> fields, bool structMembers,
                               usize& typedFields) noexcept {
    typedFields = 0;
    if (fields.Size() > 4096) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < fields.Size(); ++index) {
        const auto& field = fields[index];
        // 대상 형식은 배열이면 원소, Optional이면 내부, 아니면 값이다(Docs/Design/
        // ReflectionFoundations.ko.md 5.6절, ReflectionExtensions.ko.md 6절). Array 필드는
        // value.type 0, 원소 종류는 성질 표의 Array 원소 종류(Opaque·Array 거부), maxElements는 [1,
        // MaxArrayElements]이다. Optional 필드는 value.type 0, 내부 종류는 성질 표의 Optional 내부
        // 종류(참조·컨테이너·Optional 거부 — 참조 struct는 ResolveValueTypes가 거부한다),
        // maxElements 0이다. 그 밖의 필드는 element = {Opaque, 0}·maxElements 0이다.
        // Map 필드(리플렉션 B9f2)는 value.type 0, 키 종류는 성질 표의 Map 키 종류, 값 종류는 Map 값
        // 종류(ObjectReference는 ObjectRefMap — 리플렉션 B9f4, 참조 struct는 ResolveValueTypes가
        // 거부한다), maxElements는 배열과 같은 범위다. Set 필드는 원소 종류가
        // Set 원소 종류다. Map이 아닌 필드의 key는 {Opaque, 0}이다. StaticArray 필드(리플렉션
        // B9g)는 value.type 0, 원소 종류는 성질 표의 정적 배열 원소 종류(참조 struct 포함 —
        // Registry 컨테이너를 담은 원소 struct는 ResolveValueTypes가 거부한다), maxElements는
        // 정확한 원소 수 N으로 [1, MaxStaticArrayElements]이다.
        const bool array = field.value.kind == ValueKind::Array;
        const bool fixed = field.value.kind == ValueKind::StaticArray;
        const bool optional = field.value.kind == ValueKind::Optional;
        const bool map = field.value.kind == ValueKind::Map;
        const bool set = field.value.kind == ValueKind::Set;
        const ValueType& target = FieldTargetType(field);
        const ValueType& key = FieldKeyType(field);
        const bool counted = field.value.type == TypeId{} && field.maxElements >= 1 &&
                             field.maxElements <= MaxArrayElements;
        const bool arrayShape = array   ? counted && ValueKindCanBeArrayElement(field.element.kind)
                                : fixed ? field.value.type == TypeId{} && field.maxElements >= 1 &&
                                              field.maxElements <= MaxStaticArrayElements &&
                                              ValueKindCanBeStaticArrayElement(field.element.kind)
                                : optional ? field.value.type == TypeId{} &&
                                                 ValueKindCanBeOptionalInner(field.element.kind) &&
                                                 field.maxElements == 0
                                : map ? counted && ValueKindCanBeMapValue(field.element.kind) &&
                                            ValueKindCanBeMapKey(key.kind)
                                : set
                                    ? counted && ValueKindCanBeSetElement(field.element.kind)
                                    : field.element.kind == ValueKind::Opaque &&
                                          field.element.type == TypeId{} && field.maxElements == 0;
        // 키 대상 type은 키 종류가 대상 타입 종류일 때만 쓰고(Enum ID·ClassRef Base 필수, Soft 기대
        // 타입 선택) 그 밖은 0이다. Map이 아닌 필드의 key는 {Opaque, 0}이다.
        const bool keyShape =
            map ? (ValueKindHasTargetType(key.kind) || key.type == TypeId{}) &&
                      (!ValueKindRequiresTargetId(key.kind) || key.type != TypeId{})
                : key.kind == ValueKind::Opaque && key.type == TypeId{};
        // Required는 성질 표가 허용하는 대상 종류(typed 참조·소프트 참조, 배열이면 원소)에만 뜻이
        // 있다(저장이 null을 검사하려면 접근기·codec이 있어야 한다). Optional은 대상 종류가 내부
        // 종류이고 Optional 내부 종류는 모두 Required를 받지 않으므로 같은 판정이 거부한다(성질
        // 표의 불변식 — Meta/TypeSchema.h static_assert). 도구 이름은 비어 있거나 ASCII 식별자다.
        if (field.id == Guid{} || (field.flags & ~KnownFieldFlags) != 0 ||
            (HasFlag(field.flags, FieldFlags::Required) &&
             (field.flags & DuplicateExcludedFlags) != 0) ||
            (HasFlag(field.flags, FieldFlags::Required) &&
             !ValueKindAcceptsRequired(target.kind)) ||
            (field.kind != FieldKind::Value && field.kind != FieldKind::ObjectReference) ||
            !SupportedFieldKind(field.value.kind, structMembers) || !arrayShape || !keyShape ||
            !StringView(field.name.Data(), field.name.Size()).ValidateUtf8() ||
            !StringView(field.category.Data(), field.category.Size()).ValidateUtf8() ||
            !StringView(field.displayName.Data(), field.displayName.Size()).ValidateUtf8() ||
            (field.toolName.Size() != 0 && !IsAsciiIdentifier(field.toolName))) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // bundle 목록(에셋 A4e2a)은 정규 목록이고, 소프트 참조·struct·InstancedStruct 값(Map은 키
        // 소프트 참조도)에만, EditorOnly가 아닌 필드에만 쓴다(쿠킹 패키지에서 빠지는 참조는 bundle
        // 구성원이 될 수 없다 — MetaCompiler RMC2202·RMC2212와 같은 규칙).
        if (field.assetBundles.Size() != 0) {
            const bool carries = target.kind == ValueKind::SoftObjectReference ||
                                 target.kind == ValueKind::Struct ||
                                 target.kind == ValueKind::InstancedStruct ||
                                 (map && key.kind == ValueKind::SoftObjectReference);
            if (!carries || HasFlag(field.flags, FieldFlags::EditorOnly) ||
                !IsAssetBundleList(field.assetBundles)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        // FieldKind::ObjectReference(강한 GC 간선) ⇔ 대상 형식이 ObjectReference(단일 참조,
        // ObjectRefArray, ObjectRef 원소 정적 배열 또는 ObjectRefMap). 대상 type은 성질 표의 대상
        // 타입 종류(참조·소프트 참조의 기대 대상, Enum·Struct의 대상 ID)에만 쓰고, 대상 ID 필수
        // 종류(Enum·Struct)는 ID가 있어야 한다.
        if (field.value.kind != ValueKind::Opaque) {
            ++typedFields;
            const bool reference = target.kind == ValueKind::ObjectReference;
            if (field.name.Size() == 0 || (field.kind == FieldKind::ObjectReference) != reference ||
                (!ValueKindHasTargetType(target.kind) && target.type != TypeId{}) ||
                (ValueKindRequiresTargetId(target.kind) && target.type == TypeId{})) {
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

bool CarriesValueNativeType(const FieldSchema& field) noexcept {
    const ValueKind kind = FieldTargetType(field).kind;
    return kind == ValueKind::Enum || kind == ValueKind::Struct;
}

bool CarriesKeyNativeType(const FieldSchema& field) noexcept {
    return field.value.kind == ValueKind::Map && FieldKeyType(field).kind == ValueKind::Enum;
}

bool ContainsInheritedFields(Span<const FieldSchema> base,
                             Span<const FieldSchema> derived) noexcept {
    for (const FieldSchema& inherited : base) {
        bool matched = false;
        for (const FieldSchema& field : derived) {
            if (field.id == inherited.id && field.kind == inherited.kind &&
                field.value.kind == inherited.value.kind &&
                field.value.type == inherited.value.type &&
                field.element.kind == inherited.element.kind &&
                field.element.type == inherited.element.type &&
                field.key.kind == inherited.key.kind && field.key.type == inherited.key.type &&
                field.maxElements == inherited.maxElements && field.flags == inherited.flags &&
                SameBytes(EffectiveToolName(field), EffectiveToolName(inherited)) &&
                SameBytes(field.assetBundles, inherited.assetBundles)) {
                matched = true;
                break;
            }
        }

        if (!matched) {
            return false;
        }
    }

    return true;
}

} // namespace detail

Result<TypeRegistrationToken> TypeRegistry::Register(const TypeSchema& schema,
                                                     RuntimeTypeOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }

    if (schema.id == Guid{} || schema.version == 0 || schema.fields.Size() > 4096 ||
        ops.fields.Size() > schema.fields.Size() || schema.baseType == schema.id ||
        (schema.flags & ~KnownClassFlags) != 0 ||
        (HasFlag(schema.flags, TypeFlags::Abstract) != (ops.construct == nullptr)) ||
        !ops.destroy || !ops.trace || !ops.nativeType || ops.size < sizeof(Object) ||
        ops.alignment < alignof(Object) || (ops.alignment & (ops.alignment - 1)) != 0 ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8() ||
        (schema.toolName.Size() != 0 && !IsTypeToolName(schema.toolName))) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    usize typedFields = 0;
    if (auto validated = ValidateFieldList(schema.fields, false, typedFields); !validated) {
        return Result<TypeRegistrationToken>::Failure(validated.Error());
    }

    if (typedFields != ops.fields.Size() ||
        (ops.completeFieldLayout && typedFields != schema.fields.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto listed = ValidateEditorOnlyFieldIds(schema.fields, schema.editorOnlyFieldIds);
        !listed) {
        return Result<TypeRegistrationToken>::Failure(listed.Error());
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
                CarriesValueNativeType(schemaField) == (field.valueNativeType != nullptr) &&
                CarriesKeyNativeType(schemaField) == (field.keyNativeType != nullptr)) {
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
        // 규칙). 경로 접근 thunk(path)는 모든 필드가 가질 수 있다 — 게시된 객체의 최상위 단일 값
        // 읽기·쓰기는 그래도 read·write 한 경로이고, 단일 값의 thunk는 staged 객체·Detach·Attach가
        // 쓴다(리플렉션 B9i3c0).
        if (!matched || !accessors || (field.encode == nullptr) != (field.decode == nullptr) ||
            (aggregateField && !field.encode && !HasFlag(matched->flags, FieldFlags::Transient))) {
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
    // 값 대상과 키 대상(리플렉션 B9f2) 레코드 포인터를 이어 둔다.
    const usize typesEnd = typesOffset + 2 * schema.fields.Size() * sizeof(Record*);
    // 필드 ID 정렬 색인과 스키마 위치 → 함수표 위치 색인(경로 접근의 FieldIndex·FieldOps).
    const usize orderOffset = (typesEnd + alignof(uint32) - 1) & ~(alignof(uint32) - 1);
    const usize opsIndexOffset = orderOffset + schema.fields.Size() * sizeof(uint32);
    // EditorOnly 필드 ID 목록(Guid 정렬)을 문자열보다 먼저 둔다.
    const usize idsOffset =
        (opsIndexOffset + schema.fields.Size() * sizeof(uint32) + alignof(Guid) - 1) &
        ~(alignof(Guid) - 1);
    usize bytes = idsOffset + schema.editorOnlyFieldIds.Size() * sizeof(Guid);
    if (!AddBytes(bytes, schema.name.Size()) || !AddBytes(bytes, schema.toolName.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }

    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        const auto& field = schema.fields[fieldIndex];
        if (!AddBytes(bytes, field.name.Size()) || !AddBytes(bytes, field.attributes.Size()) ||
            !AddBytes(bytes, field.category.Size()) || !AddBytes(bytes, field.displayName.Size()) ||
            !AddBytes(bytes, field.toolName.Size()) ||
            !AddBytes(bytes, field.assetBundles.Size())) {
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

        // 타입 도구 이름은 클래스·struct 사이에서 대소문자 무시로 유일하다.
        if (FindLocked(schema.id) != nullptr || FindToolNameLocked(schema.toolName) != nullptr) {
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
        if (auto resolved =
                ResolveValueTypes(schema.fields, &ClassThunkType, &ops.fields,
                                  HasFlag(schema.flags, TypeFlags::WorldOnly), nullptr, nullptr);
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
            // static_assert(IsBaseOfV)가 보증하고, 여기서는 명백히 잘못된 수동 함수표(기반보다
            // 작거나 정렬이 약한 파생)를 거부한다.
            if (ops.size < base->ops.size || ops.alignment < base->ops.alignment) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
            }

            // WorldOnly·Transient·Deprecated(리플렉션 B9h2)·EditorOnly(에셋 A4e2a)는 상속되는
            // 플래그다 — 수동 등록이 파생에서 빠뜨려도 받아들이지 않는다. Within은 파생이
            // 이어받는다 (base에 Within이 있으면 파생도 0이 아니어야 한다 — 파생 관계 판정은
            // MetaCompiler RMC2208).
            if ((base->schema.flags & InheritedTypeFlags & ~schema.flags) != 0 ||
                (base->schema.withinType != TypeId{} && schema.withinType == TypeId{})) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
            }

            // 파생이 Within을 다시 정하면 base Within의 파생이어야 한다(좁히기만 — MetaCompiler
            // RMC2208과 같은 규칙). 자기 자신이면 base 사슬로, 등록된 다른 클래스면 그 레코드
            // 사슬로 본다. 아직 등록되지 않은 Within 클래스(모듈 등록 순서)는 여기서 판정하지
            // 않는다.
            if (base->schema.withinType != TypeId{} &&
                schema.withinType != base->schema.withinType) {
                const auto derives = [&](const Record* record) noexcept {
                    for (; record; record = record->base) {
                        if (record->schema.id == base->schema.withinType) {
                            return true;
                        }
                    }

                    return false;
                };
                const Record* within =
                    schema.withinType == schema.id ? base : FindLocked(schema.withinType);
                if (within && !derives(within)) {
                    return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
                }
            }

            // 파생 스키마는 base field를 동일한 안정 ID/표현으로 포함한다. 그래서 기반 타입의 필드
            // 함수(Resolve<기반> 경유)를 파생 객체에 써도 같은 필드를 가리킨다.
            if (!ContainsInheritedFields(base->schema.fields, schema.fields)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
            }
        } else if (ops.nativeBaseType != nullptr) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        mutating_ = true;
    }

    // 색인 공간은 레코드보다 먼저 준비한다(실패하면 아무것도 바꾸지 않는다).
    IndexGrowth growth(allocator_);
    if (auto prepared = PrepareIndexes(growth, false, schema.toolName.Size() != 0); !prepared) {
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
    Record** fieldKeyTypes = fieldTypes + schema.fields.Size();
    auto* fieldOrder = reinterpret_cast<uint32*>(static_cast<uint8*>(memory.Value()) + orderOffset);
    auto* opsIndex =
        reinterpret_cast<uint32*>(static_cast<uint8*>(memory.Value()) + opsIndexOffset);
    uint8* cursor = static_cast<uint8*>(memory.Value()) + idsOffset;
    {
        // mutating_이 등록·해제를 막으므로 위에서 확인한 enum·struct 레코드가 그대로 있다.
        platform::LockGuard lock(gate_);
        Span<const RuntimeFieldOps> thunks = ops.fields;
        if (!ResolveValueTypes(schema.fields, &ClassThunkType, &thunks,
                               HasFlag(schema.flags, TypeFlags::WorldOnly), fieldTypes,
                               fieldKeyTypes)) {
            platform::Fatal("TypeRegistry value type dependency vanished during registration");
        }
    }

    record->fieldTypes = fieldTypes;
    record->fieldKeyTypes = fieldKeyTypes;
    record->schema.id = schema.id;
    record->schema.version = schema.version;
    record->schema.baseType = schema.baseType;
    record->schema.flags = schema.flags;
    record->schema.withinType = schema.withinType;
    record->schema.editorOnlyFieldIds = CopyBytes(schema.editorOnlyFieldIds, cursor);
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
        fields[index].assetBundles = CopyBytes(schema.fields[index].assetBundles, cursor);
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
                fieldOps[index].write.Bind(*this, ops.nativeType, fields[fieldIndex]);
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

        if (fieldKeyTypes[fieldIndex]) {
            ++fieldKeyTypes[fieldIndex]->dependents;
        }
    }

    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

} // namespace rocks
