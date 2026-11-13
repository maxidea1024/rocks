#include <Object/TypeRegistry.h>
#include <Object/ObjectRegistry.h>
#include <Core/Algorithms.h>
#include <Object/SoftObjectRef.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"
#include <new>

namespace rocks {

Result<void> FieldWriteOperation::operator()(ObjectRegistry& registry,
                                             const ObjectAccessContext& access, ObjectHandle handle,
                                             const FieldValue& value) const noexcept {
    // 묶인 struct·배열 필드는 callback이 없다(아래에서 문맥·대상 검사 뒤 Unsupported).
    const bool aggregateField =
        bound_ && (valueKind_ == ValueKind::Struct || valueKind_ == ValueKind::Array);
    if (!callback_ && !aggregateField) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!bound_) {
        // 등록 전 원본 함수표는 기존 저수준 callback 계약을 유지한다.
        return callback_(registry, access, handle, value);
    }

    // 다른 TypeRegistry 또는 같은 native 타입의 별명 스키마도 대상 객체 기준으로 확인한다.
    // callback을 호출할 때는 registry 잠금을 보유하지 않으며 대상 타입 lease는 종료까지 유지한다.
    auto type = registry.AcquireType(access, handle);
    if (!type) {
        return Result<void>::Failure(type.Error());
    }

    // 필드를 선언한 타입 또는 그 파생 객체에 쓸 수 있다. 파생 스키마는 상속 필드를 같은 id·kind·
    // flags·참조 타입으로 포함함을 등록 시 강제한다.
    if (!type.Value().IsNativeA(nativeType_)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    const FieldSchema* actual = nullptr;
    usize actualIndex = 0;
    const auto fields = type.Value().Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (fields[index].id == fieldId_) {
            actual = &fields[index];
            actualIndex = index;
            break;
        }
    }

    if (!actual) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }

    // struct·배열 필드는 FieldValue로 통째 쓰지 않는다(멤버·원소 단위 쓰기는 B6 경로 접근 —
    // 3.6·5.6절, 배열 통째 쓰기는 B6 뒤에도 Unsupported). 문맥·수명·대상 타입·필드 검사 뒤의 정책
    // 오류다.
    if (aggregateField) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 검증에 쓴 lease(대상 타입·양쪽 기대 타입·참조 대상)는 callback 완료까지 유지한다. 그 전에
    // 해제하면 callback 재진입에서 같은 ID의 타입을 unregister/재등록해 검증과 실제 필드 게시의
    // 기준이 달라질 수 있다.
    TypeLease sourceExpected;
    TypeLease targetExpected;
    TypeLease referenced;
    detail::FieldWriteTarget target;
    target.actual = actual;
    target.effectiveFlags = actual->flags;
    target.actualEnum = type.Value().FieldEnum(actualIndex);
    if (valueKind_ == ValueKind::ObjectReference && value.kind == ValueKind::ObjectReference &&
        actual->value.kind == ValueKind::ObjectReference) {
        // 등록 시점에 자기 참조나 별칭이 아직 미등록일 수 있으므로 호출 시 각 레지스트리에서
        // 참조 타입을 확인한다. 서로 다른 GUID라도 같은 native 타입인 별칭은 허용한다.
        if (referencedType_ != TypeId{}) {
            auto expected = registry_->Acquire(referencedType_);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            sourceExpected = Move(expected.Value());
        }

        if (actual->value.type != TypeId{}) {
            auto expected = registry.AcquireRegisteredType(access, actual->value.type);
            if (!expected) {
                return Result<void>::Failure(expected.Error());
            }

            targetExpected = Move(expected.Value());
        }

        target.sourceExpectedNative =
            sourceExpected.IsValid() ? sourceExpected.Ops().nativeType : nullptr;
        target.targetExpectedNative =
            targetExpected.IsValid() ? targetExpected.Ops().nativeType : nullptr;
        // 원본·대상 스키마의 기대 타입이 서로 다른 native면 참조 값을 조회하기 전에 거부한다(별칭은
        // 같은 native라 통과한다). ValidateFieldWrite도 같은 검사를 한다.
        if (target.sourceExpectedNative && target.targetExpectedNative &&
            target.sourceExpectedNative != target.targetExpectedNative) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (value.reference != ObjectHandle{}) {
            if (value.reference.IsNull()) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }

            auto found = registry.AcquireType(access, value.reference);
            if (!found) {
                return Result<void>::Failure(found.Error());
            }

            referenced = Move(found.Value());
            target.referenced = &referenced;
        }
    }

    detail::FieldWriteBinding binding;
    binding.value = ValueType{valueKind_, referencedType_};
    binding.readOnly = readOnly_;
    if (auto valid = detail::ValidateFieldWrite(binding, target, value); !valid) {
        return valid;
    }

    return callback_(registry, access, handle, value);
}

