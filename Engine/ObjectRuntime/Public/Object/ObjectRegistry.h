#pragma once

#include <Core/Containers/Array.h>
#include <Object/ObjectRefs.h>
#include <Object/ObjectDiagnostics.h>
#include <Object/TypeRegistry.h>

namespace rocks {

// 한도는 생성 시 고정한다. maxGeneration을 낮추면 슬롯 고갈 회귀 시험을 할 수 있다.
struct RegistryOptions {
    uint32 maxSlots = 1048576;
    uint32 maxGeneration = 0xffffffffu;
};

struct ObjectStats {
    uint32 liveObjects = 0;
    uint32 reservedObjects = 0;
    uint32 destroyRequested = 0;
    uint32 retiredObjects = 0;
    uint32 roots = 0;
    uint32 borrows = 0;
    uint32 backendHolds = 0;
    uint32 chunks = 0;
};

// 성공한 한 번의 full STW GC 결과다. 시간 단위는 ns이며 이후 drain 결과는 포함하지 않는다.
struct GcReport {
    uint32 examined = 0;
    uint32 marked = 0;
    uint32 retired = 0;
    uint32 reclaimed = 0;
    uint64 durationNanoseconds = 0;
};

// 비이동 객체 저장소와 GC를 소유한다. Allocator/TypeRegistry가 먼저 생성되고 나중에 파괴되어야
// 한다. 객체 접근·변경·GC는 생성 스레드 전용이다. gate는 통계와 참조 계수를 보호할 뿐, 사용자 객체
// 필드를 스레드 안전하게 만들지 않는다. 별도 래퍼의 반환/복제 예외는 ObjectRefs 참고. 종료 시 모든
// 루트·Borrow·BackendLease를 반환한다. 남은 소유권을 무시한 파괴는 즉시 종료한다.
class ObjectRegistry {
public:
    ObjectRegistry(Allocator&, TypeRegistry&, ContextId, RegistryOptions = {}) noexcept;
    ~ObjectRegistry();
    ObjectRegistry(const ObjectRegistry&) = delete;
    ObjectRegistry& operator=(const ObjectRegistry&) = delete;

    ObjectAccessContext OwnerAccess() const noexcept {
        return {context_};
    }

    // 기본 생성과 공개를 한 트랜잭션으로 실행한다. 실패 가능한 별도 초기화에는 ConstructionContext
    // 사용.
    template <class T>
    Result<RootRef<T>> NewObject(const ObjectAccessContext& access, TypeId type) noexcept {
        auto root = NewRoot(access, type, NativeTypeKey<T>());
        if (!root) {
            return Result<RootRef<T>>::Failure(root.Error());
        }
        RootRef<T> typed(this, root.Value().handle_);
        root.Value().registry_ = nullptr;
        root.Value().handle_ = {};
        return Result<RootRef<T>>::Success(Move(typed));
    }

    template <class T = Object>
    Result<Borrow<T>> Resolve(const ObjectAccessContext& access, ObjectHandle handle) noexcept {
        auto result = AddBorrow(access, handle, NativeTypeKey<T>());
        if (!result) {
            return Result<Borrow<T>>::Failure(result.Error());
        }
        return Result<Borrow<T>>::Success(Borrow<T>(this, handle));
    }

    template <class T = Object>
    Result<RootRef<T>> TryPin(const ObjectAccessContext& access, ObjectHandle handle) noexcept {
        auto result = AddRoot(access, handle, NativeTypeKey<T>());
        if (!result) {
            return Result<RootRef<T>>::Failure(result.Error());
        }
        return Result<RootRef<T>>::Success(RootRef<T>(this, handle));
    }

    // owner 저장소 안에 직접 포함된 field만 허용한다. 대상 타입/문맥/생존 검증 실패 시 field는
    // 불변이다. 현재 full STW 구현의 간선 변경 경계이며, 점진 GC의 write barrier는 아직 구현하지
    // 않았다.
    template <class T>
    Result<void> SetReference(const ObjectAccessContext& access, ObjectHandle owner,
                              ObjectRef<T>& field, ObjectHandle target) noexcept {
        return SetEdge(access, owner, field.handle_, target, 0, NativeTypeKey<T>());
    }

