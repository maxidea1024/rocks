#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/InstancedStruct.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/SoftObjectRef.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9k struct 상속 fixture다(Docs/Design/ReflectionExtensions.ko.md 12절).
// InheritBase → InheritMiddle → InheritLeaf는 3단 사슬이다(base 필드를 펼쳐 담고 필드 ID는
// 선언한 struct가 소유한다 — InheritBase는 비리플렉션 빈 base InheritMarker를, InheritLeaf는 CRTP
// 태그를 함께 상속한다). InheritOffset은 InheritMarker를 담은 비리플렉션 빈 base InheritMarked를
// 리플렉션 base 앞에 두어 같은 타입의 두 InheritMarker 하위 객체가 주소를 나눠 갖게 하므로 모든
// ABI에서 InheritBase 하위 객체 오프셋이 0이 아니다. InheritAtomicLeaf는 Atomic base를
// 상속하고(파생에 다시 쓰지 않는다), InheritRefLeaf·InheritRequiredLeaf는 강한 참조·Required 형질을
// 상속한다. InheritOther는 InheritBase와 무관한 struct다. InheritHolder는 상속 struct 필드와
// InstancedStruct<InheritBase>(필드·Array 원소·struct 멤버)를 담는다(StructInheritanceTests·
// TextInstancedTests).

// 비리플렉션 빈 base(CRTP 태그)다.
template <class Derived>
struct InheritTag {};

// 비리플렉션 빈 base(표지)다.
struct InheritMarker {};

// InheritMarker를 담은 비리플렉션 빈 base다.
struct InheritMarked : InheritMarker {};

ROCKS_STRUCT()
struct InheritBase : InheritMarker {
    explicit InheritBase(Allocator& allocator) noexcept : name(allocator), note(allocator) {
    }

    ROCKS_PROPERTY() int32 a = 0;
    ROCKS_PROPERTY() String name;
    // 리플렉션하지 않는 base 멤버다(파생의 깊은 복사가 보존해야 한다 — B9j–B9m 묶음 리뷰 I1).
    ROCKS_NATIVE() uint32 nativeCount = 0;
    ROCKS_NATIVE() String note;
};

ROCKS_STRUCT()
struct InheritMiddle : InheritBase {
    explicit InheritMiddle(Allocator& allocator) noexcept : InheritBase(allocator) {
    }

    ROCKS_PROPERTY() uint32 b = 0;
};

ROCKS_STRUCT()
struct InheritLeaf : InheritMiddle, InheritTag<InheritLeaf> {
    explicit InheritLeaf(Allocator& allocator) noexcept : InheritMiddle(allocator) {
    }

    ROCKS_PROPERTY() float c = 0.0f;
};

ROCKS_STRUCT()
struct InheritOffset : InheritMarked, InheritBase {
    explicit InheritOffset(Allocator& allocator) noexcept : InheritBase(allocator) {
    }

    ROCKS_PROPERTY() int32 d = 0;
};

ROCKS_STRUCT(Atomic)
struct InheritAtomicBase {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

// Atomic은 base에서 상속된다(파생에 다시 쓰지 않는다).
ROCKS_STRUCT()
struct InheritAtomicLeaf : InheritAtomicBase {
    ROCKS_PROPERTY() int32 z = 0;
};

ROCKS_STRUCT()
struct InheritRefBase {
    ROCKS_PROPERTY() ObjectRef<Object> target;
};

// 강한 참조 형질을 base에서 상속한다.
ROCKS_STRUCT()
struct InheritRefLeaf : InheritRefBase {
    ROCKS_PROPERTY() int32 n = 0;
};

ROCKS_STRUCT()
struct InheritRequiredBase {
    explicit InheritRequiredBase(Allocator& allocator) noexcept : asset(allocator) {
    }

    ROCKS_PROPERTY(Required) SoftObjectRef<> asset;
};

// Required 형질을 base에서 상속한다.
ROCKS_STRUCT()
struct InheritRequiredLeaf : InheritRequiredBase {
    explicit InheritRequiredLeaf(Allocator& allocator) noexcept : InheritRequiredBase(allocator) {
    }

    ROCKS_PROPERTY() int32 m = 0;
};

// InheritBase와 무관한 struct다(InstancedStruct<InheritBase>가 거부한다).
ROCKS_STRUCT()
struct InheritOther {
    ROCKS_PROPERTY() int32 q = 0;
};

// InstancedStruct<InheritBase> 멤버를 담는 struct다.
ROCKS_STRUCT()
struct InheritShapeHost {
    ROCKS_PROPERTY() InstancedStruct<InheritBase> shape;
    ROCKS_PROPERTY() uint32 count = 0;
};

// InstancedStruct 멤버를 base에서 상속한다(ContainsInstancedStruct 형질 상속).
ROCKS_STRUCT()
struct InheritShapeChild : InheritShapeHost {
    ROCKS_PROPERTY() int32 extra = 0;
};

// 중첩 struct 멤버를 가진 base다(깊이 2).
ROCKS_STRUCT()
struct InheritNestedBase {
    ROCKS_PROPERTY() InheritOther inner;
};

// 깊이를 base에서 상속한다(2).
ROCKS_STRUCT()
struct InheritNestedLeaf : InheritNestedBase {
    ROCKS_PROPERTY() int32 z = 0;
};

// private 리플렉션 멤버를 가진 base다(파생 생성 코드의 base private 멤버 접근 — 리뷰 S4).
ROCKS_STRUCT()
struct InheritPrivateBase {
    ROCKS_GENERATED_BODY()

public:
    int32 Secret() const noexcept {
        return secret;
    }

    void SetSecret(int32 value) noexcept {
        secret = value;
    }

private:
    ROCKS_PROPERTY() int32 secret = 0;
};

ROCKS_STRUCT()
struct InheritPrivateLeaf : InheritPrivateBase {
    ROCKS_PROPERTY() int32 open = 0;
};

ROCKS_CLASS()
struct InheritHolder final : Object {
    explicit InheritHolder(Allocator& allocator) noexcept
        : leaf(allocator), offset(allocator), shapes(allocator) {
    }

    ROCKS_PROPERTY() InheritLeaf leaf;
    ROCKS_PROPERTY() InheritOffset offset;
    ROCKS_PROPERTY() InheritAtomicLeaf atomic;
    ROCKS_PROPERTY() InheritRefLeaf link;
    ROCKS_PROPERTY() InstancedStruct<InheritBase> shape;
    ROCKS_PROPERTY() Array<InstancedStruct<InheritBase>> shapes;
    ROCKS_PROPERTY() InheritShapeHost host;
};

} // namespace rocks::reflection_fixture