namespace detail {

Result<void> ValidateFieldWrite(const FieldWriteBinding& binding, const FieldWriteTarget& target,
                                const FieldValue& value) noexcept {
    const ValueKind kind = binding.value.kind;
    // 원소 모드는 배열 필드의 원소 형식과 대조한다.
    if (!target.actual || (target.element && target.actual->value.kind != ValueKind::Array)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    const ValueType& actual = target.element ? target.actual->element : target.actual->value;
    if (actual.kind != kind || value.kind != kind) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    // enum 값은 필드가 가리키는 enum의 ID를 싣는다(원본·대상 스키마가 같은 enum을 가리켜야 한다).
    if (kind == ValueKind::Enum &&
        (value.type != binding.value.type || actual.type != binding.value.type)) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (kind == ValueKind::ObjectReference) {
        // 서로 다른 GUID라도 같은 native 타입인 별칭은 허용한다.
        const void* source = target.sourceExpectedNative;
        const void* expected = target.targetExpectedNative;
        if (source && expected && source != expected) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        // null은 단일 참조·ObjectRefArray 원소 모두 받는다(B5 — 원소 null 허용).
        if (value.reference != ObjectHandle{}) {
            // 선언 타입 또는 그 파생 타입 객체를 허용한다(SetReference와 같은 상속 규칙).
            if (!target.referenced || (source && !target.referenced->IsNativeA(source)) ||
                (expected && !target.referenced->IsNativeA(expected))) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }
    }

    // struct 값·배열(원소 모드의 struct 원소 포함)은 FieldValue로 통째 쓰지 않는다 — 종류 대조
    // 뒤의 정책 오류다(멤버·원소 단위 쓰기는 경로 접근이 단일 값으로 내려가 이 함수를 다시 거친다).
    if (kind == ValueKind::Struct || kind == ValueKind::Array) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (binding.readOnly || HasFlag(target.effectiveFlags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    // 값 내용 검사는 읽기 전용 정책 뒤다(정수 폭 Overflow를 callback이 검사하는 것과 같은 순서).
    // 소프트 참조의 null 규칙·경로 상한은 수동 callback도 지키도록 여기서 검사한다. 기대 타입은
    // 대상 에셋을 로드하지 않으므로 쓰기에서 검사하지 않는다.
    if (kind == ValueKind::SoftObjectReference &&
        !SoftObjectRef<>::Validate(AssetId{value.guid}, value.string.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 선언되지 않은 enum 값(Flags는 선언 값의 OR가 아닌 값)은 수동 callback에도 넘기지 않는다.
    // 실제 대상 객체 쪽 enum 스키마로 검사한다.
    if (kind == ValueKind::Enum &&
        (!target.actualEnum || !IsDeclaredEnumValue(*target.actualEnum, value.signedInteger))) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> ValidateArrayLength(const FieldSchema& field, usize newLength) noexcept {
    if (field.value.kind != ValueKind::Array) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    if (newLength > field.maxElements) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    return Result<void>::Success();
}

} // namespace detail

namespace detail {

uint64 NextRegistryDomain() noexcept {
    static platform::Mutex gate;
    static uint64 next = 1;
    platform::LockGuard lock(gate);
    if (next == 0) {
        platform::Fatal("Registry domain exhausted");
    }

    return next++;
}

} // namespace detail

// 클래스·enum·struct 레코드가 같은 목록·ID 공간을 쓴다. enum 레코드는 schema.id·name과
// ops.nativeType·module만 채우고 enumSchema에 열거자를 둔다. struct 레코드는 schema(id·version·
// name·fields)와 ops(size·alignment·nativeType·module)를 채우고 structOps에 멤버 함수표(스키마
// 순서)·construct·destroy·trace·migrate를, fieldOrder에 필드 ID 순 정렬 색인을 둔다. 클래스·struct
// 레코드의 fieldTypes는 schema.fields와 같은 길이이며 필드의 대상 형식(배열이면 원소)이 가리키는
// Enum·Struct 레코드(그 밖은 nullptr)다 — 이 참조는 lease가 아니라 dependents 계수로 표시하므로
// Shutdown이 영구 Busy가 되지 않는다.
struct TypeRegistry::Record {
    Record* next = nullptr;
    TypeKind kind = TypeKind::Class;
    TypeSchema schema;
    RuntimeTypeOps ops;
    EnumSchema enumSchema;
    RuntimeStructOps structOps;
    uint32* fieldOrder = nullptr;
    uint32 depth = 0;
    bool containsStrongReference = false;
    bool containsRequired = false;
    bool requiresWorldOnlyOwner = false;
    bool containsReferenceArray = false;
    Record** fieldTypes = nullptr;
    uint32 generation = 0;
    uint32 leases = 0;
    uint32 dependents = 0;
    Record* base = nullptr;
};

namespace {

bool AddBytes(usize& total, usize count) noexcept {
    if (count > ~usize(0) - total) {
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

char AsciiLower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool EqualsIgnoringAsciiCase(Span<const char> left, Span<const char> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (AsciiLower(left[index]) != AsciiLower(right[index])) {
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
            if (AsciiLower(prefix[index]) != AsciiLower(longer[index])) {
                return false;
            }
        }

        for (usize index = 0; index < shorter.Size(); ++index) {
            if (AsciiLower(shorter[index]) != AsciiLower(longer[prefix.Size() + index])) {
                return false;
            }
        }

        return true;
    };
    return prefixed(left, right) || prefixed(right, left);
}

constexpr usize MaxEnumerators = 4096;

// 도구 이름이 없으면 C++ 필드 이름이 도구 이름이다.
Span<const char> EffectiveToolName(const FieldSchema& field) noexcept {
    return field.toolName.Size() != 0 ? field.toolName : field.name;
}

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

TypeRegistry::TypeRegistry(Allocator& allocator) noexcept
    : allocator_(allocator), domain_(detail::NextRegistryDomain()),
      ownerThread_(platform::CurrentThreadId()) {
}

TypeRegistry::~TypeRegistry() {
    auto result = Shutdown();
    if (!result) {
        platform::Fatal("TypeRegistry destroyed with active leases or on wrong thread");
    }
}

TypeLease::TypeLease(TypeLease&& other) noexcept : owner_(other.owner_), record_(other.record_) {
    other.owner_ = nullptr;
    other.record_ = nullptr;
}

TypeLease& TypeLease::operator=(TypeLease&& other) noexcept {
    if (this != &other) {
        Reset();
        owner_ = other.owner_;
        record_ = other.record_;
        other.owner_ = nullptr;
        other.record_ = nullptr;
    }

    return *this;
}

TypeLease::~TypeLease() {
    Reset();
}

void TypeLease::Reset() noexcept {
    if (owner_) {
        owner_->Release(record_);
        owner_ = nullptr;
        record_ = nullptr;
    }
}

Result<TypeLease> TypeLease::Clone() const noexcept {
    if (!owner_) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    return owner_->Clone(record_);
}

const TypeSchema& TypeLease::Schema() const& noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->schema;
}

TypeId TypeLease::Id() const noexcept {
    return Schema().id;
}

TypeKind TypeLease::Kind() const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->kind;
}

const EnumSchema& TypeLease::Enum() const& noexcept {
    if (Kind() != TypeKind::Enum) {
        platform::Fatal("TypeLease::Enum called on a non-enum lease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->enumSchema;
}

const EnumSchema* TypeLease::FieldEnum(usize fieldIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || fieldIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldEnum called on an empty lease or out of range");
    }

    if (!record->fieldTypes || !record->fieldTypes[fieldIndex] ||
        record->fieldTypes[fieldIndex]->kind != TypeKind::Enum) {
        return nullptr;
    }

    return &record->fieldTypes[fieldIndex]->enumSchema;
}

StructView TypeLease::FieldStruct(usize fieldIndex) const& noexcept {
    const auto* record = static_cast<TypeRegistry::Record*>(record_);
    if (!record || fieldIndex >= record->schema.fields.Size()) {
        platform::Fatal("TypeLease::FieldStruct called on an empty lease or out of range");
    }

    if (!record->fieldTypes || !record->fieldTypes[fieldIndex] ||
        record->fieldTypes[fieldIndex]->kind != TypeKind::Struct) {
        return StructView();
    }

    return StructView(record->fieldTypes[fieldIndex]);
}

StructView TypeLease::Struct() const& noexcept {
    if (Kind() != TypeKind::Struct) {
        platform::Fatal("TypeLease::Struct called on a non-struct lease");
    }

    return StructView(record_);
}

const RuntimeStructOps& TypeLease::StructOps() const& noexcept {
    if (Kind() != TypeKind::Struct) {
        platform::Fatal("TypeLease::StructOps called on a non-struct lease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->structOps;
}

namespace {

// 레코드 타입 이름(private)은 호출하는 StructView 멤버(TypeRegistry의 friend)에서 접근 검사된다.
template <class Record>
const Record& ViewAs(const void* record) noexcept {
    if (!record) {
        platform::Fatal("Access to an empty StructView");
    }

    return *static_cast<const Record*>(record);
}

} // namespace

const TypeSchema& StructView::Schema() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).schema;
}

const RuntimeStructOps& StructView::StructOps() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).structOps;
}

const RuntimeStructFieldOps& StructView::FieldOps(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.structOps.fields.Size()) {
        platform::Fatal("StructView::FieldOps out of range");
    }

    return record.structOps.fields[schemaIndex];
}

usize StructView::FieldIndex(Guid fieldId) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    usize low = 0;
    usize high = record.schema.fields.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        const Guid id = record.schema.fields[record.fieldOrder[middle]].id;
        if (id == fieldId) {
            return record.fieldOrder[middle];
        }

        if (id.high < fieldId.high || (id.high == fieldId.high && id.low < fieldId.low)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return NoField;
}

const EnumSchema* StructView::FieldEnum(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.schema.fields.Size()) {
        platform::Fatal("StructView::FieldEnum out of range");
    }

    const auto* target = record.fieldTypes[schemaIndex];
    return target && target->kind == TypeKind::Enum ? &target->enumSchema : nullptr;
}

StructView StructView::FieldStruct(usize schemaIndex) const noexcept {
    const auto& record = ViewAs<TypeRegistry::Record>(record_);
    if (schemaIndex >= record.schema.fields.Size()) {
        platform::Fatal("StructView::FieldStruct out of range");
    }

    const auto* target = record.fieldTypes[schemaIndex];
    return target && target->kind == TypeKind::Struct ? StructView(target) : StructView();
}

uint32 StructView::Depth() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).depth;
}

bool StructView::ContainsStrongReference() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsStrongReference;
}

