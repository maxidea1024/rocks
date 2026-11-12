#pragma once

#include <Object/ObjectRegistry.h>
#include <Platform/Fatal.h>

namespace rocks {

// 소유 스레드에 묶인 비공개 생성 트랜잭션. 생성된 객체는 Commit 전 Resolve/TryPin할 수 없다.
// Commit하지 않고 범위를 벗어나면 생성 역순으로 롤백한다. 객체 포인터를 외부에 유출하지 않는다.
// Registry보다 먼저 파괴해야 한다. 현재 활성 트랜잭션이 있으면 Collect는 Busy다.
class ConstructionContext {
public:
    ConstructionContext(ObjectRegistry&, const ObjectAccessContext&) noexcept;
    ~ConstructionContext();
    ConstructionContext(const ConstructionContext&) = delete;
    ConstructionContext& operator=(const ConstructionContext&) = delete;

    template <class T>
    Result<ObjectHandle> Create(TypeId type) noexcept {
        return CreateImpl(type, NativeTypeKey<T>());
    }

    // 같은 트랜잭션의 객체를 초기화할 때만 사용한다. 반환 포인터는 완료 이후 보관하지 않는다.
    // 실패 가능한 초기화 결과를 호출자가 확인하고 실패 시 Commit을 생략한다.
    template <class T>
    Result<T*> Get(ObjectHandle handle) noexcept {
        if (finished_) {
            return Result<T*>::Failure(ErrorCode::InvalidArgument);
        }
        auto result = registry_.GetPending(access_, handle, transaction_, NativeTypeKey<T>());
        if (!result) {
            return Result<T*>::Failure(result.Error());
        }
        return Result<T*>::Success(static_cast<T*>(result.Value()));
    }

    template <class T>
    Result<void> SetReference(ObjectHandle owner, ObjectRef<T>& field,
                              ObjectHandle target) noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
        return registry_.SetEdge(access_, owner, field.handle_, target, transaction_,
                                 NativeTypeKey<T>());
    }

    // 모든 staged 간선을 검증한 뒤 한 번에 공개하고 첫 객체의 루트만 반환한다.
    // 나머지는 이 루트에서 도달하거나 별도로 Pin해야 다음 GC에서 생존한다.
    // 실패 시 비공개 상태를 유지한다. Create 실패는 트랜잭션을 오류 상태로 고정한다.
    Result<RootRef<Object>> Commit() noexcept;
    void Rollback() noexcept;

private:
    friend class ObjectRegistry;
    Result<ObjectHandle> CreateImpl(TypeId type, const void* nativeType) noexcept;

    ObjectRegistry& registry_;
    ObjectAccessContext access_;
    Array<ObjectHandle> staged_;
    uint64 transaction_ = 0;
    ErrorCode error_ = ErrorCode::None;
    bool finished_ = false;
};

} // namespace rocks
