#include <Object/TypeRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"
#include <new>

namespace rocks {

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

const TypeSchema& TypeLease::Schema() const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }
    return static_cast<TypeRegistry::Record*>(record_)->schema;
}

const RuntimeTypeOps& TypeLease::Ops() const noexcept {
    if (!record_) {
        platform::Fatal("Access to an empty TypeLease");
    }
    return static_cast<TypeRegistry::Record*>(record_)->ops;
}

bool TypeLease::IsA(TypeId expected) const noexcept {
    for (auto* record = static_cast<TypeRegistry::Record*>(record_); record;
         record = record->base) {
        if (record->schema.id == expected) {
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
        !ops.construct || !ops.destroy || !ops.trace || !ops.nativeType ||
        ops.size < sizeof(Object) || ops.alignment < alignof(Object) ||
        (ops.alignment & (ops.alignment - 1)) != 0 ||
        !StringView(schema.name.Data(), schema.name.Size()).ValidateUtf8()) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }
    usize typedFields = 0;
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        const auto& field = schema.fields[index];
        if (field.id == Guid{} ||
            (field.kind != FieldKind::Value && field.kind != FieldKind::ObjectReference) ||
            static_cast<uint8>(field.valueKind) > static_cast<uint8>(ValueKind::ObjectReference) ||
            !StringView(field.name.Data(), field.name.Size()).ValidateUtf8()) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
        if (field.valueKind != ValueKind::Opaque) {
            ++typedFields;
            const bool reference = field.valueKind == ValueKind::ObjectReference;
            if (field.name.Size() == 0 || (field.kind == FieldKind::ObjectReference) != reference ||
                (!reference && field.referencedType != TypeId{})) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
            }
        } else if (field.referencedType != TypeId{}) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize previous = 0; previous < index; ++previous) {
            if (field.id == schema.fields[previous].id) {
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
            if (schemaField.id == field.fieldId && schemaField.valueKind != ValueKind::Opaque) {
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
        if (!AddBytes(bytes, field.name.Size()) || !AddBytes(bytes, field.attributes.Size())) {
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
            // 파생 스키마는 base field를 동일한 안정 ID/표현으로 포함한다. 실제 멤버 접근은
            // 파생 native type을 검증하는 generated thunk가 담당하며 이 조회가 cast를 허용하지
            // 않는다.
            for (usize baseIndex = 0; baseIndex < base->schema.fields.Size(); ++baseIndex) {
                const auto& inherited = base->schema.fields[baseIndex];
                bool matched = false;
                for (usize fieldIndex = 0; fieldIndex < schema.fields.Size(); ++fieldIndex) {
                    const auto& field = schema.fields[fieldIndex];
                    if (field.id == inherited.id && field.kind == inherited.kind &&
                        field.valueKind == inherited.valueKind &&
                        field.referencedType == inherited.referencedType) {
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
    record->schema.name = CopyBytes(schema.name, cursor);
    record->schema.fields = {fields, schema.fields.Size()};
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        ::new (fields + index) FieldSchema(schema.fields[index]);
        fields[index].name = CopyBytes(schema.fields[index].name, cursor);
        fields[index].attributes = CopyBytes(schema.fields[index].attributes, cursor);
    }
    for (usize index = 0; index < ops.fields.Size(); ++index) {
        ::new (fieldOps + index) RuntimeFieldOps(ops.fields[index]);
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
