#pragma once

#include "ObjectRegistryInternal.h"

#include <Object/ObjectRegistry.h>

namespace rocks::detail {

// Registry가 잠금 안에서 슬롯을 본 누적 횟수다. 성능 회귀를 시간 대신 걸음 수로 확인한다(한 번의
// 생성·회수가 보는 슬롯 수가 저장소 크기와 무관한지).
struct RegistryWorkCounters {
    uint64 freeSlotProbes = 0; // 생성이 빈 슬롯을 고르며 본 슬롯 수(생성마다 1)
    uint64 reclaimProbes = 0;  // 회수 대상 목록에서 본 슬롯 수(회수한 것과 지연으로 분류한 것)
};

// ObjectRegistry 내부 계수를 읽는 시험 통로다(friend). 제품 코드는 쓰지 않는다.
struct ObjectRegistryAccess {
    static RegistryWorkCounters WorkCounters(const ObjectRegistry& registry) noexcept {
        platform::LockGuard lock(registry.gate_);
        return {registry.freeSlotProbes_, registry.reclaimProbes_};
    }

    // 소유자 범위 검사가 hot의 object와 타입 기반 위치(TypeLease::ObjectOffset)로 유도하는 저장소
    // 시작이다. handle은 생성된 객체여야 한다.
    static uptr DerivedStorageStart(const ObjectRegistry& registry, ObjectHandle handle) noexcept {
        platform::LockGuard lock(registry.gate_);
        return ObjectRegistry::StorageStartOf(*registry.FindSlot(handle));
    }

    // 생성이 cold에 기록한 실제 저장소 시작이다.
    static uptr RecordedStorageStart(const ObjectRegistry& registry, ObjectHandle handle) noexcept {
        platform::LockGuard lock(registry.gate_);
        return reinterpret_cast<uptr>(registry.ColdOf(*registry.FindSlot(handle)).storage);
    }
};

} // namespace rocks::detail
