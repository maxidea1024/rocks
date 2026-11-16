#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9i3c1 대입 묶음 fixture다(Docs/Design/ReflectionExtensions.ko.md 10.2절 —
// ReflectionText TextAssignmentTests). AssignHolder가 준비 단위·override 정규화의 모양(단일
// 값·목록·struct 원소 Array·struct 정적 배열·Set·Map·Optional·Atomic struct·중첩 struct)과 ReadOnly
// 필드·멤버, Deprecated 클래스를 가리킬 수 있는 ClassRef, Weak 참조, 값과 강한 참조를 섞은 인라인
// struct, 참조 단위(B9i3c2)의 자리 — ObjectRef 필드(ReadOnly 포함)·정적 배열 원소·StructRefArray
// 원소 멤버를 담는다.
ROCKS_STRUCT()
struct AssignPoint {
    ROCKS_PROPERTY() int32 x = 0;
    ROCKS_PROPERTY() int32 y = 0;
};

ROCKS_STRUCT(Atomic)
struct AssignAtom {
    ROCKS_PROPERTY() int32 a = 0;
    ROCKS_PROPERTY() int32 b = 0;
};

ROCKS_STRUCT()
struct AssignInner {
    ROCKS_PROPERTY() int32 value = 0;
    ROCKS_PROPERTY(ReadOnly) int32 fixed = 0;
    ROCKS_PROPERTY() AssignPoint point;
};

ROCKS_CLASS()
struct AssignTarget final : Object {};

ROCKS_STRUCT()
struct AssignMixed {
    explicit AssignMixed(Allocator& allocator) noexcept : note(allocator) {
    }

    ROCKS_PROPERTY() int32 weight = 0;
    ROCKS_PROPERTY() String note;
    ROCKS_PROPERTY() ObjectRef<AssignTarget> link;
};

ROCKS_CLASS(Abstract)
struct AssignKindBase : Object {};

ROCKS_CLASS()
struct AssignKindCurrent final : AssignKindBase {};

ROCKS_CLASS(Deprecated)
struct AssignKindOld final : AssignKindBase {};

ROCKS_CLASS()
struct AssignHolder final : Object {
    explicit AssignHolder(Allocator& allocator) noexcept
        : label(allocator), numbers(allocator), points(allocator), tags(allocator),
          scores(allocator), byKey(allocator), mixed(allocator), items(allocator) {
    }

    ROCKS_PROPERTY() int32 count = 0;
    ROCKS_PROPERTY() String label;
    ROCKS_PROPERTY() Array<int32> numbers;
    ROCKS_PROPERTY() Array<AssignPoint> points;
    ROCKS_PROPERTY() AssignPoint pair[2] = {};
    ROCKS_PROPERTY() OrderedSet<String> tags;
    ROCKS_PROPERTY() OrderedMap<String, int32> scores;
    ROCKS_PROPERTY() OrderedMap<String, AssignPoint> byKey;
    ROCKS_PROPERTY() Optional<int32> limit;
    ROCKS_PROPERTY() Optional<AssignPoint> anchor;
    ROCKS_PROPERTY() AssignAtom atom;
    ROCKS_PROPERTY() AssignInner inner;
    ROCKS_PROPERTY(ReadOnly) int32 locked = 0;
    ROCKS_PROPERTY(ReadOnly) AssignPoint sealed;
    ROCKS_PROPERTY() ClassRef<AssignKindBase> kind;
    ROCKS_PROPERTY() WeakRef<AssignTarget> peer;
    ROCKS_PROPERTY() AssignMixed mixed;
    ROCKS_PROPERTY() ObjectRef<AssignTarget> next;
    ROCKS_PROPERTY(ReadOnly) ObjectRef<AssignTarget> anchorLink;
    ROCKS_PROPERTY() ObjectRef<AssignTarget> slots[2];
    ROCKS_PROPERTY() StructRefArray<AssignMixed> items;
};

} // namespace rocks::reflection_fixture
