#pragma once

#include <Core/Result.h>
#include <Object/Object.h>
#include <Object/ObjectAccessContext.h>

namespace rocks {

class ObjectRegistry;
class ConstructionContext;

namespace detail {
Result<void> RetainRoot(ObjectRegistry*, ObjectHandle) noexcept;
void ReleaseRoot(ObjectRegistry*, ObjectHandle) noexcept;
Result<void*> AccessBorrow(ObjectRegistry*, ObjectHandle, const void*) noexcept;
Result<ObjectLifecycle> InspectBorrow(ObjectRegistry*, ObjectHandle) noexcept;
void ReleaseBorrow(ObjectRegistry*, ObjectHandle) noexcept;
void ReleaseBackend(ObjectRegistry*, ObjectHandle) noexcept;
} // namespace detail

// GC의 외부 루트. 포인터 접근권과 구분하며 실제 접근은 Borrow를 사용한다.
// Registry가 모든 RootRef보다 오래 살아야 한다. Reset은 계수만 낮추고 즉시 파괴하지 않는다.
// 서로 다른 래퍼의 Clone/Reset은 다른 스레드에서도 허용한다. 같은 래퍼의 동시 변경은 금지한다.
template <class T = Object>
class RootRef {
public:
    RootRef() noexcept = default;

    RootRef(RootRef&& other) noexcept : registry_(other.registry_), handle_(other.handle_) {
        other.registry_ = nullptr;
        other.handle_ = {};
    }

    RootRef& operator=(RootRef&& other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = other.registry_;
            handle_ = other.handle_;
            other.registry_ = nullptr;
            other.handle_ = {};
        }
        return *this;
    }

    RootRef(const RootRef&) = delete;
    RootRef& operator=(const RootRef&) = delete;

    ~RootRef() {
        Reset();
    }

    // 기존 소유권을 복제한다. DestroyRequested에서도 저장소 보존은 가능하지만 접근은 불가하다.
    // GC/콜백 처리 중에는 Busy, 종료 시작 후에는 ShuttingDown을 반환한다.
    Result<RootRef> Clone() const noexcept {
        auto result = detail::RetainRoot(registry_, handle_);
        if (!result) {
            return Result<RootRef>::Failure(result.Error());
        }
        return Result<RootRef>::Success(RootRef(registry_, handle_));
    }

    void Reset() noexcept {
        if (registry_) {
            detail::ReleaseRoot(registry_, handle_);
            registry_ = nullptr;
            handle_ = {};
        }
    }

    ObjectHandle Handle() const noexcept {
        return handle_;
    }

    // 래퍼가 루트를 보유하는지만 뜻한다. 객체가 논리적으로 Alive라는 뜻은 아니다.
    bool IsValid() const noexcept {
        return registry_ != nullptr;
    }

private:
    friend class ObjectRegistry;
    friend class ConstructionContext;

    RootRef(ObjectRegistry* registry, ObjectHandle handle) noexcept
        : registry_(registry), handle_(handle) {
    }

    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

// 소유 스레드에서만 역참조할 수 있는 임시 접근권이다. 활성 Borrow는 전체 GC를 막는다.
// Get의 포인터를 Borrow 밖으로 유출하거나 다른 스레드에 전달하지 않는다.
// Reset만 다른 스레드에서도 가능하며, Registry는 모든 Borrow보다 오래 살아야 한다.
// 래퍼 소유권을 안전하게 이관한 뒤 반환한다. 같은 래퍼의 Get/이동/Reset을 동시에 호출하지 않는다.
template <class T = Object>
class Borrow {
public:
    Borrow() noexcept = default;

    Borrow(Borrow&& other) noexcept : registry_(other.registry_), handle_(other.handle_) {
        other.registry_ = nullptr;
        other.handle_ = {};
    }

    Borrow& operator=(Borrow&& other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = other.registry_;
            handle_ = other.handle_;
            other.registry_ = nullptr;
            other.handle_ = {};
        }
        return *this;
    }

    Borrow(const Borrow&) = delete;
    Borrow& operator=(const Borrow&) = delete;

    ~Borrow() {
        Reset();
    }

