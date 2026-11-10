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

Result<TypeRegistrationToken> TypeRegistry::Register(const TypeSchema& schema,
                                                     RuntimeTypeOps ops) noexcept {
    if (platform::CurrentThreadId() != ownerThread_) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::WrongThread);
    }
    if (schema.id == Guid{} || schema.version == 0 || schema.fields.Size() > 4096 ||
        !ops.construct || !ops.destroy || !ops.trace || !ops.nativeType ||
        ops.size < sizeof(Object) || ops.alignment < alignof(Object) ||
        (ops.alignment & (ops.alignment - 1)) != 0) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (schema.fields[index].id == Guid{}) {
            return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
        }
        for (usize previous = 0; previous < index; ++previous) {
            if (schema.fields[index].id == schema.fields[previous].id) {
                return Result<TypeRegistrationToken>::Failure(ErrorCode::AlreadyExists);
            }
        }
    }

    // 레코드/필드 배열/가변 길이 바이트를 하나의 소유 블록에 배치한다.
    // 입력 scratch 버퍼가 사라져도 스키마를 읽을 수 있고 실패 시 정리할 할당도 하나뿐이다.
    constexpr usize offset =
        (sizeof(Record) + alignof(FieldSchema) - 1) & ~(alignof(FieldSchema) - 1);
    usize bytes = offset + schema.fields.Size() * sizeof(FieldSchema);
    if (!AddBytes(bytes, schema.name.Size())) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::Overflow);
    }
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (!AddBytes(bytes, schema.fields[index].name.Size()) ||
            !AddBytes(bytes, schema.fields[index].attributes.Size())) {
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
        for (Record* record = records_; record; record = record->next) {
            if (record->schema.id == schema.id) {
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
    Record* record = ::new (memory.Value()) Record();
    FieldSchema* fields =
        reinterpret_cast<FieldSchema*>(static_cast<uint8*>(memory.Value()) + offset);
    uint8* cursor = reinterpret_cast<uint8*>(fields + schema.fields.Size());
    record->schema.id = schema.id;
    record->schema.version = schema.version;
    record->schema.name = CopyBytes(schema.name, cursor);
    record->schema.fields = {fields, schema.fields.Size()};
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        ::new (fields + index) FieldSchema();
        fields[index].id = schema.fields[index].id;
        fields[index].kind = schema.fields[index].kind;
        fields[index].flags = schema.fields[index].flags;
        fields[index].name = CopyBytes(schema.fields[index].name, cursor);
        fields[index].attributes = CopyBytes(schema.fields[index].attributes, cursor);
    }
    record->ops = Move(ops);

    platform::LockGuard lock(gate_);
    record->generation = nextGeneration_++;
    record->next = records_;
    records_ = record;
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
        if ((*previous)->leases != 0) {
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
