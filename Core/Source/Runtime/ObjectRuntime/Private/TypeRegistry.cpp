#include <Object/TypeRegistry.h>
#include <Object/ObjectRegistry.h>
#include <Object/SoftObjectRef.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"
#include <new>

namespace rocks {

Result<void> FieldWriteOperation::operator()(ObjectRegistry& registry,
                                             const ObjectAccessContext& access, ObjectHandle handle,
                                             const FieldValue& value) const noexcept {
    if (!callback_) {
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
    const auto fields = type.Value().Schema().fields;
    for (usize index = 0; index < fields.Size(); ++index) {
        if (fields[index].id == fieldId_) {
            actual = &fields[index];
            break;
        }
    }
    if (!actual) {
        return Result<void>::Failure(ErrorCode::NotFound);
    }
    if (actual->value.kind != valueKind_ || value.kind != valueKind_) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }
    // 검증한 참조 타입은 callback 완료까지 유지한다. 그 전에 해제하면 callback 재진입에서
    // 같은 ID의 타입을 unregister/재등록해 검증과 실제 필드 게시의 기준이 달라질 수 있다.
    TypeLease sourceExpected;
    TypeLease targetExpected;
    if (valueKind_ == ValueKind::ObjectReference) {
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
        const void* sourceKey =
            sourceExpected.IsValid() ? sourceExpected.Ops().nativeType : nullptr;
        const void* targetKey =
            targetExpected.IsValid() ? targetExpected.Ops().nativeType : nullptr;
        if (sourceKey && targetKey && sourceKey != targetKey) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
        if (value.reference != ObjectHandle{}) {
            if (value.reference.IsNull()) {
                return Result<void>::Failure(ErrorCode::StaleHandle);
            }
            auto referenced = registry.AcquireType(access, value.reference);
            if (!referenced) {
                return Result<void>::Failure(referenced.Error());
            }
            // 선언 타입 또는 그 파생 타입 객체를 허용한다(SetReference와 같은 상속 규칙).
            if ((sourceKey && !referenced.Value().IsNativeA(sourceKey)) ||
                (targetKey && !referenced.Value().IsNativeA(targetKey))) {
                return Result<void>::Failure(ErrorCode::TypeMismatch);
            }
        }
    }
    if (readOnly_ || HasFlag(actual->flags, FieldFlags::ReadOnly)) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }
    // 값 내용 검사는 읽기 전용 정책 뒤다(정수 폭 Overflow를 callback이 검사하는 것과 같은 순서).
    // 소프트 참조의 null 규칙(asset 0이면 경로 없음)·경로 상한은 수동 callback도 지키도록 여기서
    // 검사한다. 기대 타입(value.type)은 대상 에셋을 로드하지 않으므로 쓰기에서 검사하지 않는다.
    if (valueKind_ == ValueKind::SoftObjectReference &&
        !SoftObjectRef<>::Validate(AssetId{value.guid}, value.string.View())) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
    return callback_(registry, access, handle, value);
}

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

struct TypeRegistry::Record {
    Record* next = nullptr;
    TypeSchema schema;
    RuntimeTypeOps ops;
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

// 지금 등록할 수 있는 값 종류다. Enum·Struct·Array는 각 단계(B3b·B4·B5)에서 연다.
bool SupportedValueKind(ValueKind kind) noexcept {
    switch (kind) {
    case ValueKind::Enum:
    case ValueKind::Struct:
    case ValueKind::Array:
        return false;
    default:
        return static_cast<uint8>(kind) <= static_cast<uint8>(ValueKind::Digest256);
    }
}

// 도구 이름이 없으면 C++ 필드 이름이 도구 이름이다.
Span<const char> EffectiveToolName(const FieldSchema& field) noexcept {
    return field.toolName.Size() != 0 ? field.toolName : field.name;
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
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const auto& field = schema.fields[index];
        // Required는 typed 참조 필드에만 뜻이 있다(저장이 null을 검사하려면 접근기가 있어야 한다).
        // 도구 이름은 비어 있거나 ASCII 식별자다.
        if (field.id == Guid{} || (field.flags & ~KnownFieldFlags) != 0 ||
            (HasFlag(field.flags, FieldFlags::Required) &&
             field.value.kind != ValueKind::ObjectReference &&
             field.value.kind != ValueKind::SoftObjectReference) ||
            (field.kind != FieldKind::Value && field.kind != FieldKind::ObjectReference) ||
            !SupportedValueKind(field.value.kind) || field.element.kind != ValueKind::Opaque ||
            field.element.type != TypeId{} || field.maxElements != 0 ||
            !StringView(field.name.Data(), field.name.Size()).ValidateUtf8() ||
            !StringView(field.category.Data(), field.category.Size()).ValidateUtf8() ||
            !StringView(field.displayName.Data(), field.displayName.Size()).ValidateUtf8() ||
            (field.toolName.Size() != 0 && !IsAsciiIdentifier(field.toolName))) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
        // FieldKind::ObjectReference(강한 GC 간선) ⇔ value.kind == ObjectReference. value.type은
        // 참조·소프트 참조의 기대 대상에만 쓴다(Enum·Struct는 B3b·B4).
        if (field.value.kind != ValueKind::Opaque) {
            ++typedFields;
            const bool reference = field.value.kind == ValueKind::ObjectReference;
            const bool typed = reference || field.value.kind == ValueKind::SoftObjectReference;
            if (field.name.Size() == 0 || (field.kind == FieldKind::ObjectReference) != reference ||
                (!typed && field.value.type != TypeId{})) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
            }
        } else if (field.value.type != TypeId{}) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize previous = 0; previous < index; ++previous) {
            if (field.id == schema.fields[previous].id) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }

            // 실효 도구 이름(상속 필드 포함)은 대소문자 무시로 유일하다. 이름 없는 Opaque 필드는
            // 제외한다.
            const Span<const char> name = EffectiveToolName(field);
            if (name.Size() != 0 &&
                EqualsIgnoringAsciiCase(name, EffectiveToolName(schema.fields[previous]))) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }
    if (typedFields != ops.fields.Size() ||
        (ops.completeFieldLayout && typedFields != schema.fields.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < ops.fields.Size(); ++index) {
        const auto& field = ops.fields[index];
        bool matched = false;
        for (usize schemaIndex = 0; schemaIndex < schema.fields.Size(); ++schemaIndex) {
            const auto& schemaField = schema.fields[schemaIndex];
            if (schemaField.id == field.fieldId && schemaField.value.kind != ValueKind::Opaque) {
                matched = true;
                break;
            }
        }
        if (!matched || !field.read || !field.write || !field.writeStaged) {
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
    usize bytes = opsOffset + ops.fields.Size() * sizeof(RuntimeFieldOps);
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
                base = record;
            }
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
    uint8* cursor = reinterpret_cast<uint8*>(fieldOps + ops.fields.Size());
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
    if (record->base) {
        platform::LockGuard lock(gate_);
        if (record->base->dependents == 0) {
            platform::Fatal("TypeRegistry base dependency underflow");
        }
        --record->base->dependents;
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
    // base가 먼저 등록되어야 하므로 head 순서는 자식→부모다. 외부 lease만 검사하고 이 순서로
    // dependent를 반환하여 내부 상속 관계 때문에 Shutdown이 영구 Busy가 되는 일을 피한다.
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
