#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Optional.h>
#include <Core/Text/String.h>
#include <Meta/Annotations.h>
#include <Object/ClassRef.h>
#include <Object/Object.h>
#include <Object/ObjectRefArray.h>
#include <Object/ObjectRefMap.h>
#include <Object/ObjectRefs.h>
#include <Object/StructRefArray.h>

namespace rocks::reflection_fixture {

// 리플렉션 B9i3c1 대입 묶음 fixture다(Docs/Design/ReflectionExtensions.ko.md 10.2절 —
// ReflectionText TextAssignmentTests). AssignHolder가 준비 단위·override 정규화의 모양(단일
// 값·목록·struct 원소 Array·struct 정적 배열·Set·Map·Optional·Atomic struct·중첩 struct)과 ReadOnly
// 필드·멤버, Deprecated 클래스를 가리킬 수 있는 ClassRef, Weak 참조, 값과 강한 참조를 섞은 인라인
// struct, 참조 단위(B9i3c2)의 자리 — ObjectRef 필드(ReadOnly 포함)·정적 배열 원소·StructRefArray
// 원소 멤버, 컨테이너 단위(B9i3c3)의 ObjectRefArray·ObjectRefMap과 그것을 담은 참조 struct를
// 담는다.
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

// 값·ObjectRef·ObjectRefArray·StructRefArray를 함께 담는 참조 struct다(리플렉션 B9i3c3·c4 — `{}`가
// 멤버마다 값 단위·참조 단위·컨테이너 단위·원소 지우기 단위로 나뉜다). Registry 컨테이너를 담으므로
// StructRefArray 원소가 될 수 없다.
ROCKS_STRUCT()
struct AssignBundle {
    explicit AssignBundle(Allocator& allocator) noexcept : crew(allocator), parts(allocator) {
    }

    ROCKS_PROPERTY() int32 level = 0;
    ROCKS_PROPERTY() ObjectRef<AssignTarget> lead;
    ROCKS_PROPERTY() ObjectRefArray<AssignTarget> crew;
    ROCKS_PROPERTY() StructRefArray<AssignMixed> parts;
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
          scores(allocator), byKey(allocator), mixed(allocator), items(allocator), group(allocator),
          named(allocator), bundle(allocator) {
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
    ROCKS_PROPERTY() ObjectRefArray<AssignTarget> group;
    ROCKS_PROPERTY() ObjectRefMap<String, AssignTarget> named;
    ROCKS_PROPERTY() AssignBundle bundle;
};

// struct Map을 담은 struct다(JSON import MergeByKey가 준비 상태를 보는지 — 리플렉션 B9i 리뷰 반영).
ROCKS_STRUCT()
struct AssignShelf {
    explicit AssignShelf(Allocator& allocator) noexcept : byKey(allocator) {
    }

    ROCKS_PROPERTY() OrderedMap<String, AssignPoint> byKey;
};

// 통째 값 대입의 fixture다(리플렉션 B9i 리뷰 반영): ReadOnly 통째 값(목록·항목 목록·정적
// 배열·Optional·struct 원소 배열·struct Map — export → 같은 객체 import 왕복이 무동작인지),
// Deprecated 클래스를 담을 수 있는 ClassRef 컨테이너, struct Map을 담은 struct의 배열·Optional.
ROCKS_CLASS()
struct AssignWholeHolder final : Object {
    explicit AssignWholeHolder(Allocator& allocator) noexcept
        : numbers(allocator), tags(allocator), scores(allocator), points(allocator),
          byKey(allocator), kinds(allocator), kindSet(allocator), kindMap(allocator),
          shelves(allocator) {
    }

    ROCKS_PROPERTY(ReadOnly) Array<int32> numbers;
    ROCKS_PROPERTY(ReadOnly) OrderedSet<String> tags;
    ROCKS_PROPERTY(ReadOnly) OrderedMap<String, int32> scores;
    ROCKS_PROPERTY(ReadOnly) int32 pair[2] = {};
    ROCKS_PROPERTY(ReadOnly) Optional<int32> limit;
    ROCKS_PROPERTY(ReadOnly) Optional<AssignPoint> anchor;
    ROCKS_PROPERTY(ReadOnly) Array<AssignPoint> points;
    ROCKS_PROPERTY(ReadOnly) OrderedMap<String, AssignPoint> byKey;
    ROCKS_PROPERTY() Array<ClassRef<AssignKindBase>> kinds;
    ROCKS_PROPERTY() OrderedSet<ClassRef<AssignKindBase>> kindSet;
    ROCKS_PROPERTY() OrderedMap<String, ClassRef<AssignKindBase>> kindMap;
    ROCKS_PROPERTY() Array<AssignShelf> shelves;
    ROCKS_PROPERTY() Optional<AssignShelf> spare;
};

} // namespace rocks::reflection_fixture