    // 논리적 접근을 즉시 닫는다. 기존 루트/간선은 GC에서 추적되어 보관된 자식도 보존한다.
    // 저장소 파괴는 도달성 및 모든 보유 계수 해소 뒤 Collect/DrainRetired에서 수행한다.
    Result<void> RequestDestroy(const ObjectAccessContext&, ObjectHandle) noexcept;
    Result<BackendLease> AcquireBackendLease(const ObjectAccessContext&, ObjectHandle) noexcept;
    // 소유 스레드 full STW 수집. Borrow/생성 트랜잭션/재진입 중에는 Busy다.
    // 작업 큐 할당이나 Trace 간선 검증 실패 시 부분 표식으로 sweep하지 않는다.
    Result<GcReport> Collect(const ObjectAccessContext&) noexcept;
    Result<uint32> DrainRetired(const ObjectAccessContext&) noexcept;
    // 신규 객체/루트/접근권을 닫은 후 회수를 시도한다. Busy면 보유권을 반환하고 재시도한다.
    // 접근 검증/재진입 거부는 닫기 전에 반환한다. 닫은 뒤의 회수 실패는 닫힌 상태를 유지한다.
    // 보유권이 남은 동안 Registry를 먼저 파괴해서는 안 된다.
    Result<void> Shutdown(const ObjectAccessContext&) noexcept;
    ObjectStats Stats() const noexcept;
    // 잠금 안에서 호출자 버퍼에 스냅샷을 복사한다. 할당하지 않으며 용량 부족 시 부분 기록하지
    // 않는다.
    Result<uint32> CaptureDiagnostics(Span<ObjectDiagnosticRecord>) const noexcept;
    // 최초 성공 전에는 0 값이다. 실패한 GC는 마지막 성공 결과를 덮어쓰지 않는다.
    GcReport LastGcReport() const noexcept;

private:
    friend class ConstructionContext;
    friend Result<void> detail::RetainRoot(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseRoot(ObjectRegistry*, ObjectHandle) noexcept;
    friend Result<void*> detail::AccessBorrow(ObjectRegistry*, ObjectHandle, const void*) noexcept;
    friend void detail::ReleaseBorrow(ObjectRegistry*, ObjectHandle) noexcept;
    friend void detail::ReleaseBackend(ObjectRegistry*, ObjectHandle) noexcept;

    struct Slot;
    struct Chunk;
    struct MarkState;
    static void MarkEdge(void*, ObjectHandle) noexcept;
    static void ValidateStagedEdge(void*, ObjectHandle) noexcept;
    Slot* FindSlot(ObjectHandle) const noexcept;
    Slot* SlotAt(uint32) const noexcept;
    Result<void> CheckAccess(const ObjectAccessContext&) const noexcept;
    Result<void> CheckLive(Slot*, const void*) const noexcept;
    Result<void> AddRoot(const ObjectAccessContext&, ObjectHandle, const void*) noexcept;
    Result<void> AddBorrow(const ObjectAccessContext&, ObjectHandle, const void*) noexcept;
    Result<void> SetEdge(const ObjectAccessContext&, ObjectHandle, ObjectHandle&, ObjectHandle,
                         uint64, const void*) noexcept;
    Result<RootRef<Object>> NewRoot(const ObjectAccessContext&, TypeId, const void*) noexcept;
    Result<uint64> BeginConstruction(const ObjectAccessContext&) noexcept;
    Result<ObjectHandle> CreatePending(const ObjectAccessContext&, TypeId, uint64,
                                       const void*) noexcept;
    Result<void*> GetPending(const ObjectAccessContext&, ObjectHandle, uint64,
                             const void*) noexcept;
    Result<RootRef<Object>> Commit(const ObjectAccessContext&, uint64,
                                   Span<const ObjectHandle>) noexcept;
    void Rollback(uint64, Span<const ObjectHandle>) noexcept;
    uint32 ReclaimEligible() noexcept;

    Allocator& allocator_;
    TypeRegistry& types_;
    ContextId context_;
    RegistryOptions options_;
    uint64 domain_ = 0;
    uint64 ownerThread_ = 0;
    mutable platform::Mutex gate_;
    Chunk* chunks_ = nullptr;
    uint32 chunkCount_ = 0;
    uint32 totalBorrows_ = 0;
    uint32 constructions_ = 0;
    uint64 nextTransaction_ = 1;
    uint64 markEpoch_ = 0;
    GcReport lastGc_;
    // 잠금을 풀고 사용자 코드를 호출하는 구간에도 변경 재진입을 차단하는 상태다.
    // 별도 worker를 실행하거나 병렬 GC 진행을 표시하는 플래그가 아니다.
    bool processing_ = false;
    bool closed_ = false;
};

template <class T>
Result<RootRef<T>> WeakRef<T>::TryPin(ObjectRegistry& registry,
                                      const ObjectAccessContext& access) const noexcept {
    return registry.TryPin<T>(access, handle_);
}

} // namespace rocks