    // 매 호출에서 타입·스레드·논리 생존 상태를 확인한다. 파괴 요청 후에는 접근 실패한다.
    // 명시 scope가 있으면 다른 World의 Borrow를 거부한다. scope가 없을 때는 호출자의 World를
    // 판별할 수 없으며, requireLifecycleScope는 이 Get 호출에 scope를 요구하지 않는다.
    // 이미 꺼낸 포인터는 이 Borrow가 살아 있는 동안 Object의 수명 상태 조회와 멱등 Destroy에만
    // 사용할 수 있다. 다른 객체 접근과 Borrow 종료 뒤의 포인터 사용은 금지다.
    Result<T*> Get() const noexcept {
        auto result = detail::AccessBorrow(registry_, handle_, NativeTypeKey<T>());
        if (!result) {
            return Result<T*>::Failure(result.Error());
        }
        return Result<T*>::Success(static_cast<T*>(result.Value()));
    }

    // 파괴 요청 후에도 이미 보유한 Borrow로 상태만 조회한다. 스레드·슬롯을 확인하고 명시된
    // scope가 있으면 그 문맥도 검사한다. scope가 없으면 기본 Registry 문맥을 쓰지만 호출자의
    // World는 식별할 수 없고, 엄격 모드에서는 실패한다. 새 포인터를 제공하지 않으며 같은 래퍼의
    // 동시 변경은 금지다.
    Result<ObjectLifecycle> InspectLifecycle() const noexcept {
        return detail::InspectBorrow(registry_, handle_);
    }

    void Reset() noexcept {
        if (registry_) {
            detail::ReleaseBorrow(registry_, handle_);
            registry_ = nullptr;
            handle_ = {};
        }
    }

private:
    friend class ObjectRegistry;

    Borrow(ObjectRegistry* registry, ObjectHandle handle) noexcept
        : registry_(registry), handle_(handle) {
    }

    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

// 객체 안에 직접 포함하는 강한 GC 간선이다. 자체적으로 루트가 되지는 않는다.
// Registry/ConstructionContext의 SetReference로만 변경하고 소유자의 Trace에서 방문한다.
// 복사·이동으로 소유자 검증을 우회하지 못하게 한다. 외부 배열 안의 간선은 아직 미지원이다.
template <class T = Object>
class ObjectRef {
public:
    ObjectRef() noexcept = default;
    ObjectRef(const ObjectRef&) = delete;
    ObjectRef& operator=(const ObjectRef&) = delete;
    ObjectRef(ObjectRef&&) = delete;
    ObjectRef& operator=(ObjectRef&&) = delete;

    ObjectHandle Handle() const noexcept {
        return handle_;
    }

private:
    friend class ObjectRegistry;
    friend class ConstructionContext;
    ObjectHandle handle_;
};

// Registry 포인터를 보관하지 않는 약한 값 참조다. 대상의 수명을 연장하지 않는다.
// 살아 있는 Registry와 문맥을 명시해 TryPin하며, 폐기·재사용된 핸들은 거부한다.
template <class T = Object>
class WeakRef {
public:
    WeakRef() noexcept = default;

    explicit WeakRef(ObjectHandle handle) noexcept : handle_(handle) {
    }

    Result<RootRef<T>> TryPin(ObjectRegistry&, const ObjectAccessContext&) const noexcept;

    ObjectHandle Handle() const noexcept {
        return handle_;
    }

private:
    ObjectHandle handle_;
};

// 비동기 백엔드 사용이 끝날 때까지 해당 객체 저장소의 회수만 지연한다.
// GC 루트나 객체/자식 접근권은 아니다. 현재 실제 GPU/IO 작업과 연결되지는 않는다.
// Reset은 다른 스레드에서도 가능하다. Registry는 모든 lease 반환까지 유지해야 한다.
// 래퍼 소유권을 안전하게 이관한 뒤 반환한다. 같은 래퍼의 이동/Reset을 동시에 호출하지 않는다.
class BackendLease {
public:
    BackendLease() noexcept = default;

    BackendLease(BackendLease&& other) noexcept
        : registry_(other.registry_), handle_(other.handle_) {
        other.registry_ = nullptr;
    }

    BackendLease& operator=(BackendLease&& other) noexcept {
        if (this != &other) {
            Reset();
            registry_ = other.registry_;
            handle_ = other.handle_;
            other.registry_ = nullptr;
        }
        return *this;
    }

    BackendLease(const BackendLease&) = delete;
    BackendLease& operator=(const BackendLease&) = delete;

    ~BackendLease() {
        Reset();
    }

    void Reset() noexcept {
        if (registry_) {
            detail::ReleaseBackend(registry_, handle_);
            registry_ = nullptr;
            handle_ = {};
        }
    }

private:
    friend class ObjectRegistry;

    BackendLease(ObjectRegistry* registry, ObjectHandle handle) noexcept
        : registry_(registry), handle_(handle) {
    }

    ObjectRegistry* registry_ = nullptr;
    ObjectHandle handle_;
};

} // namespace rocks