bool StructView::ContainsRequired() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsRequired;
}

bool StructView::RequiresWorldOnlyOwner() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).requiresWorldOnlyOwner;
}

bool StructView::ContainsReferenceArray() const noexcept {
    return ViewAs<TypeRegistry::Record>(record_).containsReferenceArray;
}

StringView TypeLease::Name() const& noexcept {
    const auto& name = Schema().name;
    return StringView(name.Data(), name.Size());
}

const RuntimeTypeOps& TypeLease::Ops() const& noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }

    return static_cast<TypeRegistry::Record*>(record_)->ops;
}

bool TypeLease::IsA(TypeId expected) const noexcept {
    if (!record_) {
        platform::Fatal("TypeLease::IsA called on empty lease");
    }

    for (auto* record = static_cast<TypeRegistry::Record*>(record_); record;
         record = record->base) {
        if (record->schema.id == expected) {
            return true;
        }
    }

    return false;
}

bool TypeLease::IsNativeA(const void* nativeType) const noexcept {
    if (!record_) {
        platform::Fatal("TypeLease::IsNativeA called on empty lease");
    }

    // nullptr는 기반 Object이며 모든 등록 타입이 만족한다. 등록 시 base 레코드의 nativeType과
    // 파생 레코드의 nativeBaseType 일치를 검증했으므로 base 사슬이 곧 C++ 상속 사슬이다.
    if (nativeType == nullptr) {
        return true;
    }

    for (auto* record = static_cast<TypeRegistry::Record*>(record_); record;
         record = record->base) {
        if (record->ops.nativeType == nativeType) {
            return true;
        }
    }

    return false;
}

