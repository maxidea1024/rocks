#include <Object/ModuleRegistration.h>

#include <Platform/Fatal.h>

namespace rocks {

ModuleRegistration::ModuleRegistration(TypeRegistry& registry, Allocator& allocator) noexcept
    : registry_(&registry), records_(allocator) {
}

usize ModuleRegistration::Size() const noexcept {
    return records_.Size();
}

Result<void> ModuleRegistration::ReserveRecord() noexcept {
    // 기록 하나를 넣을 자리를 기하급수로 확보한다(한 칸씩 늘리면 이동이 제곱으로 는다).
    const usize capacity = records_.Capacity();
    if (records_.Size() < capacity) {
        return Result<void>::Success();
    }

    const usize grown = capacity < 8               ? 8
                        : capacity > usize(-1) / 2 ? capacity + 1
                                                   : capacity + capacity / 2;
    return records_.TryReserve(grown);
}

Result<bool> ModuleRegistration::EnterModule(const void* key) noexcept {
    if (!key) {
        return Result<bool>::Failure(ErrorCode::InvalidArgument);
    }

    for (const Record& record : records_) {
        if (record.module == key) {
            return Result<bool>::Success(false);
        }
    }

    if (auto reserved = ReserveRecord(); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    Record record;
    record.module = key;
    if (!records_.TryPushBack(Move(record))) {
        platform::Fatal("ModuleRegistration reservation invariant failed");
    }

    return Result<bool>::Success(true);
}

Result<bool> ModuleRegistration::RegisterIfAbsent(const TypeSchema& schema,
                                                  RuntimeTypeOps ops) noexcept {
    // 등록·lease 획득 뒤 기록에 실패하면 되돌려야 하므로 자리를 먼저 확보한다.
    if (auto reserved = ReserveRecord(); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    auto existing = registry_->Acquire(schema.id);
    if (existing) {
        return Retain(Move(existing.Value()), ops.nativeType);
    }

    if (existing.Error() != ErrorCode::NotFound) {
        return Result<bool>::Failure(existing.Error());
    }

    auto token = registry_->Register(schema, Move(ops));
    if (!token) {
        return Result<bool>::Failure(token.Error());
    }

    return Keep(token.Value());
}

Result<bool> ModuleRegistration::RegisterEnumIfAbsent(const EnumSchema& schema,
                                                      RuntimeEnumOps ops) noexcept {
    if (auto reserved = ReserveRecord(); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    auto existing = registry_->AcquireValueType(schema.id, TypeKind::Enum);
    if (existing) {
        return Retain(Move(existing.Value()), ops.nativeType);
    }

    if (existing.Error() != ErrorCode::NotFound) {
        return Result<bool>::Failure(existing.Error());
    }

    auto token = registry_->RegisterEnum(schema, Move(ops));
    if (!token) {
        return Result<bool>::Failure(token.Error());
    }

    return Keep(token.Value());
}

Result<bool> ModuleRegistration::Retain(TypeLease existing, const void* nativeType) noexcept {
    if (existing.Ops().nativeType != nativeType) {
        return Result<bool>::Failure(ErrorCode::AlreadyExists);
    }

    Record record;
    record.retained = Move(existing);
    if (!records_.TryPushBack(Move(record))) {
        platform::Fatal("ModuleRegistration reservation invariant failed");
    }

    return Result<bool>::Success(false);
}

Result<bool> ModuleRegistration::Keep(TypeRegistrationToken token) noexcept {
    Record record;
    record.token = token;
    if (!records_.TryPushBack(Move(record))) {
        platform::Fatal("ModuleRegistration reservation invariant failed");
    }

    return Result<bool>::Success(true);
}

Result<void> ModuleRegistration::UnregisterFrom(usize mark) noexcept {
    if (mark > records_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    while (records_.Size() > mark) {
        Record& last = records_[records_.Size() - 1];
        if (!last.module && !last.retained.IsValid()) {
            if (auto removed = registry_->Unregister(last.token); !removed) {
                return removed;
            }
        }

        // 줄이기는 할당하지 않는다. 붙잡은 lease는 Record 소멸에서 놓인다.
        static_cast<void>(records_.TryResize(records_.Size() - 1));
    }

    return Result<void>::Success();
}

} // namespace rocks
