#pragma once

#include <Core/Memory/Allocator.h>
#include <Object/ObjectHandle.h>
#include <Object/TypeLease.h>

// 경로 연산(Object/PropertyPath.h의 PathRead·PathWrite 등)의 시작 위치다(리플렉션 B9i3c0 —
// Docs/Design/ReflectionExtensions.ko.md 10.2절). 게시된 생존 객체, 트랜잭션의 staged 객체, 객체
// 밖의 분리된 값(struct 값 하나 또는 필드·멤버 값 하나를 담은 DetachedValue 상자)에서 경로를
// 시작한다. 대입 묶음(B9i3c)이 값 부분 트리의 사본을 고치고 게시 때 옮기는 데 쓴다.
namespace rocks {

class ConstructionContext;
class DetachedValue;
class PreparedReferences;

// 경로 연산의 시작 위치다. ObjectHandle에서 암묵 변환되므로 기존 호출(PathRead(registry, access,
// handle, ...))은 그대로 게시된 객체에서 시작한다. 값 타입이며 가리키는 트랜잭션·struct 값·상자를
// 소유하지 않는다 — 그것들은 연산이 끝날 때까지 살아 있어야 한다.
//
// 시작 위치마다 다른 점(공통 계약은 Object/PropertyPath.h):
// - Object: 게시된 생존 객체다(지금까지의 규칙 그대로).
// - Staged: 트랜잭션(ConstructionContext)의 공개 전 객체다. 타입·객체는 트랜잭션의
//   AcquireType·Get(쓰기는 GetForWrite)으로 얻고 encode 고정·게시 직전 재확인이 없다(공개 전
//   객체는 그 트랜잭션의 소유 스레드만 본다). 참조 멤버 쓰기는 트랜잭션의 SetReference·
//   SetElementReference(staged 간선)이고, Registry 컨테이너(ObjectRefArray·StructRefArray·
//   ObjectRefMap)의 변경은 Unsupported(NoPathAccess)다(트랜잭션의 Assign*을 직접 쓰거나 준비 사본 —
//   WithReferences — 을 건다). 최상위 단일
//   값 필드도 경로 thunk로 접근한다(등록된 read는 공개된 객체만 읽는다). registry·access 인자는
//   트랜잭션의 것이어야 한다(아니면 WrongContext). ReadOnly는 막지 않는다(공개 전 객체의 초기값 —
//   리플렉션 B9i3c1, staged 객체에서 분리한 상자도 같다).
// - Value(struct 값): 등록 struct의 값 하나(view — lease가 준 view, storage — 그 struct의 살아 있는
//   C++ 값)다. 경로 첫 단계는 그 struct의 멤버다. 호출자가 소유한 값이라 고정·재확인이 없고,
//   새로 만드는 빈 자리 값(Optional의 값 등)은 allocator로 만든다. 강한 참조 쓰기는
//   Unsupported(NoPathAccess)이고 Registry 컨테이너를 담은 struct(StructView::
//   ContainsReferenceArray)는 시작 위치가 될 수 없다(Unsupported(NoPathAccess)).
// - Value(상자): PathDetach가 만든 필드·멤버 값 하나다. 경로 첫 단계는 상자에 담은 필드 자신이고
//   (그 FieldId — 분리한 경로의 마지막 단계부터 쓴다), 바깥에서 누적한 유효 플래그를 이어 받는다.
//   그 밖은 struct 값과 같다(새 값은 상자의 allocator).
//
// 사용 예(상자 안의 Array<Slot>에서 원소 멤버 하나를 고친다):
//   DetachedValue box(allocator);
//   auto detached = PathDetach(registry, access, mesh, slotsPath.View(), box);
//   auto written = PathWrite(registry, access, PathTarget::Value(box), slotNamePath.View(), name);
class PathTarget {
public:
    enum class Kind : uint8 {
        Object,
        Staged,
        Struct,
        Detached
    };

    // 게시된 생존 객체다(암묵 변환 — 기존 handle 호출을 그대로 받는다).
    PathTarget(ObjectHandle object) noexcept : kind_(Kind::Object), object_(object) {
    }

    // transaction의 공개 전 객체 object다.
    static PathTarget Staged(ConstructionContext& transaction, ObjectHandle object) noexcept {
        PathTarget target(object);
        target.kind_ = Kind::Staged;
        target.transaction_ = &transaction;
        return target;
    }

    // 등록 struct 값 하나다: lease는 view를 준 lease(그 struct의 lease 또는 그 struct를 필드로 가진
    // 타입의 lease — 상자로 분리하면 복제해 쥔다), storage는 view의 C++ struct 값, allocator는 새로
    // 만드는 빈 자리 값의 allocator다.
    static PathTarget Value(const TypeLease& lease, StructView view, void* storage,
                            Allocator& allocator) noexcept {
        PathTarget target(ObjectHandle{});
        target.kind_ = Kind::Struct;
        target.lease_ = &lease;
        target.view_ = view;
        target.storage_ = storage;
        target.allocator_ = &allocator;
        return target;
    }

    // PathDetach가 채운 상자다.
    static PathTarget Value(DetachedValue& box) noexcept {
        PathTarget target(ObjectHandle{});
        target.kind_ = Kind::Detached;
        target.box_ = &box;
        return target;
    }

    Kind GetKind() const noexcept {
        return kind_;
    }

    ObjectHandle Object() const noexcept {
        return object_;
    }

    ConstructionContext* Transaction() const noexcept {
        return transaction_;
    }

    const TypeLease* Lease() const noexcept {
        return lease_;
    }

    StructView View() const noexcept {
        return view_;
    }

    void* Storage() const noexcept {
        return storage_;
    }

    Allocator* ValueAllocator() const noexcept {
        return allocator_;
    }

    DetachedValue* Box() const noexcept {
        return box_;
    }

    // 이 시작 위치(게시된 객체·staged 객체)에 Registry 컨테이너 준비 사본 stage를 건 사본이다
    // (리플렉션 B9i3c3 — Object/PreparedReferences.h). 경로 끝이
    // ObjectRefArray·ObjectRefMap(원소·항목 포함)이어야 하고, 그 연산은 객체 대신 사본을 읽고
    // 바꾼다(객체는 읽기 Borrow — 변경 표지를 올리지 않는다). 끝이 그 밖이거나 분리된 값이면
    // InvalidArgument다. stage는 연산이 끝날 때까지 살아 있어야 한다.
    //
    // 사용 예:
    //   auto erased = PathRemoveElement(registry, access, PathTarget(mesh).WithReferences(stage),
    //                                   slotPath.View());
    PathTarget WithReferences(PreparedReferences& stage) const noexcept {
        PathTarget target = *this;
        target.references_ = &stage;
        return target;
    }

    PreparedReferences* References() const noexcept {
        return references_;
    }

private:
    Kind kind_;
    ObjectHandle object_{};
    ConstructionContext* transaction_ = nullptr;
    const TypeLease* lease_ = nullptr;
    StructView view_{};
    void* storage_ = nullptr;
    Allocator* allocator_ = nullptr;
    DetachedValue* box_ = nullptr;
    PreparedReferences* references_ = nullptr;
};

} // namespace rocks