Result<bool> TypeRegistry::IsA(TypeId actual, TypeId expected) noexcept {
    if (expected == TypeId{}) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    auto lease = Acquire(actual);
    if (!lease) {
        return Result<bool>::Failure(lease.Error());
    }

    return Result<bool>::Success(lease.Value().IsA(expected));
}

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

        // struct·배열 필드는 FieldValue로 통째 읽고 쓰지 않으므로 read·write·writeStaged가 없어야
        // 하고(멤버·원소 단위는 B6 경로 접근) 그 밖의 필드는 셋 다 있어야 한다.
        const bool aggregateField = matched && IsAggregateField(*matched);
        const bool accessors = aggregateField ? (!field.read && !field.write && !field.writeStaged)
                                              : (field.read && field.write && field.writeStaged);
        // struct·배열 필드는 Transient가 아니면 codec이 있어야 한다(그 밖은 completeFieldLayout
        // 규칙).
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
    usize bytes = typesOffset + schema.fields.Size() * sizeof(Record*);
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

        for (Record* record = records_; record; record = record->next) {
            if (record->schema.id == schema.id) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }

            if (record->schema.id == schema.baseType) {
                // base는 클래스여야 한다(Acquire의 종류 규칙과 같다).
                if (record->kind != TypeKind::Class) {
                    return Result<TypeRegistrationToken>::Failure(ErrorCode::TypeMismatch);
                }

                base = record;
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

            if (ops.nativeBaseType != base->ops.nativeType || base->dependents == 0xffffffffu) {
                return Result<TypeRegistrationToken>::Failure(base->dependents == 0xffffffffu
                                                                  ? ErrorCode::Overflow
                                                                  : ErrorCode::TypeMismatch);
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

    auto memory = allocator_.Allocate(bytes, alignof(Record), 0x54595045u);
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
    uint8* cursor = reinterpret_cast<uint8*>(fieldTypes + schema.fields.Size());
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
    }

    for (usize index = 0; index < ops.fields.Size(); ++index) {
        ::new (fieldOps + index) RuntimeFieldOps(ops.fields[index]);
        for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
            if (fields[fieldIndex].id == fieldOps[index].fieldId) {
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
    record->generation = nextGeneration_++;
    record->next = records_;
    records_ = record;
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
        for (Record* record = records_; record; record = record->next) {
            if (record->schema.id == schema.id ||
                (record->kind == TypeKind::Enum && record->ops.nativeType == ops.nativeType)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }

        mutating_ = true;
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), 0x54595045u);
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
    record->generation = nextGeneration_++;
    record->next = records_;
    records_ = record;
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

        Record* target = nullptr;
        for (Record* record = records_; record; record = record->next) {
            if (record->schema.id == value.type) {
                target = record;
                break;
            }
        }

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

        if (target->dependents >= 0xffffffffu - fields.Size()) {
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
        // Transient 멤버는 저장·복원하지 않으므로 Required 누락 진단 대상이 아니다. 배열 멤버의
        // Required는 원소마다 non-null이라는 뜻이고 누락된 배열은 생성자 값이므로 세지 않는다.
        required = required || (HasFlag(field.flags, FieldFlags::Required) &&
                                !HasFlag(field.flags, FieldFlags::Transient) &&
                                field.value.kind != ValueKind::Array);
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
        for (Record* record = records_; record; record = record->next) {
            if (record->schema.id == schema.id ||
                (record->kind == TypeKind::Struct && record->ops.nativeType == ops.nativeType)) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }

        // struct는 WorldOnly 형질을 거부하지 않고 전파한다(그것을 필드로 둔 클래스가 판정한다).
        Span<const RuntimeStructFieldOps> thunks = ops.fields;
        if (auto resolved =
                ResolveValueTypes(schema.fields, &StructThunkType, &thunks, true, nullptr);
            !resolved) {
            return Result<TypeRegistrationToken>::Failure(resolved.Error());
        }

        // 멤버 struct와 struct 원소 배열 멤버의 형질을 전파한다(원소 배열 멤버의 깊이도 원소 깊이 +
        // 1). 누락 struct 안 Required 후손 진단은 struct 멤버로만 내려가고(배열 원소 제외), 참조
        // struct 원소 배열(StructRefArray) 멤버는 그 자체가 Registry 컨테이너다.
        for (const FieldSchema& field : schema.fields) {
            const ValueType& value = FieldTargetType(field);
            if (value.kind != ValueKind::Struct) {
                continue;
            }

            const bool array = field.value.kind == ValueKind::Array;
            for (Record* record = records_; record; record = record->next) {
                if (record->schema.id == value.type) {
                    depth = record->depth + 1 > depth ? record->depth + 1 : depth;
                    strongReference = strongReference || record->containsStrongReference;
                    required = required || (!array && record->containsRequired &&
                                            !HasFlag(field.flags, FieldFlags::Transient));
                    requiresWorldOnlyOwner =
                        requiresWorldOnlyOwner || record->requiresWorldOnlyOwner;
                    referenceArray = referenceArray || (array ? record->containsStrongReference
                                                              : record->containsReferenceArray);
                    break;
                }
            }
        }

        // 수동 등록의 깊이 방어(MetaCompiler RMC2303이 먼저 막는다)와 Trace 계약.
        if (depth > MaxStructDepth || (ops.trace != nullptr) != strongReference) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }

        mutating_ = true;
    }

    auto memory = allocator_.Allocate(bytes, alignof(Record), 0x54595045u);
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
    record->generation = nextGeneration_++;
    record->next = records_;
    records_ = record;
    for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
        if (fieldTypes[fieldIndex]) {
            ++fieldTypes[fieldIndex]->dependents;
        }
    }

    mutating_ = false;
    return Result<TypeRegistrationToken>::Success({domain_, record->generation, schema.id});
}

