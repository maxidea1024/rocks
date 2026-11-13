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

        // Registry는 Object 하위 객체 주소를 준다. 파생 객체의 기반 T 접근도 static_cast가
        // 보정한다.
        return Result<T*>::Success(static_cast<T*>(static_cast<Object*>(result.Value())));
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

    // 이 트랜잭션의 staged 객체 owner 안 ObjectRefArray 내용을 targets로 통째로 바꾼다(그래프
    // 로드가 원소마다 부르지 않고 한 번에 채운다). owner는 이 트랜잭션의 Reserved여야 하고(Borrow
    // 불요 — Alive·다른 트랜잭션이면 StaleHandle), 배열은 owner 저장소 안이어야 한다(아니면
    // InvalidArgument). 원소는 null(ObjectHandle{}로 정규화), 생존 객체, 또는 이 트랜잭션의
    // Reserved 객체다(그 밖 StaleHandle — GC cutoff 포함, T의 파생이 아니면 TypeMismatch). 용량이
    // 모자라면 레지스트리 처리 표시(processing_) 아래 잠금 밖에서 새 버퍼를 할당한다(그동안의
    // Registry 재진입은 Busy). 생존 대상만 shade하고 Reserved 대상은 Commit이 shade한다. Commit은
    // owner Trace로 원소를 다시 검증한다. 실패하면 배열 내용은 바뀌지 않는다. 트랜잭션이 끝났거나
    // 오류 상태면 InvalidArgument다.
    //
    // 사용 예(owner·actor는 같은 트랜잭션의 Create·Get 결과):
    //   const ObjectHandle components[] = {first, {}, second};
    //   if (auto assigned = transaction.AssignReferences(owner, actor->components_, components);
    //       !assigned) {
    //       return assigned;
    //   }
    template <class T>
    Result<void> AssignReferences(ObjectHandle owner, ObjectRefArray<T>& array,
                                  Span<const ObjectHandle> targets) noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return registry_.AssignReferencesStaged(access_, owner, array, targets, transaction_);
    }

    // staged owner 안 StructRefArray를 count개의 기본 원소로 통째로 바꾼다. owner 규칙은
    // AssignReferences와 같다. 블록 할당·원소 생성·옛 원소 소멸은 처리 표시 아래 잠금 밖에서 하며,
    // 하나라도 실패하면 만든 원소를 모두 되돌리고 배열은 바뀌지 않는다(옛 원소는 게시 뒤 소멸한다).
    // count가 uint32 상한을 넘으면 Overflow다.
    //
    // 사용 예:
    //   if (auto assigned = transaction.AssignElements(owner, mesh->materials, count); !assigned) {
    //       return assigned;
    //   }
    //
    //   return transaction.SetElementReference(owner, mesh->materials, 0,
    //                                          mesh->materials[0].material, material);
    template <class S>
    Result<void> AssignElements(ObjectHandle owner, StructRefArray<S>& array,
                                usize count) noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return registry_.AssignElementsStaged(access_, owner, array, count, transaction_);
    }

    // staged owner 안 StructRefArray 원소(element 위치)의 참조 멤버 field에 target을 쓴다
    // (SetReference의 원소판). 컨테이너가 owner 저장소 안·index < size·field가 그 원소 블록
    // 안이어야 한다(아니면 InvalidArgument). 대상 규칙은 SetReference와 같다(null·생존·이
    // 트랜잭션의 Reserved). element는 detail::StructRefArrayAccess::Location으로 만든다(codec용).
    template <class T>
    Result<void> SetElementReference(ObjectHandle owner,
                                     const detail::StructElementLocation& element,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return registry_.SetEdge(access_, owner, field.handle_, target, transaction_,
                                 NativeTypeKey<T>(), &element);
    }

    // 위와 같고 (배열, index)로 원소를 고른다.
    template <class S, class T>
    Result<void> SetElementReference(ObjectHandle owner, StructRefArray<S>& array, usize index,
                                     ObjectRef<T>& field, ObjectHandle target) noexcept {
        if (finished_ || error_ != ErrorCode::None) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return registry_.SetElementReferenceStaged(access_, owner, array, index, field, target,
                                                   transaction_);
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
