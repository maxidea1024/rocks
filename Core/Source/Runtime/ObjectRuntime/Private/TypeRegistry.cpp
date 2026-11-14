#include "TypeRegistryInternal.h"

#include <Object/TypeRegistry.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include "RegistryIdentity.h"

// TypeRegistry 기반부다. Registry 도메인 번호(detail::NextRegistryDomain), 생성·소멸, 레코드
// 색인(ID·native)과 lease 발급, 조회(Acquire·AcquireValueType·AcquireStructByNative·IsA), lease
// 계수, 해제·종료(Unregister·Shutdown), 통계를 둔다. 등록은 TypeRegistryRegister.cpp,
// TypeLease·StructView 접근은 TypeLease.cpp, 필드 쓰기 검증은 RuntimeTypeOps.cpp다.
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

TypeRegistry::TypeRegistry(Allocator& allocator) noexcept
    : allocator_(allocator), byId_(allocator), byNative_(allocator),
      domain_(detail::NextRegistryDomain()), ownerThread_(platform::CurrentThreadId()) {
}

TypeRegistry::~TypeRegistry() {
    auto result = Shutdown();
    if (!result) {
        platform::Fatal("TypeRegistry destroyed with active leases or on wrong thread");
    }
}

Result<TypeLease> TypeRegistry::LeaseLocked(Record* record) noexcept {
    if (record->leases == NumericLimits<uint32>::Max()) {
        return Result<TypeLease>::Failure(ErrorCode::Overflow);
    }

    ++record->leases;
    return Result<TypeLease>::Success(TypeLease(this, record));
}

TypeRegistry::Record* TypeRegistry::FindLocked(TypeId type) const noexcept {
    Record* const* found = byId_.Find(type);
    return found != nullptr ? *found : nullptr;
}

TypeRegistry::Record* TypeRegistry::FindNativeLocked(const void* nativeType,
                                                     TypeKind kind) const noexcept {
    Record* const* found = byNative_.Find(NativeIndexKey{nativeType, kind});
    return found != nullptr ? *found : nullptr;
}

Result<void> TypeRegistry::PrepareIndexes(IndexGrowth& growth, bool native) noexcept {
    // mutating_ 아래라 색인을 바꾸는 쪽은 이 스레드뿐이다(다른 스레드의 조회는 잠금 안에서 읽기만
    // 한다) — 크기를 잠금 없이 읽어도 경합이 없다. 새 색인은 아직 아무도 보지 않으므로 잠금 밖에서
    // 할당한다. 삽입이 할당하지 않으려면 공간이 있고 지운 칸이 없어야 한다(HashMap 삽입은 지운 칸이
    // 많으면 스스로 재해시한다).
    growth.rebuildId = idErased_ || byId_.Count() + 1 > byId_.Capacity();
    if (growth.rebuildId) {
        if (auto reserved = growth.id.Reserve(byId_.Count() + 1); !reserved) {
            return reserved;
        }
    }

    growth.rebuildNative =
        native && (nativeErased_ || byNative_.Count() + 1 > byNative_.Capacity());
    if (growth.rebuildNative) {
        if (auto reserved = growth.native.Reserve(byNative_.Count() + 1); !reserved) {
            return reserved;
        }
    }

    return Result<void>::Success();
}