Result<TypeLease> TypeRegistry::Acquire(TypeId type) noexcept {
    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    for (Record* record = records_; record; record = record->next) {
        if (record->schema.id == type) {
            // 기존 호출자(ObjectRegistry·그래프 저장·복원)는 모두 클래스를 가정한다.
            if (record->kind != TypeKind::Class) {
                return Result<TypeLease>::Failure(ErrorCode::TypeMismatch);
            }

            if (record->leases == 0xffffffffu) {
                return Result<TypeLease>::Failure(ErrorCode::Overflow);
            }

            ++record->leases;
            return Result<TypeLease>::Success(TypeLease(this, record));
        }
    }

    return Result<TypeLease>::Failure(ErrorCode::NotFound);
}

Result<TypeLease> TypeRegistry::AcquireValueType(TypeId type, TypeKind kind) noexcept {
    if (kind == TypeKind::Class) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    for (Record* record = records_; record; record = record->next) {
        if (record->schema.id == type) {
            if (record->kind != kind) {
                return Result<TypeLease>::Failure(ErrorCode::TypeMismatch);
            }

            if (record->leases == 0xffffffffu) {
                return Result<TypeLease>::Failure(ErrorCode::Overflow);
            }

            ++record->leases;
            return Result<TypeLease>::Success(TypeLease(this, record));
        }
    }

    return Result<TypeLease>::Failure(ErrorCode::NotFound);
}

