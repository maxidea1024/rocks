#pragma once

#include <Core/Memory/Allocator.h>
#include <Meta/TypeSchema.h>
#include <Object/RuntimeTypeOps.h>
#include <Object/TypeLease.h>
#include <Platform/Sync.h>

namespace rocks {

class TypeRegistry;

struct TypeRegistrationToken {
    uint64 registry = 0;
    uint32 generation = 0;
    TypeId type = {};
};

struct TypeStats {
    uint32 registeredTypes = 0;
    uint32 leases = 0;
};

// 등록·해제·Shutdown은 생성 스레드 전용이다. Acquire/Stats/lease 계수는 잠금으로 보호한다.
// Allocator는 Registry보다 오래 살아야 하며 소멸 전 모든 lease를 반환해야 한다.
// 콜백 코드는 입력 계약을 신뢰한다. 임의 플러그인의 메모리 안전성을 검증하지 않는다.
class TypeRegistry {
public:
    explicit TypeRegistry(Allocator& allocator) noexcept;
    ~TypeRegistry();
    TypeRegistry(const TypeRegistry&) = delete;
    TypeRegistry& operator=(const TypeRegistry&) = delete;

    // 입력 버퍼는 호출 동안 유효해야 한다. 실패하면 기존 등록 상태를 유지한다.
    // ops는 값으로 소유권을 넘기므로 등록 실패 시에도 그 ModuleLease는 정리된다.
    Result<TypeRegistrationToken> Register(const TypeSchema&, RuntimeTypeOps) noexcept;
    Result<TypeLease> Acquire(TypeId) noexcept;
    Result<bool> IsA(TypeId actual, TypeId expected) noexcept;
    Result<void> Unregister(TypeRegistrationToken) noexcept;
    // 신규 Acquire/Clone을 닫는다. lease가 남으면 Busy이며, 반환 후 다시 호출한다.
    // Busy를 무시한 Registry 파괴는 강제 해제 대신 즉시 종료한다.
    Result<void> Shutdown() noexcept;
    TypeStats Stats() const noexcept;

private:
    friend class TypeLease;
    struct Record;
    Result<TypeLease> Clone(void*) noexcept;
    void Release(void*) noexcept;
    void DestroyRecord(Record*) noexcept;

    Allocator& allocator_;
    mutable platform::Mutex gate_;
    Record* records_ = nullptr;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    uint32 nextGeneration_ = 1;
    bool closed_ = false;
    bool mutating_ = false;
};

} // namespace rocks