void TypeRegistry::PublishLocked(Record* record, IndexGrowth& growth) noexcept {
    // 새 색인으로 바꿀 때는 지금 내용을 옮긴다(예약했으므로 무할당). 옛 색인은 growth가 쥐고 잠금
    // 밖에서 반환한다.
    if (growth.rebuildId) {
        byId_.ForEach([&](const TypeId& id, Record*& value) noexcept {
            if (!growth.id.Insert(TypeId(id), static_cast<Record*>(value))) {
                platform::Fatal("Reserved TypeRegistry id index unexpectedly failed");
            }
        });
        growth.retiredId = Move(byId_);
        byId_ = Move(growth.id);
        idErased_ = false;
    }

    if (!byId_.Insert(TypeId(record->schema.id), static_cast<Record*>(record))) {
        platform::Fatal("Reserved TypeRegistry id index unexpectedly failed");
    }

    if (record->kind != TypeKind::Class) {
        if (growth.rebuildNative) {
            byNative_.ForEach([&](const NativeIndexKey& key, Record*& value) noexcept {
                if (!growth.native.Insert(NativeIndexKey(key), static_cast<Record*>(value))) {
                    platform::Fatal("Reserved TypeRegistry native index unexpectedly failed");
                }
            });
            growth.retiredNative = Move(byNative_);
            byNative_ = Move(growth.native);
            nativeErased_ = false;
        }

        if (!byNative_.Insert(NativeIndexKey{record->ops.nativeType, record->kind},
                              static_cast<Record*>(record))) {
            platform::Fatal("Reserved TypeRegistry native index unexpectedly failed");
        }
    }

    record->generation = nextGeneration_++;
    record->previous = nullptr;
    record->next = records_;
    if (records_ != nullptr) {
        records_->previous = record;
    }

    records_ = record;
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

Result<TypeLease> TypeRegistry::Acquire(TypeId type) noexcept {
    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    Record* record = FindLocked(type);
    if (record == nullptr) {
        return Result<TypeLease>::Failure(ErrorCode::NotFound);
    }

    // 기존 호출자(ObjectRegistry·그래프 저장·복원)는 모두 클래스를 가정한다.
    if (record->kind != TypeKind::Class) {
        return Result<TypeLease>::Failure(ErrorCode::TypeMismatch);
    }

    return LeaseLocked(record);
}

Result<TypeLease> TypeRegistry::AcquireValueType(TypeId type, TypeKind kind) noexcept {
    if (kind == TypeKind::Class) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    Record* record = FindLocked(type);
    if (record == nullptr) {
        return Result<TypeLease>::Failure(ErrorCode::NotFound);
    }

    if (record->kind != kind) {
        return Result<TypeLease>::Failure(ErrorCode::TypeMismatch);
    }

    return LeaseLocked(record);
}

Result<TypeLease> TypeRegistry::AcquireStructByNative(const void* nativeType) noexcept {
    if (nativeType == nullptr) {
        return Result<TypeLease>::Failure(ErrorCode::InvalidArgument);
    }

    platform::LockGuard lock(gate_);
    if (closed_) {
        return Result<TypeLease>::Failure(ErrorCode::ShuttingDown);
    }

    // 등록이 struct native 키를 유일하게 하므로 색인의 레코드가 답이다.
    Record* record = FindNativeLocked(nativeType, TypeKind::Struct);
    if (record == nullptr) {
        return Result<TypeLease>::Failure(ErrorCode::NotFound);
    }

    return LeaseLocked(record);
}

Result<TypeLease> TypeRegistry::Clone(void* pointer) noexcept {
    platform::LockGuard lock(gate_);
    auto* record = static_cast<Record*>(pointer);
    if (closed_ || record->leases == NumericLimits<uint32>::Max()) {
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

        Record* found = FindLocked(token.type);
        if (found == nullptr || found->generation != token.generation) {
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        if (found->leases != 0 || found->dependents != 0) {
            return Result<void>::Failure(ErrorCode::Busy);
        }

        // 목록·색인에서 뗀다(삭제는 할당하지 않는다 — 지운 칸은 다음 등록이 새 색인으로 정리한다).
        removed = found;
        if (removed->previous != nullptr) {
            removed->previous->next = removed->next;
        } else {
            records_ = removed->next;
        }

        if (removed->next != nullptr) {
            removed->next->previous = removed->previous;
        }

        removed->next = nullptr;
        removed->previous = nullptr;
        if (!byId_.Erase(removed->schema.id)) {
            platform::Fatal("TypeRegistry id index lost a record");
        }

        idErased_ = true;
        if (removed->kind != TypeKind::Class) {
            if (!byNative_.Erase(NativeIndexKey{removed->ops.nativeType, removed->kind})) {
                platform::Fatal("TypeRegistry native index lost a record");
            }

            nativeErased_ = true;
        }

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
    // 색인 저장소는 잠금 밖에서 반환한다(잠금 안에서는 빈 색인으로 바꾸기만 한다).
    IdIndex retiredId(allocator_);
    NativeIndex retiredNative(allocator_);
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
        retiredId = Move(byId_);
        retiredNative = Move(byNative_);
        idErased_ = false;
        nativeErased_ = false;
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