Result<TypeLease> TypeRegistry::Clone(void* pointer) noexcept {
    platform::LockGuard lock(gate_);
    auto* record = static_cast<Record*>(pointer);
    if (closed_ || record->leases == 0xffffffffu) {
        return Result<TypeLease>::Failure(closed_ ? ErrorCode::ShuttingDown : ErrorCode::Overflow);
    }

    ++record->leases;
    return Result<TypeLease>::Success(TypeLease(this, record));
}

void TypeRegistry::Release(void* pointer) noexcept {
    platform::LockGuard lock(gate_);
    auto* record = static_cast<Record*>(pointer);
    if (record->leases == 0) {
        platform::Fatal("TypeLease reference underflow");
    }

    --record->leases;
}

void TypeRegistry::DestroyRecord(Record* record) noexcept {
    // 의존자(파생 클래스·enum·struct를 쓰는 클래스·struct)는 의존 대상보다 먼저
    // 파괴된다(Unregister는 Busy로 막고, Shutdown은 등록의 역순으로 파괴한다).
    if (record->dependents != 0) {
        platform::Fatal("TypeRegistry record destroyed before its dependents");
    }

    if (record->base) {
        platform::LockGuard lock(gate_);
        if (record->base->dependents == 0) {
            platform::Fatal("TypeRegistry base dependency underflow");
        }

        --record->base->dependents;
    }

    if (record->fieldTypes) {
        platform::LockGuard lock(gate_);
        for (usize index = 0; index < record->schema.fields.Size(); ++index) {
            Record* target = record->fieldTypes[index];
            if (!target) {
                continue;
            }

            if (target->dependents == 0) {
                platform::Fatal("TypeRegistry value type dependency underflow");
            }

            --target->dependents;
        }
    }

    record->~Record();
    auto result = allocator_.Deallocate(record);
    if (!result) {
        platform::Fatal("TypeRegistry allocation ownership mismatch");
    }
}

Result<void> TypeRegistry::Unregister(TypeRegistrationToken token) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    Record* removed = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (mutating_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        if (token.registry != domain_) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        Record** previous = &records_;
        while (*previous && !((*previous)->schema.id == token.type)) {
            previous = &(*previous)->next;
        }

        if (!*previous || (*previous)->generation != token.generation) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if ((*previous)->leases != 0 || (*previous)->dependents != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        removed = *previous;
        *previous = removed->next;
        mutating_ = true;
    }

    // ModuleLease 정리와 할당기 콜백은 재진입할 수 있으므로 Registry 잠금 밖에서 실행한다.
    // mutating_은 콜백이 등록/해제를 다시 시작하지 못하게 한다.
    DestroyRecord(removed);
    {
        platform::LockGuard lock(gate_);
        mutating_ = false;
    }

    return Result<void>::Success();
}

Result<void> TypeRegistry::Shutdown() noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    Record* removed = nullptr;
    {
        platform::LockGuard lock(gate_);
        if (mutating_) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        closed_ = true;
        for (Record* record = records_; record; record = record->next) {
            if (record->leases != 0) {
                return Result<void>::Failure(ErrorCode::Busy);
            }
        }

        removed = records_;
        records_ = nullptr;
        mutating_ = true;
    }

    // base·enum·struct가 먼저 등록되어야 하므로 head 순서는 의존자→의존 대상이다. 외부 lease만
    // 검사하고 이 순서로 dependent를 반환하여 내부 의존 관계 때문에 Shutdown이 영구 Busy가 되는
    // 일을 피한다.
    while (removed) {
        Record* next = removed->next;
        DestroyRecord(removed);
        removed = next;
    }

    {
        platform::LockGuard lock(gate_);
        mutating_ = false;
    }

    return Result<void>::Success();
}

TypeStats TypeRegistry::Stats() const noexcept {
    platform::LockGuard lock(gate_);
    TypeStats stats;
    for (Record* record = records_; record; record = record->next) {
        ++stats.registeredTypes;
        stats.leases += record->leases;
    }

    return stats;
}

} // namespace rocks
